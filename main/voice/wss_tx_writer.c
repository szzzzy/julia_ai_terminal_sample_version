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
    stats->first_transient_us = -1;

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
        /* 帧头与载荷可以分两次调用本函数，但共享同一个绝对截止时间；因此即使
         * 每次都有少量正向进展，也不能把一个 WebSocket 帧无限拖长。 */
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
                finish_stats(ops, stats);
                return WSS_TX_WRITE_FATAL;
            }
            stats->bytes_sent += (size_t)result;
            continue;
        }

        if (!ops->is_transient(ops->ctx, result, system_error)) {
            finish_stats(ops, stats);
            return WSS_TX_WRITE_FATAL;
        }

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
