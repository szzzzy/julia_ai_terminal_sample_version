/**
 * @file julia_battery.c
 * @brief 读取板载 BAT_ADC，提供启动压降日志和运行期电量估算。
 */
#include "julia_battery.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_charge_detector.h"
#include "sdkconfig.h"

#define JULIA_BAT_ADC_UNIT        ADC_UNIT_1
/* 本板 IO8 是 BAT_ADC；ADC 通道从 0 编号，ADC1_CH7 对应 GPIO8。
 * IO9 接 RTC_INT，不能拿它估算电池电压。 */
#define JULIA_BAT_ADC_CHANNEL     ADC_CHANNEL_7
#define JULIA_BAT_ADC_ATTEN       ADC_ATTEN_DB_6
#define JULIA_BAT_DIVIDER_NUM     3U

#ifndef CONFIG_JULIA_BATTERY_DIAGNOSTICS
#define CONFIG_JULIA_BATTERY_DIAGNOSTICS 0
#endif
#ifndef CONFIG_JULIA_BATTERY_SAMPLE_COUNT
#define CONFIG_JULIA_BATTERY_SAMPLE_COUNT 32
#endif
#ifndef CONFIG_JULIA_BATTERY_MONITOR_INTERVAL_SECONDS
#define CONFIG_JULIA_BATTERY_MONITOR_INTERVAL_SECONDS 30
#endif
#ifndef CONFIG_JULIA_BATTERY_PRESENT_MIN_MV
#define CONFIG_JULIA_BATTERY_PRESENT_MIN_MV 2500
#endif
#ifndef CONFIG_JULIA_BATTERY_PRESENT_MAX_MV
#define CONFIG_JULIA_BATTERY_PRESENT_MAX_MV 4350
#endif
#ifndef CONFIG_JULIA_BATTERY_LOW_ENTER_PERCENT
#define CONFIG_JULIA_BATTERY_LOW_ENTER_PERCENT 15
#endif
#ifndef CONFIG_JULIA_BATTERY_LOW_EXIT_PERCENT
#define CONFIG_JULIA_BATTERY_LOW_EXIT_PERCENT 25
#endif
#ifndef CONFIG_JULIA_BATTERY_CHARGE_RISE_MV
#define CONFIG_JULIA_BATTERY_CHARGE_RISE_MV 20
#endif
#ifndef CONFIG_JULIA_BATTERY_CHARGE_EXIT_DROP_MV
#define CONFIG_JULIA_BATTERY_CHARGE_EXIT_DROP_MV 20
#endif
#ifndef CONFIG_JULIA_BATTERY_CHARGE_CONFIRM_SAMPLES
#define CONFIG_JULIA_BATTERY_CHARGE_CONFIRM_SAMPLES 2
#endif
#ifndef CONFIG_JULIA_BATTERY_CHARGE_EVIDENCE_TIMEOUT_SECONDS
#define CONFIG_JULIA_BATTERY_CHARGE_EVIDENCE_TIMEOUT_SECONDS 60
#endif

#define CHARGE_EVIDENCE_TIMEOUT_SAMPLES \
    ((CONFIG_JULIA_BATTERY_CHARGE_EVIDENCE_TIMEOUT_SECONDS + \
      CONFIG_JULIA_BATTERY_MONITOR_INTERVAL_SECONDS - 1U) / \
     CONFIG_JULIA_BATTERY_MONITOR_INTERVAL_SECONDS)

static const char *TAG = "JULIA_BATTERY";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static TaskHandle_t s_monitor_task;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static julia_battery_status_t s_status;
static julia_battery_update_cb_t s_callback;
static void *s_callback_ctx;
static bool s_low_latched;
static julia_charge_detector_t s_charge_detector;

typedef struct {
    uint16_t mv;
    uint8_t percent;
} battery_curve_point_t;

/* 单节锂电池静置电压的保守近似。设备带载时会低估电量，因此该值只适合作为
 * UI 提示和低电量趋势，不作为容量计量或保护阈值。 */
static const battery_curve_point_t s_curve[] = {
    {3000, 0}, {3300, 2}, {3500, 7}, {3600, 15}, {3700, 30},
    {3800, 50}, {3900, 65}, {4000, 80}, {4100, 90}, {4200, 100},
};

static uint8_t percent_for_voltage(uint16_t mv)
{
    if (mv <= s_curve[0].mv) return s_curve[0].percent;
    for (size_t i = 1; i < sizeof(s_curve) / sizeof(s_curve[0]); ++i) {
        if (mv <= s_curve[i].mv) {
            uint32_t span_mv = s_curve[i].mv - s_curve[i - 1U].mv;
            uint32_t offset_mv = mv - s_curve[i - 1U].mv;
            uint32_t span_percent = s_curve[i].percent - s_curve[i - 1U].percent;
            return (uint8_t)(s_curve[i - 1U].percent +
                             (offset_mv * span_percent + span_mv / 2U) / span_mv);
        }
    }
    return 100U;
}

static const char *battery_state_name(julia_battery_state_t state)
{
    switch (state) {
    case JULIA_BATTERY_STATE_NORMAL: return "NORMAL";
    case JULIA_BATTERY_STATE_LOW: return "LOW";
    case JULIA_BATTERY_STATE_CHARGING: return "CHARGING";
    case JULIA_BATTERY_STATE_UNKNOWN:
    default: return "UNKNOWN";
    }
}

static esp_err_t read_voltage(uint16_t *voltage_mv, uint16_t *minimum_mv,
                              uint16_t *maximum_mv)
{
    if (voltage_mv == NULL || s_adc == NULL || s_cali == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t sum_mv = 0U;
    int min_mv = INT32_MAX;
    int max_mv = 0;
    unsigned valid = 0;
    for (unsigned i = 0; i < CONFIG_JULIA_BATTERY_SAMPLE_COUNT; ++i) {
        int adc_mv = 0;
        esp_err_t err = adc_oneshot_get_calibrated_result(
            s_adc, s_cali, JULIA_BAT_ADC_CHANNEL, &adc_mv);
        if (err == ESP_OK) {
            if (adc_mv < min_mv) min_mv = adc_mv;
            if (adc_mv > max_mv) max_mv = adc_mv;
            sum_mv += (uint32_t)adc_mv;
            ++valid;
        }
        vTaskDelay(1);
    }
    if (valid == 0U) return ESP_FAIL;

    uint32_t average_mv = (sum_mv + valid / 2U) / valid;
    *voltage_mv = (uint16_t)(average_mv * JULIA_BAT_DIVIDER_NUM);
    if (minimum_mv != NULL) {
        *minimum_mv = (uint16_t)(min_mv * (int)JULIA_BAT_DIVIDER_NUM);
    }
    if (maximum_mv != NULL) {
        *maximum_mv = (uint16_t)(max_mv * (int)JULIA_BAT_DIVIDER_NUM);
    }
    return ESP_OK;
}

static julia_battery_status_t update_status(uint16_t raw_mv, bool infer_charging)
{
    julia_battery_status_t snapshot;
    bool present = raw_mv >= CONFIG_JULIA_BATTERY_PRESENT_MIN_MV &&
                   raw_mv <= CONFIG_JULIA_BATTERY_PRESENT_MAX_MV;
    portENTER_CRITICAL(&s_status_lock);
    bool previous_present = s_status.valid && s_status.present;
    uint16_t display_mv = raw_mv;
    if (present && s_status.valid && s_status.present) {
        display_mv = (uint16_t)(((uint32_t)s_status.voltage_mv * 3U + raw_mv + 2U) / 4U);
    }
    s_status.valid = true;
    s_status.present = present;
    s_status.voltage_mv = display_mv;
    s_status.percent = present ? percent_for_voltage(display_mv) : 0U;

    if (!present) {
        s_low_latched = false;
        julia_charge_detector_reset(&s_charge_detector);
        s_status.state = JULIA_BATTERY_STATE_UNKNOWN;
    } else {
        bool charging = false;
        if (infer_charging && previous_present) {
            charging = julia_charge_detector_update(
                &s_charge_detector, true, raw_mv,
                CONFIG_JULIA_BATTERY_CHARGE_RISE_MV,
                CONFIG_JULIA_BATTERY_CHARGE_EXIT_DROP_MV,
                CONFIG_JULIA_BATTERY_CHARGE_CONFIRM_SAMPLES,
                CHARGE_EVIDENCE_TIMEOUT_SAMPLES);
        } else {
            julia_charge_detector_reset(&s_charge_detector);
        }

        if (s_status.percent <= CONFIG_JULIA_BATTERY_LOW_ENTER_PERCENT) {
            s_low_latched = true;
        } else if (s_status.percent >= CONFIG_JULIA_BATTERY_LOW_EXIT_PERCENT) {
            s_low_latched = false;
        }
        s_status.state = charging ? JULIA_BATTERY_STATE_CHARGING :
                         s_low_latched ? JULIA_BATTERY_STATE_LOW :
                         JULIA_BATTERY_STATE_NORMAL;
    }
    snapshot = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return snapshot;
}

esp_err_t julia_battery_diagnostics_init(void)
{
#if !CONFIG_JULIA_BATTERY_DIAGNOSTICS
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_adc != NULL && s_cali != NULL) return ESP_OK;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = JULIA_BAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) return err;

    adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = JULIA_BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, JULIA_BAT_ADC_CHANNEL, &channel_cfg);
    if (err != ESP_OK) {
        (void)adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = JULIA_BAT_ADC_UNIT,
        .chan = JULIA_BAT_ADC_CHANNEL,
        .atten = JULIA_BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable: %s", esp_err_to_name(err));
        (void)adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return err;
    }

    ESP_LOGI(TAG, "ready gpio=8 adc1_ch7 divider=3:1 samples=%d",
             CONFIG_JULIA_BATTERY_SAMPLE_COUNT);
    return ESP_OK;
#endif
}

void julia_battery_log_stage(const char *stage)
{
#if CONFIG_JULIA_BATTERY_DIAGNOSTICS
    if (s_adc == NULL || s_cali == NULL) return;
    uint16_t voltage_mv = 0;
    uint16_t minimum_mv = 0;
    uint16_t maximum_mv = 0;
    if (read_voltage(&voltage_mv, &minimum_mv, &maximum_mv) != ESP_OK) {
        ESP_LOGW(TAG, "stage=%s no valid ADC samples", stage ? stage : "unknown");
        return;
    }
    julia_battery_status_t status = update_status(voltage_mv, false);
    ESP_LOGI(TAG, "stage=%s vbat=%umV range=%u..%umV percent=%u present=%u state=%s",
             stage ? stage : "unknown",
             status.voltage_mv, minimum_mv, maximum_mv,
             status.percent, status.present ? 1U : 0U,
             battery_state_name(status.state));
#else
    (void)stage;
#endif
}

static void battery_monitor_task(void *arg)
{
    (void)arg;
    TickType_t deadline = xTaskGetTickCount();
    for (;;) {
        uint16_t voltage_mv = 0;
        if (read_voltage(&voltage_mv, NULL, NULL) == ESP_OK) {
            julia_battery_status_t status = update_status(voltage_mv, true);
            ESP_LOGI(TAG, "monitor vbat=%umV percent=%u present=%u state=%s",
                     status.voltage_mv, status.percent, status.present ? 1U : 0U,
                     battery_state_name(status.state));
            if (s_callback != NULL) s_callback(&status, s_callback_ctx);
        } else {
            ESP_LOGW(TAG, "periodic ADC read failed");
        }
        vTaskDelayUntil(&deadline,
                        pdMS_TO_TICKS(CONFIG_JULIA_BATTERY_MONITOR_INTERVAL_SECONDS * 1000U));
    }
}

esp_err_t julia_battery_monitor_start(julia_battery_update_cb_t callback, void *ctx)
{
#if !CONFIG_JULIA_BATTERY_DIAGNOSTICS
    (void)callback;
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_adc == NULL || s_cali == NULL) return ESP_ERR_INVALID_STATE;
    if (s_monitor_task != NULL) return ESP_OK;
    s_callback = callback;
    s_callback_ctx = ctx;
    if (xTaskCreate(battery_monitor_task, "battery_monitor", 3072, NULL, 2,
                    &s_monitor_task) != pdPASS) {
        s_monitor_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

esp_err_t julia_battery_get_status(julia_battery_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}
