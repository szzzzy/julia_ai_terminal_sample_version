/**
 * @file voice_uplink_pump.h
 * @brief WSS owner 使用的有界 MIC ring 排空与追赶策略。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice_uplink_ring.h"

typedef bool (*voice_uplink_send_fn_t)(void *ctx, const uint8_t *data,
                                       size_t len);
typedef int64_t (*voice_uplink_now_fn_t)(void *ctx);

typedef struct {
    void *ctx;
    voice_uplink_send_fn_t send;
    voice_uplink_now_fn_t now_us;
} voice_uplink_pump_ops_t;

typedef struct {
    unsigned normal_batch;
    unsigned catchup_batch;
    size_t catchup_threshold_frames;
    int64_t run_budget_us;
} voice_uplink_pump_config_t;

typedef enum {
    VOICE_UPLINK_PUMP_OK = 0,
    VOICE_UPLINK_PUMP_GENERATION_CLOSED,
    VOICE_UPLINK_PUMP_SEND_FAILED,
    VOICE_UPLINK_PUMP_RING_ERROR,
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
    unsigned frames_sent;
    size_t remaining_frames;
    bool catchup_started;
    bool catchup_completed;
    size_t catchup_peak_frames;
    int64_t catchup_drain_us;
} voice_uplink_pump_result_t;

bool voice_uplink_pump_init(voice_uplink_pump_t *pump,
                            voice_uplink_ring_t *ring,
                            const voice_uplink_pump_ops_t *ops,
                            const voice_uplink_pump_config_t *config);
void voice_uplink_pump_start_generation(voice_uplink_pump_t *pump,
                                        uint32_t generation);
void voice_uplink_pump_stop(voice_uplink_pump_t *pump);
voice_uplink_pump_result_t voice_uplink_pump_run(voice_uplink_pump_t *pump);
