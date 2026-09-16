/**
 * @file voice_uplink_pump.h
 * @brief 公平发送麦克风积压：网络正常时低延迟发送，积压时有限追赶但不饿死控制消息。
 *
 * 每轮通常发送一块声音；积压到阈值后可多发几块，但同时受块数和执行时间限制。
 * 这样既能逐步追上实时采集，也不会让唤醒、停止播放、保活和下行回答长期得不到处理。
 * 当前取值（见 voice_service.c）：正常 1 帧/轮，追赶上限 8 帧且单轮预算 8000 us。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice_uplink_ring.h"

/** 返回 false 表示本帧未送出；此时 pump 不会 consume，帧仍留在 ring 中，由上层把
 * VOICE_UPLINK_PUMP_SEND_FAILED 升级为结束连接。 */
typedef bool (*voice_uplink_send_fn_t)(void *ctx, const uint8_t *data,
                                       size_t len);
/** 单调时钟，单位 us。 */
typedef int64_t (*voice_uplink_now_fn_t)(void *ctx);

/** 回调在 pump 调用者（WSS owner）上下文中同步执行；ctx 原样回传，不做并发保护。 */
typedef struct {
    void *ctx;
    voice_uplink_send_fn_t send;
    voice_uplink_now_fn_t now_us;
} voice_uplink_pump_ops_t;

typedef struct {
    unsigned normal_batch; /**< 正常每轮发送帧数上限。 */
    unsigned catchup_batch; /**< 追赶时每轮发送帧数上限，必须大于 normal_batch。 */
    size_t catchup_threshold_frames; /**< 追赶的进入与退出共用该阈值，单位帧；至少为 2。 */
    int64_t run_budget_us; /**< 单轮时间预算，单位 us；只在已发出至少一帧后检查。 */
} voice_uplink_pump_config_t;

typedef enum {
    VOICE_UPLINK_PUMP_OK = 0, /**< 本轮正常结束。 */
    VOICE_UPLINK_PUMP_GENERATION_CLOSED, /**< 入口已关闭；不是错误，只回报剩余积压。 */
    VOICE_UPLINK_PUMP_SEND_FAILED, /**< send 回调失败；失败帧未 consume，上层应据此结束连接。 */
    VOICE_UPLINK_PUMP_RING_ERROR, /**< generation 为 0 或 peek/consume 配对被破坏；调用方应重建会话。 */
} voice_uplink_pump_status_t;

typedef struct {
    voice_uplink_ring_t *ring;
    voice_uplink_pump_ops_t ops;
    voice_uplink_pump_config_t config;
    uint32_t generation;
    bool catchup_active;
    int64_t catchup_started_us;
    size_t catchup_peak_frames;
} voice_uplink_pump_t;

typedef struct {
    voice_uplink_pump_status_t status;
    unsigned frames_sent; /**< 本轮已发送并 consume 的帧数。 */
    size_t remaining_frames; /**< 本轮结束时的近似积压帧数。 */
    bool catchup_started; /**< 本轮是否刚进入追赶。 */
    bool catchup_completed; /**< 本轮是否刚退出追赶；只有此时 peak/drain 有效。 */
    size_t catchup_peak_frames; /**< 截至本轮的这段追赶期间最大积压帧数。 */
    int64_t catchup_drain_us; /**< 这段追赶从进入到退出的耗时，单位 us。 */
} voice_uplink_pump_result_t;

/** 绑定 ring 与回调。ring 与 ops.ctx 指向的对象按指针使用、不做拷贝，生命周期必须长于
 * pump（ops/config 结构体本身按值复制）。配置非法时返回 false。 */
bool voice_uplink_pump_init(voice_uplink_pump_t *pump,
                            voice_uplink_ring_t *ring,
                            const voice_uplink_pump_ops_t *ops,
                            const voice_uplink_pump_config_t *config);
/** 进入新的数据归属代次并复位追赶状态；0 只用于停止，见 voice_uplink_pump_stop()。 */
void voice_uplink_pump_start_generation(voice_uplink_pump_t *pump,
                                        uint32_t generation);
/** 停止发送：等价于 start_generation(0)，此后 run 直接返回 RING_ERROR，直到重新指定代次。 */
void voice_uplink_pump_stop(voice_uplink_pump_t *pump);
/** 执行一轮有界发送；只在 WSS owner 上下文调用，不在 ring 上阻塞等待数据。 */
voice_uplink_pump_result_t voice_uplink_pump_run(voice_uplink_pump_t *pump);
