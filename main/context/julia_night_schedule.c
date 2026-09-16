/**
 * @file julia_night_schedule.c
 * @brief 根据可信的本地时间决定设备何时进入夜间睡眠，而不打断正在进行的交流。
 *
 * 时钟模块负责从 RTC 恢复时间并通过网络校准；这里仅判断当前是否处于夜间。
 * 设备正在听音、等待回答或播放回答时延后睡眠；陪伴、待机或静默达到夜间宽限后
 * 才请求睡眠。早晨只恢复显示条件，设备仍需唤醒词才开始交流。
 *
 * 时间基准是本地时间（localtime_r，时区由 julia_time.c 按 CONFIG_JULIA_TIMEZONE 设置），
 * 判定粒度是整点：窗口为 [START_HOUR, END_HOUR)，允许跨午夜，START==END 表示不进入夜间。
 * julia_time_valid() 为假时不做任何判断，只等待 notify，避免用未校准的时钟误触发睡眠。
 */
#include "julia_night_schedule.h"

#include <stdbool.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_fsm_runtime.h"
#include "julia_time.h"
#include "sdkconfig.h"

#if CONFIG_JULIA_NIGHT_SLEEP_ENABLE

/* 任务栈与优先级是模块内常量，仓库里没有对应配置项，来源未确认。 */
#define NIGHT_SCHEDULE_TASK_STACK_SIZE 3072
#define NIGHT_SCHEDULE_TASK_PRIORITY   2
#define INVALID_HOUR                   (-1)

static const char *TAG = "NIGHT_SCHEDULE";
static TaskHandle_t s_task;

/* 调用者：FSM 状态观察者（voice_service_on_fsm_state，每次状态变化后）与 SNTP 同步回调
 * （校时后按新时间重算）。两者都在任务上下文；实现使用 xTaskNotifyGive，因此不得从
 * ISR 调用。任务尚未创建时静默忽略，调度会在 init 后按整点自行恢复判断。 */
void julia_night_schedule_notify(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

/* 只按小时比较：窗口含起点、不含终点，跨午夜时拆成两段判断；start==end 视为
 * 没有夜间窗口，比按"全天夜间"解释更安全。 */
static bool hour_is_night(int hour)
{
    const int start = CONFIG_JULIA_NIGHT_SLEEP_START_HOUR;
    const int end = CONFIG_JULIA_NIGHT_SLEEP_END_HOUR;
    if (start == end) return false;
    return start < end ? (hour >= start && hour < end)
                       : (hour >= start || hour < end);
}

static void post_event(fsm_event_t event)
{
    esp_err_t err = julia_fsm_runtime_post(event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FSM event %s rejected: %s", julia_fsm_event_name(event),
                 esp_err_to_name(err));
    }
}

/* 调度只有两条触发源：整点到的定时唤醒，以及 notify 提前唤醒（状态变化或校时）。
 * sleep_deadline_us 用 esp_timer 单调时钟计时，跨整点不会被系统时间跳变影响。 */
static void night_schedule_task(void *argument)
{
    (void)argument;
    bool schedule_owns_sleep = false;
    bool time_was_valid = false;
    int last_hour = INVALID_HOUR;
    int64_t sleep_deadline_us = 0;

    for (;;) {
        if (!julia_time_valid()) {
            /* 时间不可信时既不判断夜间也不保留宽限计时；恢复可信后重新开始计时。 */
            if (time_was_valid) ESP_LOGW(TAG, "wall clock became invalid; schedule paused");
            time_was_valid = false;
            last_hour = INVALID_HOUR;
            sleep_deadline_us = 0;
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        time_t now = time(NULL);
        struct tm local = {0};
        localtime_r(&now, &local);
        const bool night = hour_is_night(local.tm_hour);
        const bool bedtime = !night && local.tm_hour == 22;
        julia_main_state_t state = julia_fsm_runtime_get_state();

        if (!time_was_valid || local.tm_hour != last_hour) {
            /* 每小时最多一条状态日志，便于对照夜间窗口与当前 FSM 状态。 */
            ESP_LOGI(TAG, "local=%02d:%02d bedtime=%u night=%u state=%s", local.tm_hour,
                     local.tm_min, bedtime ? 1U : 0U, night ? 1U : 0U,
                     julia_fsm_main_state_name(state));
            last_hour = local.tm_hour;
        }
        time_was_valid = true;

        if (night) {
            if (state == JULIA_MAIN_STATE_S6_SLEEP) {
                sleep_deadline_us = 0;
            } else if (state == JULIA_MAIN_STATE_S1_COMPANION ||
                       state == JULIA_MAIN_STATE_S3_STANDBY ||
                       state == JULIA_MAIN_STATE_S5_SILENT) {
                /* 宽限只在 S1/S3/S5 计时；对话、故障、升级期间不计时也不打断。 */
                int64_t now_us = esp_timer_get_time();
                if (sleep_deadline_us == 0) {
                    sleep_deadline_us = now_us +
                                        (int64_t)CONFIG_JULIA_NIGHT_SLEEP_GRACE_SECONDS *
                                            1000000LL;
                    ESP_LOGI(TAG, "night idle grace started: %ds",
                             CONFIG_JULIA_NIGHT_SLEEP_GRACE_SECONDS);
                } else if (now_us >= sleep_deadline_us) {
                    post_event(EVT_NIGHT_TIME);
                    schedule_owns_sleep = true;
                    sleep_deadline_us = 0;
                }
            } else {
                /* 活跃状态不开始夜间空闲宽限；返回空闲状态后重新完整计时。 */
                sleep_deadline_us = 0;
            }
        } else if (schedule_owns_sleep) {
            /* 夜间窗口结束不旁路覆盖 S6 呈现；仍由唤醒词驱动 S6 -> S4。 */
            schedule_owns_sleep = false;
            sleep_deadline_us = 0;
        } else if (bedtime) {
            /* 22 点整点（非夜间窗口）是睡前提醒的入口：S1 说明用户仍在陪伴期，不主动
             * 催睡；只有 S3/S5 这类空闲状态才在宽限到期后投递 EVT_BEDTIME。 */
            if (state == JULIA_MAIN_STATE_S1_COMPANION) {
                sleep_deadline_us = 0;
            } else if (state == JULIA_MAIN_STATE_S3_STANDBY ||
                       state == JULIA_MAIN_STATE_S5_SILENT) {
                int64_t now_us = esp_timer_get_time();
                if (sleep_deadline_us == 0) {
                    sleep_deadline_us = now_us +
                                        (int64_t)CONFIG_JULIA_NIGHT_SLEEP_GRACE_SECONDS *
                                            1000000LL;
                } else if (now_us >= sleep_deadline_us) {
                    post_event(EVT_BEDTIME);
                    sleep_deadline_us = 0;
                }
            } else {
                sleep_deadline_us = 0;
            }
        } else {
            sleep_deadline_us = 0;
        }

        /* 下一整点覆盖夜间/睡前边界；校时和状态变化提前唤醒并重算。 */
        struct tm boundary = local;
        boundary.tm_hour += 1;
        boundary.tm_min = boundary.tm_sec = 0;
        boundary.tm_isdst = -1;
        time_t next = mktime(&boundary);
        int64_t wait_ms = next > now ? (int64_t)(next - now) * 1000 : 1000;
        if (sleep_deadline_us) {
            int64_t grace_ms = (sleep_deadline_us - esp_timer_get_time() + 999) / 1000;
            if (grace_ms < wait_ms) wait_ms = grace_ms;
        }
        if (wait_ms < 1) wait_ms = 1;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms) ? pdMS_TO_TICKS(wait_ms) : 1);
    }
}

/* 幂等：已创建任务直接返回 ESP_OK。日志里的 poll 字段取自 CONFIG_JULIA_NIGHT_SCHEDULE_POLL_MS，
 * 但等待时长实际由“下一个整点”和宽限截止时间决定，该配置当前不影响调度节奏。 */
esp_err_t julia_night_schedule_init(void)
{
    if (s_task != NULL) return ESP_OK;
    if (xTaskCreate(night_schedule_task, "night_schedule",
                    NIGHT_SCHEDULE_TASK_STACK_SIZE, NULL,
                    NIGHT_SCHEDULE_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready window=%02d:00-%02d:00 grace=%ds poll=%dms",
             CONFIG_JULIA_NIGHT_SLEEP_START_HOUR,
             CONFIG_JULIA_NIGHT_SLEEP_END_HOUR,
             CONFIG_JULIA_NIGHT_SLEEP_GRACE_SECONDS,
             CONFIG_JULIA_NIGHT_SCHEDULE_POLL_MS);
    return ESP_OK;
}
#else
void julia_night_schedule_notify(void) { }
esp_err_t julia_night_schedule_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
