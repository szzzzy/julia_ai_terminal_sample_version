/**
 * @file julia_night_schedule.c
 * @brief Turns trusted RTC/SNTP wall time into FSM night/wake events.
 *
 * The RTC is restored into the system wall clock by julia_time.  This module
 * therefore reads localtime() rather than touching the PCF85063 bus directly.
 * State transition semantics are intentionally deferred to julia_fsm.c. This
 * module only emits the existing time events when an idle-class state is seen.
 */
#include "julia_night_schedule.h"

#include <stdbool.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_fsm_runtime.h"
#include "julia_idle_display.h"
#include "julia_time.h"
#include "sdkconfig.h"

#define NIGHT_SCHEDULE_TASK_STACK_SIZE 3072
#define NIGHT_SCHEDULE_TASK_PRIORITY   2
#define NIGHT_SCHEDULE_POLL_MS         5000
#define NIGHT_RESUME_DELAY_MS          300000
#define INVALID_HOUR                   (-1)

static const char *TAG = "NIGHT_SCHEDULE";
static TaskHandle_t s_task;

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

static void night_schedule_task(void *argument)
{
    (void)argument;
    bool schedule_owns_sleep = false;
    bool time_was_valid = false;
    int last_hour = INVALID_HOUR;
    int64_t sleep_deadline_us = 0;

    for (;;) {
        if (!julia_time_valid()) {
            if (time_was_valid) ESP_LOGW(TAG, "wall clock became invalid; schedule paused");
            time_was_valid = false;
            last_hour = INVALID_HOUR;
            sleep_deadline_us = 0;
            vTaskDelay(pdMS_TO_TICKS(NIGHT_SCHEDULE_POLL_MS));
            continue;
        }

        time_t now = time(NULL);
        struct tm local = {0};
        localtime_r(&now, &local);
        const bool night = hour_is_night(local.tm_hour);
        const bool bedtime = !night && local.tm_hour == 22;
        julia_main_state_t state = julia_fsm_runtime_get_state();

        if (!time_was_valid || local.tm_hour != last_hour) {
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
                int64_t now_us = esp_timer_get_time();
                if (sleep_deadline_us == 0) {
                    sleep_deadline_us = now_us + (int64_t)NIGHT_RESUME_DELAY_MS * 1000LL;
                    ESP_LOGI(TAG, "night idle grace started: %dms", NIGHT_RESUME_DELAY_MS);
                } else if (now_us >= sleep_deadline_us) {
                    post_event(EVT_NIGHT_TIME);
                    schedule_owns_sleep = true;
                    sleep_deadline_us = 0;
                }
            } else {
                /* Active states do not start the night-idle grace period. */
                sleep_deadline_us = esp_timer_get_time() +
                                    (int64_t)NIGHT_RESUME_DELAY_MS * 1000LL;
            }
        } else if (schedule_owns_sleep) {
            if (state == JULIA_MAIN_STATE_S6_SLEEP) {
                julia_idle_display_note_activity();
                post_event(EVT_WAKEUP);
            }
            schedule_owns_sleep = false;
            sleep_deadline_us = 0;
        } else if (bedtime) {
            if (state == JULIA_MAIN_STATE_S1_COMPANION) {
                sleep_deadline_us = 0;
            } else if (state == JULIA_MAIN_STATE_S3_STANDBY ||
                       state == JULIA_MAIN_STATE_S5_SILENT) {
                int64_t now_us = esp_timer_get_time();
                if (sleep_deadline_us == 0) {
                    sleep_deadline_us = now_us + (int64_t)NIGHT_RESUME_DELAY_MS * 1000LL;
                } else if (now_us >= sleep_deadline_us) {
                    post_event(EVT_BEDTIME);
                    sleep_deadline_us = 0;
                }
            } else {
                sleep_deadline_us = esp_timer_get_time() +
                                    (int64_t)NIGHT_RESUME_DELAY_MS * 1000LL;
            }
        } else {
            sleep_deadline_us = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(NIGHT_SCHEDULE_POLL_MS));
    }
}

esp_err_t julia_night_schedule_init(void)
{
    if (s_task != NULL) return ESP_OK;
    if (xTaskCreate(night_schedule_task, "night_schedule",
                    NIGHT_SCHEDULE_TASK_STACK_SIZE, NULL,
                    NIGHT_SCHEDULE_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready window=%02d:00-%02d:00 resume=%dms poll=%dms",
             CONFIG_JULIA_NIGHT_SLEEP_START_HOUR,
             CONFIG_JULIA_NIGHT_SLEEP_END_HOUR,
             NIGHT_RESUME_DELAY_MS,
             NIGHT_SCHEDULE_POLL_MS);
    return ESP_OK;
}
