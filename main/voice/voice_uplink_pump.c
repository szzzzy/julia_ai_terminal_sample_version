/**
 * @file voice_uplink_pump.c
 * @brief MIC ring 的正常发送与有界追赶实现。
 */
#include "voice_uplink_pump.h"

#include <string.h>

static void start_catchup(voice_uplink_pump_t *pump, size_t backlog,
                          int64_t now_us,
                          voice_uplink_pump_result_t *result)
{
    if (pump->catchup_active ||
        backlog < pump->config.catchup_threshold_frames) {
        return;
    }
    pump->catchup_active = true;
    pump->catchup_started_us = now_us;
    pump->catchup_peak_frames = backlog;
    result->catchup_started = true;
}

static void update_catchup_peak(voice_uplink_pump_t *pump, size_t backlog)
{
    if (pump->catchup_active && backlog > pump->catchup_peak_frames) {
        pump->catchup_peak_frames = backlog;
    }
}

bool voice_uplink_pump_init(voice_uplink_pump_t *pump,
                            voice_uplink_ring_t *ring,
                            const voice_uplink_pump_ops_t *ops,
                            const voice_uplink_pump_config_t *config)
{
    if (pump == NULL || ring == NULL || ops == NULL || ops->send == NULL ||
        ops->now_us == NULL || config == NULL || config->normal_batch == 0U ||
        config->catchup_batch <= config->normal_batch ||
        config->catchup_threshold_frames < 2U || config->run_budget_us <= 0) {
        return false;
    }
    memset(pump, 0, sizeof(*pump));
    pump->ring = ring;
    pump->ops = *ops;
    pump->config = *config;
    return true;
}

void voice_uplink_pump_start_generation(voice_uplink_pump_t *pump,
                                        uint32_t generation)
{
    if (pump == NULL) return;
    pump->generation = generation;
    pump->catchup_active = false;
    pump->catchup_started_us = 0;
    pump->catchup_peak_frames = 0;
}

void voice_uplink_pump_stop(voice_uplink_pump_t *pump)
{
    voice_uplink_pump_start_generation(pump, 0);
}

voice_uplink_pump_result_t voice_uplink_pump_run(voice_uplink_pump_t *pump)
{
    voice_uplink_pump_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = VOICE_UPLINK_PUMP_RING_ERROR;
    if (pump == NULL || pump->ring == NULL || pump->generation == 0U) {
        return result;
    }

    result.status = VOICE_UPLINK_PUMP_OK;
    if (!voice_uplink_ring_is_accepting(pump->ring)) {
        result.status = VOICE_UPLINK_PUMP_GENERATION_CLOSED;
        result.remaining_frames = voice_uplink_ring_count(pump->ring);
        return result;
    }
    int64_t run_started_us = pump->ops.now_us(pump->ops.ctx);
    size_t backlog = voice_uplink_ring_count(pump->ring);
    start_catchup(pump, backlog, run_started_us, &result);
    update_catchup_peak(pump, backlog);
    unsigned batch = pump->catchup_active ? pump->config.catchup_batch
                                          : pump->config.normal_batch;

    while (result.frames_sent < batch) {
        if (!voice_uplink_ring_is_accepting(pump->ring)) {
            result.status = VOICE_UPLINK_PUMP_GENERATION_CLOSED;
            break;
        }
        if (result.frames_sent > 0U &&
            pump->ops.now_us(pump->ops.ctx) - run_started_us >=
                pump->config.run_budget_us) {
            break;
        }
        const uint8_t *frame = NULL;
        size_t bytes = 0;
        if (!voice_uplink_ring_peek(pump->ring, pump->generation,
                                    &frame, &bytes)) {
            break;
        }
        if (!pump->ops.send(pump->ops.ctx, frame, bytes)) {
            result.status = VOICE_UPLINK_PUMP_SEND_FAILED;
            break;
        }
        if (!voice_uplink_ring_consume(pump->ring)) {
            result.status = VOICE_UPLINK_PUMP_RING_ERROR;
            break;
        }
        result.frames_sent++;
        backlog = voice_uplink_ring_count(pump->ring);
        update_catchup_peak(pump, backlog);
    }

    result.remaining_frames = voice_uplink_ring_count(pump->ring);
    if (result.status != VOICE_UPLINK_PUMP_OK) return result;
    /* 一次正常发送可能在 TLS 内停滞并让 producer 积累数据，因此发送完成后也要
     * 立即进入追赶状态，下一轮无需再等待额外阈值判断。 */
    start_catchup(pump, result.remaining_frames,
                  pump->ops.now_us(pump->ops.ctx), &result);
    update_catchup_peak(pump, result.remaining_frames);
    if (pump->catchup_active &&
        result.remaining_frames < pump->config.catchup_threshold_frames) {
        result.catchup_completed = true;
        result.catchup_peak_frames = pump->catchup_peak_frames;
        result.catchup_drain_us = pump->ops.now_us(pump->ops.ctx) -
                                  pump->catchup_started_us;
        pump->catchup_active = false;
        pump->catchup_started_us = 0;
        pump->catchup_peak_frames = 0;
    }
    return result;
}
