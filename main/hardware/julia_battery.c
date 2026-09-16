/**
 * @file julia_battery.c
 * @brief 读取板载 BAT_ADC，提供启动压降日志和运行期电量估算。
 *
 * 采样链：GPIO8=BAT_ADC（ADC1_CH7）经 200K/100K 分压，ADC 校准读数乘 3 还原电池端
 * 电压；每次测量取 CONFIG_JULIA_BATTERY_SAMPLE_COUNT 个样本求平均，显示值再与上次
 * 结果做 3:1 低通。所有电压单位为 mV，百分比为 0～100。
 *
 * 本模块只产生 UI 提示用的 NORMAL／LOW 状态，不进入行为 FSM，也不推测充电状态：
 * present 只表示端电压落在合理区间，BAT 节点有电压不等于物理上装着电池。
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
#include "sdkconfig.h"

#define JULIA_BAT_ADC_UNIT        ADC_UNIT_1
/* 本板 IO8 是 BAT_ADC；ADC 通道从 0 编号，ADC1_CH7 对应 GPIO8。
 * IO9 接 RTC_INT，不能拿它估算电池电压。 */
#define JULIA_BAT_ADC_CHANNEL     ADC_CHANNEL_7
/* 6 dB 衰减必须覆盖分压后的端电压：2500～4350 mV 电池对应 ADC 端约 0.83～1.45 V。 */
#define JULIA_BAT_ADC_ATTEN       ADC_ATTEN_DB_6
/* 200K/100K 分压使 ADC 端只有电池电压的 1/3，还原端电压时乘 3。 */
#define JULIA_BAT_DIVIDER_NUM     3U

#ifndef CONFIG_JULIA_BATTERY_DIAGNOSTICS
#define CONFIG_JULIA_BATTERY_DIAGNOSTICS 0
#endif
#ifndef CONFIG_JULIA_BATTERY_SAMPLE_COUNT
#define CONFIG_JULIA_BATTERY_SAMPLE_COUNT 16
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
static const char *TAG = "JULIA_BATTERY";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static TaskHandle_t s_monitor_task;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static julia_battery_status_t s_status;
static julia_battery_update_cb_t s_callback;
static void *s_callback_ctx;
/* 低电量锁存：进入阈值取 CONFIG_JULIA_BATTERY_LOW_ENTER_PERCENT（当前 15%），退出阈值取
 * CONFIG_JULIA_BATTERY_LOW_EXIT_PERCENT（当前 25%）。两者分开是为了让百分比在阈值附近
 * 抖动时不反复切换 NORMAL／LOW；提示语音是一次性标志，退出 LOW 后才会重新武装。 */
static bool s_low_latched;

typedef struct {
    uint16_t mv;
    uint8_t percent;
} battery_curve_point_t;

/* 单节锂电池静置电压的保守近似。设备带载时会低估电量，因此该值只适合作为
 * UI 提示和低电量趋势，不作为容量计量或保护阈值。
 * 3.40 V=0% 与 3.55 V=15% 是保留的低端标定点，4.10 V=90% 与 4.20 V=100% 是高端点；
 * 相邻点之间线性插值。这些点位在仓库内找不到标定记录，来源未确认。
 * UI 侧 julia_avatar_set_battery_status() 会把结果取整到 5% 步进（≥98% 显示 100%）。 */
static const battery_curve_point_t s_curve[] = {
    {3400, 0}, {3550, 15}, {3650, 30}, {3750, 50},
    {3850, 65}, {3950, 80}, {4100, 90}, {4200, 100},
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
    case JULIA_BATTERY_STATE_UNKNOWN:
    default: return "UNKNOWN";
    }
}

/* 失败样本被跳过并只统计有效个数，全部失败才返回 ESP_FAIL；此时调用方保持上一次
 * 快照不变，不会因为一次读失败把电量清成 UNKNOWN。min/max 供启动阶段日志判断压降。 */
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

/* 显示电压是 3:1 一阶低通结果，不等于单次 ADC 读数：负载突变后需要多个测量周期才
 * 跟得上；present 判定变化或首次测量时直接采用本次原始值。低通权重是模块内固定值，
 * 仓库里没有相关标定记录，来源未确认。 */
static julia_battery_status_t update_status(uint16_t raw_mv)
{
    julia_battery_status_t snapshot;
    bool present = raw_mv >= CONFIG_JULIA_BATTERY_PRESENT_MIN_MV &&
                   raw_mv <= CONFIG_JULIA_BATTERY_PRESENT_MAX_MV;
    portENTER_CRITICAL(&s_status_lock);
    uint16_t display_mv = raw_mv;
    if (present && s_status.valid && s_status.present) {
        display_mv = (uint16_t)(((uint32_t)s_status.voltage_mv * 3U + raw_mv + 2U) / 4U);
    }
    s_status.valid = true;
    s_status.present = present;
    s_status.voltage_mv = display_mv;
    s_status.percent = present ? percent_for_voltage(display_mv) : 0U;

    if (!present) {
        /* 电压不可信时清除锁存：present 只表示电压合理，无法据此判断电池是否真被取下。 */
        s_low_latched = false;
        s_status.state = JULIA_BATTERY_STATE_UNKNOWN;
    } else {
        if (s_status.percent <= CONFIG_JULIA_BATTERY_LOW_ENTER_PERCENT) {
            s_low_latched = true;
        } else if (s_status.percent >= CONFIG_JULIA_BATTERY_LOW_EXIT_PERCENT) {
            s_low_latched = false;
        }
        s_status.state = s_low_latched ? JULIA_BATTERY_STATE_LOW :
                                         JULIA_BATTERY_STATE_NORMAL;
    }
    snapshot = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return snapshot;
}

/* 幂等：已就绪直接返回 ESP_OK。任何一步失败都先释放 ADC unit 再返回，不留下
 * 半初始化的 s_adc／s_cali 供后续采样使用。关闭诊断配置时返回 NOT_SUPPORTED。 */
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
    julia_battery_status_t status = update_status(voltage_mv);
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
            julia_battery_status_t status = update_status(voltage_mv);
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

/* 幂等：重复调用只登记回调，不会创建第二个监测任务。回调在 battery_monitor 任务
 * 上下文按测量周期执行，实现应快速返回。任务栈 3072、优先级 2 是模块内常量，
 * 仓库内没有对应配置项，来源未确认。 */
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
