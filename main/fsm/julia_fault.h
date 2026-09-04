/**
 * @file julia_fault.h
 * @brief S7.2 严重故障的原因分类与 NVS 快照接口。
 *
 * 本模块只记录已经完成本地恢复、仍无法继续提供核心能力的故障。
 * S7.1 只是业务连接断开的三秒提示，不写故障快照也不触发复位；网络暂时离线、
 * 单轮会话超时和普通 OTA 包校验失败同样不属于这里。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "julia_fsm.h"

typedef enum {
    JULIA_FAULT_NONE = 0,                 /**< 未发生严重故障。 */
    JULIA_FAULT_CRITICAL_INIT,            /**< 未能进一步归类的关键初始化失败。 */
    JULIA_FAULT_DISPLAY_INIT,             /**< 显示、Avatar、背光或闲置显示初始化失败。 */
    JULIA_FAULT_AUDIO_INIT,               /**< 板级音频或语音播放装配失败。 */
    JULIA_FAULT_VOICE_INIT,               /**< 语音服务或唤醒检测初始化失败。 */
    JULIA_FAULT_FSM_RUNTIME_INIT,         /**< FSM 队列、计时器或任务创建失败。 */
    JULIA_FAULT_FSM_STATE_CORRUPT,        /**< 主状态与 S2 子状态组合非法。 */
    JULIA_FAULT_NVS_UNRECOVERABLE,        /**< NVS 初始化或修复失败。 */
    JULIA_FAULT_FLASH_IO,                 /**< Flash/启动分区访问失败。 */
    JULIA_FAULT_OTA_ROLLBACK_UNAVAILABLE, /**< OTA 启动验收失败且无法安全回滚。 */
    JULIA_FAULT_CORE_TASK_STALLED,        /**< 预留给后续核心任务心跳检测。 */
} julia_fault_reason_t;

typedef struct {
    uint32_t schema_version;    /**< 记录布局版本。 */
    uint32_t sequence;          /**< 全局递增故障序号。 */
    uint32_t repeat_count;      /**< 同原因短时连续出现次数。 */
    uint32_t reason;            /**< julia_fault_reason_t 的持久化值。 */
    int32_t error_code;         /**< 原始 esp_err_t。 */
    uint32_t uptime_ms;         /**< 故障发生时本次启动已运行时长。 */
    uint32_t free_heap;         /**< 故障发生时 8-bit 可用堆。 */
    uint32_t reset_reason;      /**< 本次启动的 ESP-IDF 复位原因。 */
    uint8_t main_state;         /**< 进入 S7.2 前的主状态。 */
    uint8_t s2_sub_state;       /**< 进入 S7.2 前的 S2 子状态。 */
    uint8_t reserved[2];        /**< 后续布局扩展保留。 */
    char firmware_version[32];  /**< 当前应用版本。 */
} julia_fault_record_t;

/** 将最近一次严重故障覆盖写入独立 NVS namespace，并执行一次 commit。 */
esp_err_t julia_fault_record(julia_fault_reason_t reason, esp_err_t error,
                             julia_main_state_t main_state,
                             julia_s2_sub_state_t s2_sub_state);

/** 读取最近一次故障快照；尚无记录时返回 ESP_ERR_NOT_FOUND。 */
esp_err_t julia_fault_read_last(julia_fault_record_t *record);

/** 最近同类故障短时连续出现不超过三次时允许自动复位。 */
bool julia_fault_reset_allowed(void);

const char *julia_fault_reason_name(julia_fault_reason_t reason);
