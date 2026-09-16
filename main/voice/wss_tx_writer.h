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

/**
 * I/O 与时钟注入点。ctx 原样回传给每个回调且不提供任何并发保护，因此同一份 ops
 * 不得被多个任务同时使用（生产端只在 WSS 会话任务内串行调用）。
 */
typedef struct {
    void *ctx;
    wss_tx_writer_write_fn_t write;
    wss_tx_writer_now_fn_t now_us; /**< 单调时钟，单位 us；与 deadline_us 同一时间基准。 */
    wss_tx_writer_wait_fn_t wait_once; /**< 暂时错误且未超期时让出一次调度。 */
    wss_tx_writer_transient_fn_t is_transient;
    wss_tx_writer_abort_fn_t should_abort; /**< 可选；每次底层写入前检查，优先于 deadline 判断。 */
} wss_tx_writer_ops_t;

typedef enum {
    WSS_TX_WRITE_OK = 0, /**< 请求区间已全部写出。 */
    WSS_TX_WRITE_FATAL, /**< 永久错误，或底层声称写出的字节数超过请求长度；不重试。 */
    WSS_TX_WRITE_TIMEOUT, /**< 绝对期限耗尽；已写出的字节数保留在 stats.bytes_sent 中。 */
    WSS_TX_WRITE_ABORTED, /**< should_abort 要求终止；本次未完成，但不代表链路故障。 */
} wss_tx_write_result_t;

typedef struct {
    size_t bytes_sent; /**< 已确认写出的字节数。 */
    uint32_t transient_retries; /**< 暂时错误次数。 */
    int last_result; /**< 最后一次底层 write 的返回值（含成功路径）。 */
    int last_system_error; /**< 最后一次底层 write 的 errno。 */
    int last_transient_result; /**< 最后一次暂时错误的返回值。 */
    int last_transient_system_error; /**< 最后一次暂时错误的 errno。 */
    int64_t started_us; /**< 进入写入循环时的单调时钟（us）。 */
    int64_t first_transient_us; /**< 首次暂时错误的时刻（us）；-1 表示本次调用尚未发生。 */
    int64_t finished_us; /**< 返回前的单调时钟（us）。 */
} wss_tx_write_stats_t;

/**
 * 写完一个连续字节区间，暂时错误保持当前 data+offset/remaining 不变。
 *
 * deadline_us 是绝对时刻（us，与 now_us 同一时间基准），帧头与载荷可以分两次调用
 * 本函数并共享同一截止时间，从而限制整帧而不是单次调用的耗时；已进入的底层 write
 * 不能被中断，因此实际返回时间允许多出一次底层调用的耗时。
 *
 * 判定顺序：should_abort 先于期限；永久错误立即 FATAL；暂时错误先记入 stats 再判
 * 期限，未超期才 wait_once 让出。
 *
 * @note last_* 在每条返回路径上都会更新，last_transient_* 只在暂时错误时更新；失败
 *       诊断需要按 transient_retries 是否为 0 在两者之间选择，不能混用。
 */
wss_tx_write_result_t wss_tx_write_all(const wss_tx_writer_ops_t *ops,
                                        const uint8_t *data, size_t len,
                                        int64_t deadline_us,
                                        wss_tx_write_stats_t *stats);
