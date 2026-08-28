/**
 * @file    julia_idle_display.c
 * @brief   待机显示策略实现：陪伴常驻，五分钟无交互后进入远场待机。
 *
 * 职责边界：
 *   - 唯一职责是依据"距最近交互的时长 + busy 标志"推进显示档位，并在每档切换时
 *     设置立绘 dozing 与背光。它不读取 FSM 状态、不参与语音逻辑、不绘制立绘内容。
 *   - 与 LVGL 刷新配合：它只调用 julia_avatar_set_dozing 与 julia_backlight_* ，
 *     具体 LVGL 绘制由 julia_avatar/julia_ui 处理；这里保证"先令立绘闭眼、再启动背光
 *     呼吸、若状态已变则回滚"的一致性。
 *
 * 并发模型：
 *   - 有一个后台任务（display_theme_task）轮询推进状态；用户/语音侧通过
 *     note_activity()/set_busy() 更新共享状态。所有共享状态（s_last_activity_us、
 *     s_busy、s_state、s_generation）都在 s_lock（portMUX）临界区内读写。
 *   - 用 generation 计数器做"关卡约定"：任务在推进到某档前先记账，执行时再校验
 *     自己仍然是最新的一档（transition_is_current），避免与并发唤醒竞争——
 *     若唤醒抢先加了 generation 并改了 state，旧推进就会检测到不匹配而回滚 restore。
 */
#include "julia_idle_display.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_backlight.h"
#include "julia_avatar.h"
#include "julia_fsm_runtime.h"
#include "sdkconfig.h"

#define DISPLAY_THEME_TASK_STACK_SIZE 3072
#define DISPLAY_THEME_TASK_PRIORITY   3

/* 显示闲置两档：日常保持陪伴显示，达到长休阈值才进入远场待机。 */
typedef enum {
    DISPLAY_ACTIVITY_ACTIVE = 0,   ///< 活跃：立绘睁眼、背光 100%。
    DISPLAY_ACTIVITY_SLEEP,        ///< 睡眠：立绘闭眼 + 背光低暗呼吸。
} display_activity_state_t;

static const char *TAG = "DISPLAY_THEME";
static TaskHandle_t s_task;                             /* 后台轮询任务句柄（用于判重/防重复 init）。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED; /* 保护下面前四个共享字段的临界区锁。 */
static int64_t s_last_activity_us;                      /* 最近一次用户/语音交互时刻（us）。 */
static bool s_busy;                                     /* 当前是否处于听-想-说的"占屏"期。 */
static display_activity_state_t s_state = DISPLAY_ACTIVITY_ACTIVE; /* 当前显示档位。 */
static uint32_t s_generation;                            /* 档位代次：每次状态变化自增，用于并发赶超校验。 */

static void post_fsm_event(fsm_event_t event)
{
    esp_err_t err = julia_fsm_runtime_post(event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FSM event %s rejected: %s", julia_fsm_event_name(event),
                 esp_err_to_name(err));
    }
}

/*
 * 判断"要执行的某档推进是否仍是最新"。
 * 条件：共享状态恰好等于该档，且代次未变。若唤醒/置忙抢先改了状态和代次，
 * 则这里返回 false，调用方应放弃本次视觉设定或回滚。
 * 在持锁下读取，保证与写方的一致性视图。
 */
static bool transition_is_current(display_activity_state_t state, uint32_t generation)
{
    bool current;
    portENTER_CRITICAL(&s_lock);
    current = s_state == state && s_generation == generation;
    portEXIT_CRITICAL(&s_lock);
    return current;
}

/*
 * 恢复到"完全活跃"的视觉：停止背光呼吸、背光拉满、立绘睁眼。
 * 作为所有"被唤醒/回滚"时的统一还原动作。
 */
static void display_restore(void)
{
    julia_backlight_breathe_stop();
    julia_backlight_set(100);
    julia_avatar_set_dozing(false);
}

/*
 * 进入"睡眠"档：立绘闭眼 + 背光进入低暗呼吸（用配置的最小/最大亮度与周期）。
 * 呼吸启动失败只打警告、不阻断进入睡眠（视觉上仍闭眼，只是没有呼吸动画）。
 * 同样带两层 generation 校验，捕获与唤醒的竞争。
 */
static void display_enter_sleep(uint32_t generation)
{
    if (!transition_is_current(DISPLAY_ACTIVITY_SLEEP, generation)) return;
    julia_avatar_set_dozing(true);
    esp_err_t err = julia_backlight_breathe_start(
        CONFIG_JULIA_DISPLAY_BREATHE_MIN_PERCENT,
        CONFIG_JULIA_DISPLAY_BREATHE_MAX_PERCENT,
        CONFIG_JULIA_DISPLAY_BREATHE_PERIOD_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "idle breathing start failed: %s", esp_err_to_name(err));
    }
    if (!transition_is_current(DISPLAY_ACTIVITY_SLEEP, generation)) {
        display_restore();
        return;
    }
    /* Five ordinary idle minutes mean far standby (S1.2), not night sleep.
     * The RTC-backed night scheduler is the only time-based S0.1 source. */
    post_fsm_event(EVT_USER_LEAVE);
    ESP_LOGI(TAG, "closed-eye breathing after %d seconds",
             CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS);
}

/*
 * 后台轮询任务：周期性检查"闲置时长"，推动显示档位。优先级 3、栈 3072B。
 * 每次轮询先读一份共享快照（在锁内拷贝），再据此判断，避免长时间持锁。
 * 推进规则：空闲 >= sleep 阈值 且当前不是 sleep 且不忙 -> 进入远场待机
 * （闭眼+呼吸）。阈值前始终保持 ACTIVE 陪伴显示，不再经过短暂停档。
 * 处于 SLEEP 时即使继续空闲也不重复推进。
 *   忙（busy）时一律不推进 —— 正在听/想/说，屏幕必须保持活跃。
 * 每次进入之前都在临界区内"按当前快照重新确认 + 更新 state + 提升 generation"，
 * 保证与并发 note_activity/set_busy 的竞态能被 generation 机制察觉。
 */
static void display_theme_task(void *argument)
{
    (void)argument;
    const int64_t sleep_us =
        (int64_t)CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS * 1000000LL;

    for (;;) {
        int64_t now_us = esp_timer_get_time();
        int64_t last_activity_us;
        bool busy;
        display_activity_state_t state;

        portENTER_CRITICAL(&s_lock);
        last_activity_us = s_last_activity_us;
        busy = s_busy;
        state = s_state;
        portEXIT_CRITICAL(&s_lock);

        if (!busy && state != DISPLAY_ACTIVITY_SLEEP) {
            int64_t idle_us = now_us - last_activity_us;
            if (idle_us >= sleep_us) {
                bool enter = false;
                uint32_t generation = 0;
                /* 申请进入 sleep：锁内再校验一次（可能上次快照已过期），并原子地
                 * 更新 state、抬 generation。 */
                portENTER_CRITICAL(&s_lock);
                if (!s_busy && s_state != DISPLAY_ACTIVITY_SLEEP) {
                    s_state = DISPLAY_ACTIVITY_SLEEP;
                    generation = ++s_generation;
                    enter = true;
                }
                portEXIT_CRITICAL(&s_lock);
                if (enter) {
                    display_enter_sleep(generation);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_JULIA_DISPLAY_ACTIVITY_POLL_MS));
    }
}

/*
 * @brief 初始化待机显示策略并启动后台轮询任务。
 * @return ESP_OK（含已初始化过的幂等情形）；ESP_ERR_NO_MEM（任务创建失败）。
 * @side  将初始状态置为 ACTIVE、代次=1，并恢复一次显示（背光满、睁眼、停呼吸）。
 *
 * 幂等：若 s_task 已非空直接返回 ESP_OK，避免重复启动任务（重复 init 安全）。
 * 初值把 s_last_activity_us 设为当前时刻，避免"上电即闭眼/睡眠"。
 */
esp_err_t julia_idle_display_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_lock);
    s_last_activity_us = esp_timer_get_time();
    s_busy = false;
    s_state = DISPLAY_ACTIVITY_ACTIVE;
    s_generation = 1;
    portEXIT_CRITICAL(&s_lock);

    display_restore();
    if (xTaskCreate(display_theme_task, "display_idle", DISPLAY_THEME_TASK_STACK_SIZE,
                    NULL, DISPLAY_THEME_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready companion->far-standby=%ds poll=%dms",
             CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS,
             CONFIG_JULIA_DISPLAY_ACTIVITY_POLL_MS);
    return ESP_OK;
}

/*
 * @brief 记录一次有效交互，并把显示唤醒回 ACTIVE。
 * 调用上下文：任何用户/语音交互点（如唤醒词、语音会话、按键）。
 * 副作用：刷新 last_activity、置 ACTIVE、抬 generation；若之前处于 sleep
 *         则调用 display_restore() 恢复睁眼+满背光。
 * 并发：在锁内更新共享状态，抬 generation 使可能正在进行的"进入 sleep"推进失效。
 */
void julia_idle_display_note_activity(void)
{
    display_activity_state_t previous;
    portENTER_CRITICAL(&s_lock);
    s_last_activity_us = esp_timer_get_time();
    previous = s_state;
    s_state = DISPLAY_ACTIVITY_ACTIVE;
    ++s_generation;
    portEXIT_CRITICAL(&s_lock);

    if (previous != DISPLAY_ACTIVITY_ACTIVE) {
        display_restore();
        post_fsm_event(EVT_WAKEUP);
        ESP_LOGI(TAG, "activity restored display from far standby");
    }
}

/*
 * @brief 设置"独占期"标志：听-想-说期间保持屏幕活跃。
 * @param busy true 进入占屏期（不推进降档），false 结束占屏期。
 * 副作用：同 note_activity——刷新 last_activity、置 ACTIVE、抬 generation，
 *         若非 ACTIVE 则恢复显示。用于防止"Julia 正在说话时屏幕却被闲置降档闭眼"。
 */
void julia_idle_display_set_busy(bool busy)
{
    display_activity_state_t previous;
    portENTER_CRITICAL(&s_lock);
    s_busy = busy;
    s_last_activity_us = esp_timer_get_time();
    previous = s_state;
    s_state = DISPLAY_ACTIVITY_ACTIVE;
    ++s_generation;
    portEXIT_CRITICAL(&s_lock);

    if (previous != DISPLAY_ACTIVITY_ACTIVE) {
        display_restore();
        post_fsm_event(EVT_WAKEUP);
    }
}

/*
 * @brief 查询是否已进入"闭眼 + 背光呼吸"的睡眠档。
 * @return true 表示当前处于 DISPLAY_ACTIVITY_SLEEP。
 * 供调用方（如要避免在睡眠态做某些视觉操作）读取；只读共享状态，不加视觉副作用。
 */
bool julia_idle_display_is_sleeping(void)
{
    bool sleeping;
    portENTER_CRITICAL(&s_lock);
    sleeping = s_state == DISPLAY_ACTIVITY_SLEEP;
    portEXIT_CRITICAL(&s_lock);
    return sleeping;
}
