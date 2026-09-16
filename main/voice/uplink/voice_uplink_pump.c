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
    /* 进入条件与退出条件共用 catchup_threshold_frames：一旦进入，就持续发送到剩余积压
     * 低于该阈值为止，不引入第二个滞回阈值。 */
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
    /* generation 为 0 只可能是 stop() 之后或尚未指定代次；此处不猜测行为，直接报错。 */
    if (pump == NULL || pump->ring == NULL || pump->generation == 0U) {
        return result;
    }

    result.status = VOICE_UPLINK_PUMP_OK;
    if (!voice_uplink_ring_is_accepting(pump->ring)) {
        /* 入口关闭是会话收尾的正常状态，不是错误：只把剩余积压回报给上层。 */
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
        /* 只在已发出至少一帧后才检查时间预算：预算小于单帧耗时（TLS 写入停滞）时，
         * 也必须保证每轮至少推进一帧，否则积压永远不下降。 */
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
            /* 发送失败不 consume：帧仍留在 ring 中，由上层把 SEND_FAILED 升级为结束连接，
             * 随后换代会连同这批旧代次数据一起丢弃。 */
            result.status = VOICE_UPLINK_PUMP_SEND_FAILED;
            break;
        }
        if (!voice_uplink_ring_consume(pump->ring)) {
            /* peek 成功而 consume 失败说明配对已被破坏，继续发送会重复或错位。 */
            result.status = VOICE_UPLINK_PUMP_RING_ERROR;
            break;
        }
        result.frames_sent++;
        backlog = voice_uplink_ring_count(pump->ring);
        update_catchup_peak(pump, backlog);
    }

    result.remaining_frames = voice_uplink_ring_count(pump->ring);
    if (result.status != VOICE_UPLINK_PUMP_OK) return result;
    /* 用发送后的剩余积压立即再判一次阈值（与进入条件同一阈值）：仍不低于阈值就继续
     * 保持追赶状态，低于阈值留给下面的退出分支收尾，本轮不再多发。 */
    start_catchup(pump, result.remaining_frames,
                  pump->ops.now_us(pump->ops.ctx), &result);
    update_catchup_peak(pump, result.remaining_frames);
    if (pump->catchup_active &&
        result.remaining_frames < pump->config.catchup_threshold_frames) {
        /* 退出追赶：peak 与 drain 只在这段追赶结束时上报一次，随后复位状态。 */
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
