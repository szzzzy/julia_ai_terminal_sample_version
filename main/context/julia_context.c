#include "julia_context.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "PCF85063.h"
#include "QMI8658.h"
#include "julia_memory.h"
#include "julia_routine.h"
#include "julia_voice.h"

#define TAG "JULIA_CONTEXT"
#define SAMPLE_PERIOD_MS 500
#define QUIET_COMPANION_MS (60 * 1000LL)
#define FAR_STANDBY_MS (5 * 60 * 1000LL)
#define DAY_AWAY_MS (20 * 60 * 1000LL)
#define NIGHT_SLEEP_IDLE_MS (10 * 60 * 1000LL)
#define MOTION_ACCEL_DELTA_G 0.10f
#define MOTION_GYRO_DPS 12.0f
#define LONG_ABSENCE_SECONDS (24LL * 60 * 60)

static volatile int64_t s_last_motion_ms;
static bool s_time_synced;
static bool s_rtc_valid;
static nvs_handle_t s_nvs;
static bool s_return_care_checked;

static bool valid_time(const datetime_t *value)
{
    return value->year >= 2024 && value->year <= 2099 && value->month >= 1 &&
           value->month <= 12 && value->day >= 1 && value->day <= 31 && value->hour < 24;
}

static void sync_rtc_from_system(void)
{
    time_t now = time(NULL);
    if (now < 1704067200) return;
    struct tm local; localtime_r(&now, &local);
    datetime_t value = {
        .year = local.tm_year + 1900, .month = local.tm_mon + 1, .day = local.tm_mday,
        .dotw = local.tm_wday, .hour = local.tm_hour, .minute = local.tm_min, .second = local.tm_sec,
    };
    PCF85063_Set_All(value);
    s_time_synced = true; s_rtc_valid = true;
    ESP_LOGI(TAG, "RTC synchronized: %04u-%02u-%02u %02u:%02u",
             value.year, value.month, value.day, value.hour, value.minute);
}

static int current_hour(void)
{
    time_t now = time(NULL);
    if (now >= 1704067200) { struct tm local; localtime_r(&now, &local); return local.tm_hour; }
    datetime_t value = {0}; PCF85063_Read_Time(&value);
    if (valid_time(&value)) { s_rtc_valid = true; return value.hour; }
    return -1;
}

static bool motion_detected(float *last_ax, float *last_ay, float *last_az)
{
    getAccelerometer(); getGyroscope();
    float delta = fabsf(Accel.x - *last_ax) + fabsf(Accel.y - *last_ay) + fabsf(Accel.z - *last_az);
    float gyro = sqrtf(Gyro.x * Gyro.x + Gyro.y * Gyro.y + Gyro.z * Gyro.z);
    *last_ax = Accel.x; *last_ay = Accel.y; *last_az = Accel.z;
    return delta >= MOTION_ACCEL_DELTA_G || gyro >= MOTION_GYRO_DPS;
}

static void save_context(julia_sub_state_t state, int64_t activity_ms)
{
    if (!s_nvs) return;
    nvs_set_u8(s_nvs, "last_state", (uint8_t)state);
    nvs_set_i64(s_nvs, "activity_ms", activity_ms);
    nvs_commit(s_nvs);
}

static void context_task(void *arg)
{
    (void)arg;
    float ax = 0, ay = 0, az = 0;
    s_last_motion_ms = esp_timer_get_time() / 1000;
    julia_sub_state_t saved_state = julia_voice_get_state();
    int log_counter = 0;
    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool motion = motion_detected(&ax, &ay, &az);
        if (motion) {
            s_last_motion_ms = now_ms;
            julia_routine_on_activity(JULIA_ACTIVITY_SENSOR);
        }
        julia_routine_is_deviation();
        int64_t audio_ms = julia_voice_last_audio_activity_ms();
        int64_t last_activity = s_last_motion_ms > audio_ms ? s_last_motion_ms : audio_ms;
        int64_t idle_ms = now_ms - last_activity;
        julia_sub_state_t state = julia_voice_get_state();

        if (!julia_voice_is_busy()) {
            if (motion && (state <= JULIA_SUB_STATE_S0_3_MANUAL_SLEEP ||
                           state == JULIA_SUB_STATE_S1_2_FAR_STANDBY)) {
                julia_voice_handle_event(EVT_USER_RETURN);
                if (!s_return_care_checked) {
                    time_t now = time(NULL);
                    int64_t last_interaction = julia_memory_last_interaction();
                    s_return_care_checked = true;
                    if (now >= 1704067200 && last_interaction >= 1704067200 &&
                        (int64_t)now - last_interaction >= LONG_ABSENCE_SECONDS) {
                        ESP_LOGI(TAG, "Long absence detected: %lld hours since last interaction",
                                 ((int64_t)now - last_interaction) / 3600);
                        julia_voice_handle_event(EVT_ROUTINE_BREAK);
                    }
                }
            } else {
                int hour = current_hour();
                bool night = hour >= 23 || (hour >= 0 && hour < 7);
                if (night && idle_ms >= NIGHT_SLEEP_IDLE_MS)
                    julia_voice_handle_event(EVT_NIGHT_TIME);
                else if (idle_ms >= DAY_AWAY_MS)
                    julia_voice_handle_event(EVT_DAY_AWAY);
                else if (idle_ms >= FAR_STANDBY_MS &&
                         state >= JULIA_SUB_STATE_S1_1_NEAR_STANDBY &&
                         state <= JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION)
                    julia_voice_handle_event(EVT_USER_LEAVE);
                else if (hour == 22 && state == JULIA_SUB_STATE_S1_1_NEAR_STANDBY)
                    julia_voice_handle_event(EVT_BEDTIME);
                else if (idle_ms >= QUIET_COMPANION_MS && state == JULIA_SUB_STATE_S1_1_NEAR_STANDBY)
                    julia_voice_handle_event(EVT_SILENCE_TIMEOUT);
            }
        }

        state = julia_voice_get_state();
        if (state != saved_state) { save_context(state, last_activity); saved_state = state; }
        if (++log_counter >= 20) {
            ESP_LOGI(TAG, "state=%s idle=%llds motion=%s time=%s",
                     julia_fsm_sub_state_name(state), idle_ms / 1000,
                     motion ? "yes" : "no", (s_time_synced || s_rtc_valid) ? "valid" : "unknown");
            log_counter = 0;
        }
        if (!s_time_synced && (log_counter % 10) == 0) sync_rtc_from_system();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

esp_err_t julia_context_init(void)
{
    setenv("TZ", "CST-8", 1); tzset();
    PCF85063_Init(); QMI8658_Init();
    setAccODR(acc_odr_norm_30); setGyroODR(gyro_odr_norm_30);
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    err = nvs_open("julia_ctx", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "NVS unavailable: %s", esp_err_to_name(err));
    if (xTaskCreate(context_task, "julia_context", 5120, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Context sensing ready: IMU, RTC, NTP and voice activity");
    return ESP_OK;
}
