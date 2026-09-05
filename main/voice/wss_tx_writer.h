/**
 * @file wss_tx_writer.h
 * @brief 可注入 I/O 与时钟的有界 TLS 写入策略。
 *
 * 本模块不依赖 ESP-IDF。生产端注入 esp_tls_conn_write、单调时钟和一次调度让出；
 * 主机测试注入确定性返回序列，直接验证部分写、WANT_READ/WANT_WRITE、绝对截止
 * 时间以及写入偏移。调用方负责判断哪些底层返回值属于暂时错误。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*wss_tx_writer_write_fn_t)(void *ctx, const uint8_t *data,
                                        size_t len, int *system_error);
typedef int64_t (*wss_tx_writer_now_fn_t)(void *ctx);
typedef void (*wss_tx_writer_wait_fn_t)(void *ctx);
typedef bool (*wss_tx_writer_transient_fn_t)(void *ctx, int result,
                                             int system_error);
typedef bool (*wss_tx_writer_abort_fn_t)(void *ctx);

typedef struct {
    void *ctx;
    wss_tx_writer_write_fn_t write;
    wss_tx_writer_now_fn_t now_us;
    wss_tx_writer_wait_fn_t wait_once;
    wss_tx_writer_transient_fn_t is_transient;
    wss_tx_writer_abort_fn_t should_abort; /**< 可选；在后续写入前响应 owner 的终止请求。 */
} wss_tx_writer_ops_t;

typedef enum {
    WSS_TX_WRITE_OK = 0,
    WSS_TX_WRITE_FATAL,
    WSS_TX_WRITE_TIMEOUT,
    WSS_TX_WRITE_ABORTED,
} wss_tx_write_result_t;

typedef struct {
    size_t bytes_sent;
    uint32_t transient_retries;
    int last_result;
    int last_system_error;
    int last_transient_result;
    int last_transient_system_error;
    int64_t started_us;
    int64_t first_transient_us;
    int64_t finished_us;
} wss_tx_write_stats_t;

/**
 * 写完一个连续字节区间，暂时错误保持当前 data+offset/remaining 不变。
 * deadline_us 限制后续调用/重试，可由帧头和载荷共享；已进入的底层 write
 * 不能被中断，因此实际返回时间允许多出一次底层调用的耗时。
 */
wss_tx_write_result_t wss_tx_write_all(const wss_tx_writer_ops_t *ops,
                                        const uint8_t *data, size_t len,
                                        int64_t deadline_us,
                                        wss_tx_write_stats_t *stats);
