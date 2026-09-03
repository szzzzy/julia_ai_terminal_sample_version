/**
 * @file    julia_idle_display.c
 * @brief   实现“交流结束后保留一段免唤醒时间，超时后恢复等待唤醒”的策略。
 *
 * 职责边界：
 *   - 最近一次有效交流超过设定时长，且设备没有在听音、等待回答或播放回答时，
 *     报告用户已经离开，设备由免唤醒陪伴返回等待唤醒。
 *   - 本模块只维护时间与“当前交流是否仍在进行”，不直接操作面板、背光或立绘；
 *     睡眠状态生效后不会被空闲计时逻辑从旁路重新点亮。
 *
 * 并发模型：
 *   - 后台任务定期检查时间；语音处理只负责报告新活动或交流开始/结束。
 *   - 每次活动都会更新一个变化编号。后台任务准备报告超时时会再次核对编号；
 *     如果期间出现新活动，就放弃已经过时的超时结果。
 */
#include "julia_idle_display.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_fsm_runtime.h"
#include "sdkconfig.h"

#define DISPLAY_THEME_TASK_STACK_SIZE 3072
#define DISPLAY_THEME_TASK_PRIORITY   3

/* 该状态只表示陪伴计时是否仍可到期，不承载立绘或背光语义。 */
typedef enum {
    DISPLAY_ACTIVITY_ACTIVE = 0,
    DISPLAY_ACTIVITY_STANDBY,
} display_activity_state_t;

static const char *TAG = "DISPLAY_THEME";
static TaskHandle_t s_task;
/* 下列状态均由 s_lock 保护；时间来自 esp_timer 单调时钟，单位为 us。
 * generation 使锁外计算结果在新活动到达后失效。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_activity_us;
static bool s_busy;
static display_activity_state_t s_state = DISPLAY_ACTIVITY_STANDBY;
static uint32_t s_generation;

static void post_fsm_event(fsm_event_t event)
{
    esp_err_t err = julia_fsm_runtime_post(event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FSM event %s rejected: %s", julia_fsm_event_name(event),
                 esp_err_to_name(err));
    }
}

/*
 * 确认一次准备执行的超时判断仍然有效。
 * 如果用户刚刚开始交流，或语音处理刚刚声明设备正在忙碌，活动变化编号就会更新，
 * 旧判断必须放弃，避免把正在交流的设备误送回待机。
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
 * 陪伴时间耗尽后只报告用户离开。设备状态管理随后统一切换表情和背光，
 * 本模块不直接改变画面。
 */
static void display_enter_standby(uint32_t generation)
{
    if (!transition_is_current(DISPLAY_ACTIVITY_STANDBY, generation)) return;
    post_fsm_event(EVT_USER_LEAVE);
    ESP_LOGI(TAG, "closed-eye breathing after %d seconds",
             CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS);
}

/*
 * 后台任务定期检查免唤醒陪伴时间。正在交流时不计超时；已经回到等待唤醒后
 * 不重复报告。真正提交超时前会再次确认期间没有新活动。
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
 * @side  初始状态表示尚未进入陪伴窗口；具体待机画面随后由设备状态管理应用。
 *
 * 重复调用不会创建第二个计时任务。启动时不报告用户离开，因为设备本来就在待机。
 */
esp_err_t julia_idle_display_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_lock);
    s_last_activity_us = esp_timer_get_time();
    s_busy = false;
    s_state = DISPLAY_ACTIVITY_STANDBY;
    s_generation = 1;
    portEXIT_CRITICAL(&s_lock);

    if (xTaskCreate(display_theme_task, "display_idle", DISPLAY_THEME_TASK_STACK_SIZE,
                    NULL, DISPLAY_THEME_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready default=standby companion-window=%ds poll=%dms",
             CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS,
             CONFIG_JULIA_DISPLAY_ACTIVITY_POLL_MS);
    return ESP_OK;
}

/*
 * @brief 记录一次有效交流，并从现在重新计算免唤醒陪伴时间。
 * 唤醒词、用户话语和有效按键都属于活动；本函数不直接改变显示。
 */
void julia_idle_display_note_activity(void)
{
    portENTER_CRITICAL(&s_lock);
    s_last_activity_us = esp_timer_get_time();
    s_state = DISPLAY_ACTIVITY_ACTIVE;
    ++s_generation;
    portEXIT_CRITICAL(&s_lock);
}

/*
 * @brief 声明当前交流是否仍在进行。
 * @param busy true 表示正在听音、等待回答或播放回答，不允许陪伴窗口超时；
 *             false 表示本轮处理结束，从当前时刻重新计算时间。
 * 本函数只维护计时条件，实际显示由设备状态统一决定。
 */
void julia_idle_display_set_busy(bool busy)
{
    portENTER_CRITICAL(&s_lock);
    s_busy = busy;
    s_last_activity_us = esp_timer_get_time();
    if (busy) s_state = DISPLAY_ACTIVITY_ACTIVE;
    ++s_generation;
    portEXIT_CRITICAL(&s_lock);
}

/*
 * @brief 查询免唤醒陪伴窗口是否已经结束。
 * @return true 表示后续交流需要重新经过唤醒流程。
 * @note 接口名为兼容旧调用保留；返回值不代表屏幕或整机已经睡眠。
 */
bool julia_idle_display_is_sleeping(void)
{
    bool sleeping;
    portENTER_CRITICAL(&s_lock);
    sleeping = s_state == DISPLAY_ACTIVITY_STANDBY;
    portEXIT_CRITICAL(&s_lock);
    return sleeping;
}
