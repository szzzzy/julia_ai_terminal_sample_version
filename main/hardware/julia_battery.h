#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 与行为 S0～S8 正交的持久电池状态。UNKNOWN 表示尚无可信电池电压。 */
typedef enum {
    JULIA_BATTERY_STATE_UNKNOWN = 0,
    JULIA_BATTERY_STATE_NORMAL,
    JULIA_BATTERY_STATE_LOW,
    JULIA_BATTERY_STATE_CHARGING,
} julia_battery_state_t;

typedef struct {
    bool valid;          /**< 至少完成过一次有效 ADC 测量。 */
    bool present;        /**< BAT 节点位于合理单节锂电池电压范围。 */
    uint16_t voltage_mv; /**< 滤波后的电池端估算电压。 */
    uint8_t percent;     /**< 基于带载端电压曲线估算的剩余百分比。 */
    julia_battery_state_t state; /**< NORMAL／LOW／CHARGING 正交状态。 */
} julia_battery_status_t;

typedef void (*julia_battery_update_cb_t)(const julia_battery_status_t *status,
                                          void *ctx);

/**
 * @brief 初始化板载 BAT_ADC（GPIO8 / ADC1_CH7）诊断通道。
 *
 * 初始化失败不会影响电源保持或其它外设；调用方可以继续启动，只是没有电池电压日志。
 */
esp_err_t julia_battery_diagnostics_init(void);

/**
 * @brief 对 BAT_ADC 做一组校准采样并记录当前启动阶段的电池电压。
 *
 * 板载 200K/100K 分压使 ADC 输入约为电池电压的三分之一。函数只用于诊断，
 * 不把单次电压读数解释为电池容量、充电状态或精确的输出电流。
 */
void julia_battery_log_stage(const char *stage);

/**
 * @brief 启动常驻电量监测任务；首次测量会立即执行，之后按配置周期更新。
 *
 * @param[in] callback 每次有效测量后的通知；可为 NULL。
 * @param[in] ctx 原样传给 callback。
 */
esp_err_t julia_battery_monitor_start(julia_battery_update_cb_t callback, void *ctx);

/** 获取最近一次线程安全快照；尚无有效测量时 valid=false。 */
esp_err_t julia_battery_get_status(julia_battery_status_t *status);
