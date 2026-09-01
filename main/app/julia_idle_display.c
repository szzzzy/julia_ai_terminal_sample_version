/**
 * @file    julia_idle_display.c
 * @brief   待机显示策略实现：陪伴常驻，十分钟无交互后进入 S3 待机。
 *
 * 职责边界：
 *   - 依据“距最近交互的时长 + busy 标志”判断 S1 是否应进入 S3，并投递
 *     EVT_USER_LEAVE；调试阶段不在这里切换立绘，具体状态呈现统一由 FSM 运行时负责。
 *   - note_activity()/set_busy() 仍记录活动并可恢复被 S6 等状态覆盖的显示，但不会
 *     冒充唤醒词改变行为状态。
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

/* 闲置策略两档：活跃陪伴，以及已经投递 S3 的待机档。 */
typedef enum {
    DISPLAY_ACTIVITY_ACTIVE = 0,   ///< 活跃：立绘睁眼、背光 100%。
    DISPLAY_ACTIVITY_STANDBY,      ///< 已达到 S3 待机阈值，等待新活动。
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
 * 达到 S3 待机阈值后投递一次用户离开事件。函数不直接修改立绘和背光，
 * 避免调试阶段 Companion 共用 UI 与闲置策略互相覆盖。
 */
static void display_enter_standby(uint32_t generation)
{
    if (!transition_is_current(DISPLAY_ACTIVITY_STANDBY, generation)) return;
    /* 普通空闲达到阈值后投递现有用户离开事件，使 S1 进入 S3 待机；
     * 夜间调度仍单独负责投递进入 S6 睡眠的事件。 */
    post_fsm_event(EVT_USER_LEAVE);
    ESP_LOGI(TAG, "closed-eye breathing after %d seconds",
             CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS);
}

/*
 * 后台轮询任务：周期性检查"闲置时长"，推动显示档位。优先级 3、栈 3072B。
 * 每次轮询先读一份共享快照（在锁内拷贝），再据此判断，避免长时间持锁。
 * 推进规则：空闲达到阈值、当前不是 STANDBY 且不忙时投递一次 S1→S3；
 * 阈值前保持 ACTIVE，处于 STANDBY 时不重复投递。
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

        if (!busy && state != DISPLAY_ACTIVITY_STANDBY) {
            int64_t idle_us = now_us - last_activity_us;
            if (idle_us >= sleep_us) {
                bool enter = false;
                uint32_t generation = 0;
                /* 申请进入 standby：锁内再校验一次（可能上次快照已过期），并原子地
                 * 更新 state、抬 generation。 */
                portENTER_CRITICAL(&s_lock);
                if (!s_busy && s_state != DISPLAY_ACTIVITY_STANDBY) {
                    s_state = DISPLAY_ACTIVITY_STANDBY;
                    generation = ++s_generation;
                    enter = true;
                }
                portEXIT_CRITICAL(&s_lock);
                if (enter) {
                    display_enter_standby(generation);
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
        /* 普通显示活动只恢复画面，不冒充唤醒词改变行为状态。 */
        ESP_LOGI(TAG, "activity restored display; FSM still waits for wake word");
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
        /* busy 只控制显示降档；行为状态仍由语音链路的唤醒词事件推进。 */
    }
}

/*
 * @brief 查询闲置策略是否已经投递 S3 待机。
 * @return true 表示当前处于 DISPLAY_ACTIVITY_STANDBY。
 * @note 保留旧接口名以避免破坏调用方；返回值不再等同于实际闭眼或 S6 睡眠。
 */
bool julia_idle_display_is_sleeping(void)
{
    bool sleeping;
    portENTER_CRITICAL(&s_lock);
    sleeping = s_state == DISPLAY_ACTIVITY_STANDBY;
    portEXIT_CRITICAL(&s_lock);
    return sleeping;
}
