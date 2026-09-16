/**
 * @file julia_fault.h
 * @brief S7.2 严重故障的原因分类与 NVS 快照接口。
 *
 * 本模块只记录已经完成本地恢复、仍无法继续提供核心能力的故障。
 * S7.1 只负责一次可恢复断联提示，不写故障快照也不触发复位；持续离线由独立
 * 服务状态保存。单轮会话超时和普通 OTA 包校验失败同样不属于这里。
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

/**
 * 最近一次严重故障的定长快照。读取时要求 blob 长度与 schema_version 都与当前固件一致，
 * 否则丢弃内容并返回 ESP_ERR_INVALID_VERSION。
 *
 * main_state/s2_sub_state 保存的是 julia_main_state_t/julia_s2_sub_state_t 的枚举序号，
 * 没有同时保存状态名字。因此重排或插入枚举成员会让历史快照静默指向另一个状态：
 * 这类改动必须同时提升 FAULT_SCHEMA_VERSION（见 julia_fault.c，当前 1U），必要时还要
 * 调整下面的字段布局。
 */
typedef struct {
    uint32_t schema_version;    /**< 记录布局版本；与 FAULT_SCHEMA_VERSION 不符即视为无效快照。 */
    uint32_t sequence;          /**< 每次成功写入递增的故障序号；读不到上一条时从 1 重新开始。 */
    uint32_t repeat_count;      /**< 同原因快速故障链长度：上一条快照原因相同、两次启动都发生在
                                 *   CONFIG_JULIA_FAULT_QUICK_UPTIME_SECONDS（Kconfig，当前 60 s）之内，
                                 *   且固件版本字符串一致时才加一，否则重置为 1；饱和于 UINT32_MAX。 */
    uint32_t reason;            /**< julia_fault_reason_t 的持久化值。 */
    int32_t error_code;         /**< 原始 esp_err_t。 */
    uint32_t uptime_ms;         /**< 故障发生时本次启动已运行时长，单位 ms，基于 esp_timer 单调时钟。 */
    uint32_t free_heap;         /**< 故障发生时 8-bit 可用堆，单位 byte。 */
    uint32_t reset_reason;      /**< 本次启动的 ESP-IDF 复位原因。 */
    uint8_t main_state;         /**< 进入 S7.2 前的主状态枚举序号，非状态名。 */
    uint8_t s2_sub_state;       /**< 进入 S7.2 前的 S2 子状态枚举序号，非状态名。 */
    uint8_t reserved[2];        /**< 后续布局扩展保留。 */
    char firmware_version[32];  /**< 当前应用版本；同时用于判断是否仍处于同一条快速故障链。 */
} julia_fault_record_t;

/**
 * 将最近一次严重故障覆盖写入独立 NVS namespace，并执行一次 commit。
 *
 * 原因越界，或 main_state/s2_sub_state 不是 julia_fsm_state_is_valid() 认可的合法组合时
 * 返回 ESP_ERR_INVALID_ARG 且不写盘。写入失败只返回 NVS/ESP-IDF 错误：快照是诊断手段
 * 而不是恢复前提，调用方仍可继续故障呈现和复位，但也就失去了重复故障的判据。
 */
esp_err_t julia_fault_record(julia_fault_reason_t reason, esp_err_t error,
                             julia_main_state_t main_state,
                             julia_s2_sub_state_t s2_sub_state);

/** 读取最近一次故障快照；尚无记录时返回 ESP_ERR_NOT_FOUND。 */
esp_err_t julia_fault_read_last(julia_fault_record_t *record);

/**
 * 最近一次快照的 repeat_count 不超过 CONFIG_JULIA_FAULT_AUTO_RESET_LIMIT
 * （Kconfig 可配，当前值 3）时允许自动复位。
 *
 * 读不到快照（无记录、版本不符或 NVS 不可用）时同样返回 true：没有可依据的重复证据，
 * 就按首次故障处理。调用方据此决定是自动复位还是停留在 S7.2。
 */
bool julia_fault_reset_allowed(void);

const char *julia_fault_reason_name(julia_fault_reason_t reason);
