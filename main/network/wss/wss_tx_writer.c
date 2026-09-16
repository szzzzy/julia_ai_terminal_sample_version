/**
 * @file wss_tx_writer.c
 * @brief 有界 TLS 写入策略的纯 C 实现。
 */
#include "wss_tx_writer.h"

#include <string.h>

static void finish_stats(const wss_tx_writer_ops_t *ops,
                         wss_tx_write_stats_t *stats)
{
    stats->finished_us = ops->now_us(ops->ctx);
}

wss_tx_write_result_t wss_tx_write_all(const wss_tx_writer_ops_t *ops,
                                        const uint8_t *data, size_t len,
                                        int64_t deadline_us,
                                        wss_tx_write_stats_t *stats)
{
    if (stats == NULL) return WSS_TX_WRITE_FATAL;
    memset(stats, 0, sizeof(*stats));
    stats->first_transient_us = -1; /* 哨兵：表示本次调用尚未出现暂时错误。 */

    if (ops == NULL || ops->write == NULL || ops->now_us == NULL ||
        ops->wait_once == NULL || ops->is_transient == NULL ||
        (len > 0U && data == NULL)) {
        return WSS_TX_WRITE_FATAL;
    }
    stats->started_us = ops->now_us(ops->ctx);

    while (stats->bytes_sent < len) {
        if (ops->should_abort != NULL && ops->should_abort(ops->ctx)) {
            finish_stats(ops, stats);
            return WSS_TX_WRITE_ABORTED;
        }
        /* deadline_us 是绝对时刻：帧头与载荷分两次调用时共享同一截止时间。 */
        if (ops->now_us(ops->ctx) >= deadline_us) {
            finish_stats(ops, stats);
            return WSS_TX_WRITE_TIMEOUT;
        }

        size_t remaining = len - stats->bytes_sent;
        int system_error = 0;
        int result = ops->write(ops->ctx, data + stats->bytes_sent,
                                remaining, &system_error);
        stats->last_result = result;
        stats->last_system_error = system_error;

        if (result > 0) {
            if ((size_t)result > remaining) {
                /* 写出字节数超过请求长度说明底层 I/O 契约已被破坏，offset 不再可信，
                 * 因此直接判 FATAL，而不是当作可重试的异常继续推进。 */
                finish_stats(ops, stats);
                return WSS_TX_WRITE_FATAL;
            }
            stats->bytes_sent += (size_t)result;
            /* 每段进展之后都回到期限判断，避免"每步都有进展"把一帧无限拖长。 */
            continue;
        }

        if (!ops->is_transient(ops->ctx, result, system_error)) {
            /* 调用方判定的永久错误立即失败，不进入让出重试；上层据此把会话判为故障并重连。 */
            finish_stats(ops, stats);
            return WSS_TX_WRITE_FATAL;
        }

        /* 暂时错误先记账再判期限，超时返回时 stats 仍能反映真实重试过程。 */
        stats->last_transient_result = result;
        stats->last_transient_system_error = system_error;
        int64_t now_us = ops->now_us(ops->ctx);
        if (stats->first_transient_us < 0) {
            stats->first_transient_us = now_us;
        }
        stats->transient_retries++;
        if (now_us >= deadline_us) {
            finish_stats(ops, stats);
            return WSS_TX_WRITE_TIMEOUT;
        }
        ops->wait_once(ops->ctx);
    }

    finish_stats(ops, stats);
    return WSS_TX_WRITE_OK;
}
