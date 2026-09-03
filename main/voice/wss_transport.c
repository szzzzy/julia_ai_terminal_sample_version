/**
 * @file    wss_transport.c
 * @brief   建立设备到语音服务器的加密连接，并保证每条消息完整、有序地收发。
 *
 * 设备只保留一个负责连接的任务。其它任务不能直接读写网络，只能提交待发送内容；
 * 这样可以避免麦克风、文件和控制消息同时写入时互相穿插。连接异常后，本模块会
 * 完成当前不可拆分的操作、释放连接并自动重试。
 *
 * 以下技术约束用于说明这些业务保证如何实现：
 * - 使用 IDF v5.x esp-tls API：esp_tls_init() + esp_tls_conn_new_sync()（返回 1/-1，
 *   句柄由调用者持有），不使用旧版返回指针的 esp_tls_conn_new()；
 * - WebSocket 帧按 RFC 6455 手写：客户端帧强制掩码（4 字节随机 key，载荷逐字节
 *   XOR），服务端帧不掩码，长度支持 126/127 扩展，服务端分片消息跨帧重组；
 *   服务端帧违反协议（RSV 位、掩码、控制帧超长、64 位长度最高位、非最短长度
 *   编码、保留操作码）时以 1002 协议错误关闭；文本消息非法 UTF-8 时以 1007
 *   关闭；收到 CLOSE 会先校验载荷（1 字节载荷、非法状态码、非法 UTF-8 reason
 *   均按 1002 处理），合法时回送 CLOSE（RFC 6455 5.5.1）；
 * - 所有 socket 读写收敛到唯一会话任务；外部调用者只把不透明条目放入有界队列，
 *   避免多任务并发访问 mbedTLS 会话；
 * - 升级握手严格校验：状态行 101、Upgrade/Connection 头，并以请求 nonce 计算和
 *   比对 Sec-WebSocket-Accept；HTTP 头之后同一次 TLS 读取带回的首个 WebSocket 帧
 *   字节会被保留并优先消费，绝不丢弃；
 * - HTTP 升级完成后把空闲读取超时收紧到 20 ms（SO_RCVTIMEO）、写超时收紧到
 *   WSS_WRITE_TIMEOUT_MS（SO_SNDTIMEO）并启用 TCP keepalive，会话循环在超时
 *   窗口内处理排队条目；空闲本身不是故障：客户端按
 *   CONFIG_WSS_KEEPALIVE_INTERVAL_SECONDS 周期发送 PING；发出 PING 后只要在
 *   CONFIG_WSS_PONG_TIMEOUT_SECONDS 内收到任意下行帧（PONG 或其他帧都能证明
 *   链路存活）就取消待定探测并重置保活计时，只有该窗口内完全没有下行帧才判定
 *   链路死亡并重连自愈；
 *   写方向的 WANT_READ/WANT_WRITE/EAGAIN 在同一帧 2 秒绝对期限内保持原参数
 *   重试；期限耗尽或永久错误才结束会话，既容忍短时背压也避免永久卡死；
 * - Bearer token 优先取 COMM_DEVICE_AUTH_TOKEN_VALUE，为空时回退 CONFIG_WSS_TOKEN；
 * - 服务器证书仍由 server_certs/ca_cert.pem 内嵌信任锚校验，不引入新证书。
 *
 * 本文件不知道收到的内容代表回答声音还是文件；语音服务负责解释消息并决定
 * 何时开始上传、播放或发送文件。
 *
 * ------------------------------------------------------------------
 * 连接流程（只由负责连接的任务执行）
 * ------------------------------------------------------------------
 * 其它任务只能提交请求，不能直接改变连接：
 *
 *   [未连接] --wss_connect() 成功--> [已连接/会话中]
 *       ^                              |
 *       | wss_connect() 失败          wss_run_session() 内任一失败路径：
 *       |                              · 帧写失败（含 PING/应答/CLOSE 应答）
 *       |                              · 非法/超长/掩码/UTF-8/CLOSE 帧（1002/1007）
 *       |                              · 对端 CLOSE（回送后退出）
 *       |                              · PING 后 CONFIG_WSS_PONG_TIMEOUT_SECONDS
 *       |                                内无任何下行帧（判死）
 *       |                              · 外部任务请求 owner 以明确原因结束会话
 *       |                              v
 *       +-----[重连等待 vTaskDelay(RECONNECT_INTERVAL)]---→ 重新 wss_connect()
 *
 * 状态归属：
 * - 未连接：wss_transport_start() 刚创建任务，或上一次会话销毁句柄之后。
 * - 连接中：wss_connect() 内执行 esp_tls 同步握手 + HTTP 升级；失败会销毁
 *   会话句柄并落到重连等待，绝不留半开句柄。
 * - 已连接：进入 wss_run_session()，循环"收一帧 → 保活/分派 → 有界处理上行"。
 * - 重连等待：固定 CONFIG_WSS_RECONNECT_INTERVAL_SECONDS 退避后无条件重试；
 *   计数永不累加、永不放弃，因此链路中断无需外部干预即可自愈。
 *
 * 数据去向（这里只保证消息传输，不解释业务含义）：
 * - 上行：外部任务 wss_transport_enqueue() → s_cmd_queue → 会话任务
 *   wss_drain_queue() → 上层的 on_queue_item 回调；上层决定发送何种帧，再在
 *   回调内调用 wss_transport_send_now() → wss_ws_send()。会话任务还可能在
 *   空闲时主动发 PING（0x9），并应答服务端 PING（PONG 0xA）与 CLOSE。
 * - 下行：wss_ws_recv() 收帧 → 跨帧重组（FIN=0 续帧）→ on_text / on_binary
 *   回调交给上层。上层不返回前，会话循环暂停，期间仍可再发帧。
 * - FILE_SEND（上行推送）：BEGIN FILE/Binary/END 的字节含义在上层
 *   voice_service.c；本模块只保证"作为若干连续且不超限的 WebSocket 帧写出去"，
 *   不解释 URI、不分块文件。
 * - MIC PCM 位于上层 PSRAM ring，由 on_poll 在会话任务内调用 send_now 发送；
 *   ring overflow 只跨任务提交结束请求，TLS 仍由本任务独占 teardown。
 *
 * 多任务协作要求：
 * - s_tls、s_rx_extra*、s_msg_*（分片重组）、s_session_failed 只在会话任务
 *   上下文中读写，天然无需锁。mbedTLS 会话句柄绝不跨任务共享，是"收发全部
 *   收敛到会话任务"的根本原因。
 * - s_started / s_starting / s_session_ready / s_requested_end_reason 由
 *   s_start_lock 保护，允许普通任务提交结束请求但不直接修改 owner 状态。
 * - s_cmd_queue 是跨任务的有界通道：外部任务入队（非阻塞），会话任务出队。
 * - 会话任务只在 recv/send 上分别受 20ms 读取超时和 2s 帧写期限约束；
 *   控制分派和上层 on_poll 也必须保持有界。
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "psa/crypto.h"

#include "wss_transport.h"
#include "wss_tx_writer.h"

static const char *TAG = "wss_transport";

/** WebSocket 帧头最长字节数（2 基础 + 8 扩展长度 + 4 掩码 key）。 */
#define WSS_FRAME_HDR_SIZE 16
/** 握手请求/响应缓冲大小。 */
#define WSS_REQ_BUF_SIZE 512
#define WSS_HANDSHAKE_BUF_SIZE 1024
/** 连接与 TLS 握手的总超时；连接成功后会单独收紧空闲读取超时。 */
#define WSS_CONNECT_TIMEOUT_MS 8000
/** HTTP 升级响应的读取超时。握手不能使用会话级的短超时。 */
#define WSS_HANDSHAKE_READ_TIMEOUT_MS 1000
/**
 * 会话空闲读取超时：同时是排队命令的最长处理延迟。
 *
 * 板级 MIC 每 20 ms 产生一帧 PCM1；命令队列深度只有 4。若在一次空闲
 * recv 上阻塞 1 s，队列会在 80 ms 内写满并丢掉几乎全部上行音频。因此
 * 会话超时必须不大于一帧周期。
 */
#define WSS_READ_TIMEOUT_MS 20
/** 单次会话写调用的 SO_SNDTIMEO；暂时错误仍由帧级绝对期限约束重试。 */
#define WSS_WRITE_TIMEOUT_MS 500
/** 单个 WebSocket 帧的头与载荷共享同一个绝对写入期限。 */
#define WSS_FRAME_WRITE_DEADLINE_MS 2000
/** 帧内读取允许的连续空闲超时次数，超过即判定链路故障，防止中途死亡挂死。 */
#define WSS_READ_EAGAIN_BUDGET 5
/** HTTP 升级响应读取允许的连续空闲超时次数。 */
#define WSS_HANDSHAKE_EAGAIN_BUDGET 10

/**
 * @brief 内嵌服务器根证书符号，由 main/CMakeLists.txt 的 EMBED_TXTFILES 生成。
 *
 * 证书只读，不由本文件释放或修改。
 */
extern const unsigned char ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const unsigned char ca_cert_pem_end[] asm("_binary_ca_cert_pem_end");

/** 防止两个调用方同时启动连接；重复启动不会再创建任务或队列。 */
static portMUX_TYPE s_start_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
static bool s_starting;
static bool s_session_ready;
/** 整个程序唯一负责建立连接、收发消息和处理待发送请求的任务。 */
static TaskHandle_t s_session_task;
/** 其它任务提交给语音连接的普通待发送请求；容量固定，满时明确拒绝。 */
static QueueHandle_t s_cmd_queue;
static QueueHandle_t s_control_queue;
/** 当前加密连接，只允许负责连接的任务访问。 */
static esp_tls_t *s_tls;
/** TLS 连接配置；cacert 指针在 wss_transport_start() 中一次性填充。 */
static esp_tls_cfg_t s_tls_cfg;
/** 启动配置副本：回调与队列尺寸。 */
static wss_transport_config_t s_config;
/** 队列条目接收缓冲，容量等于 queue_item_size，由启动时分配。 */
static void *s_queue_item;
/** 表示当前连接已经不能安全继续；本轮处理结束后统一关闭并重连。 */
static bool s_session_failed;
/**
 * 服务器可能把升级响应和第一条 WebSocket 消息一起发来。响应头之后的字节必须
 * 留给消息解析，不能因握手完成而丢弃，否则第一条业务消息会缺失或错位。
 */
static uint8_t s_rx_extra[WSS_HANDSHAKE_BUF_SIZE];
static size_t s_rx_extra_len;
static size_t s_rx_extra_pos;
/** 服务端分片消息重组状态（RFC 6455 允许数据消息跨帧分片），仅由会话任务读写。 */
static uint8_t s_msg_payload[WSS_TRANSPORT_MAX_PAYLOAD + 1];
static size_t s_msg_len;
static uint8_t s_msg_opcode;
static bool s_msg_active;
/** 当前结束原因和跨任务请求；前者仅 owner 写，后者由 s_start_lock 保护。 */
static wss_transport_end_reason_t s_session_end_reason;
static wss_transport_end_reason_t s_last_write_end_reason;
static wss_transport_end_reason_t s_requested_end_reason;

/* 会话诊断计数仅由 WSS 会话任务读写，用于在断链瞬间还原现场。 */
static int64_t s_session_started_us;
static int64_t s_last_rx_us;
static int64_t s_last_tx_us;
static uint64_t s_tx_frames;
static uint64_t s_tx_payload_bytes;
static uint64_t s_rx_frames;

static void wss_set_owner_end_reason(wss_transport_end_reason_t reason)
{
    if (s_session_end_reason == WSS_TRANSPORT_END_NONE &&
        reason > WSS_TRANSPORT_END_NONE &&
        reason < WSS_TRANSPORT_END_REASON_COUNT) {
        s_session_end_reason = reason;
    }
}

static bool wss_take_requested_end(wss_transport_end_reason_t *reason)
{
    bool requested = false;
    portENTER_CRITICAL(&s_start_lock);
    if (s_requested_end_reason != WSS_TRANSPORT_END_NONE) {
        *reason = s_requested_end_reason;
        s_requested_end_reason = WSS_TRANSPORT_END_NONE;
        requested = true;
    }
    portEXIT_CRITICAL(&s_start_lock);
    return requested;
}

/** 在销毁 TLS 句柄前记录一次有界现场快照，不在正常音频路径刷日志。 */
static void wss_log_session_probe(const char *reason)
{
    int64_t now_us = esp_timer_get_time();
    int64_t session_ms = s_session_started_us > 0
                             ? (now_us - s_session_started_us) / 1000LL
                             : -1;
    int64_t rx_age_ms = s_last_rx_us > 0 ? (now_us - s_last_rx_us) / 1000LL : -1;
    int64_t tx_age_ms = s_last_tx_us > 0 ? (now_us - s_last_tx_us) / 1000LL : -1;
    UBaseType_t audio_queued = s_cmd_queue != NULL ? uxQueueMessagesWaiting(s_cmd_queue) : 0;
    UBaseType_t control_queued =
        s_control_queue != NULL ? uxQueueMessagesWaiting(s_control_queue) : 0;
    wifi_ap_record_t ap_info;
    int rssi = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK ? ap_info.rssi : INT_MIN;

    ESP_LOGW(TAG,
             "WSS probe reason=%s session_ms=%" PRIi64
             " rx_age_ms=%" PRIi64 " tx_age_ms=%" PRIi64
             " tx_frames=%" PRIu64 " tx_payload_bytes=%" PRIu64
             " rx_frames=%" PRIu64 " q_data=%u q_control=%u",
             reason, session_ms, rx_age_ms, tx_age_ms, s_tx_frames,
             s_tx_payload_bytes, s_rx_frames, (unsigned)audio_queued,
             (unsigned)control_queued);
    ESP_LOGW(TAG,
             "WSS probe resources free_heap=%u min_free_heap=%u internal_free=%u "
             "internal_min=%u internal_largest=%u stack_hwm_words=%u rssi=%d",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL), rssi);
}

/** 读取并清除 ESP-TLS 保存的最后错误；仅在写失败、会话即将重建时调用。 */
static void wss_log_tls_write_probe(const char *failure_kind,
                                    const char *phase, int opcode,
                                    size_t frame_payload_len, int result,
                                    int saved_errno, bool would_block,
                                    size_t sent, size_t requested,
                                    uint32_t retries, int64_t elapsed_us)
{
    int tls_system_error = 0;
    int tls_mbedtls_error = 0;
    int tls_esp_error = 0;
    esp_tls_error_handle_t error_handle = NULL;
    if (s_tls != NULL &&
        esp_tls_get_error_handle(s_tls, &error_handle) == ESP_OK &&
        error_handle != NULL) {
        (void)esp_tls_get_and_clear_error_type(
            error_handle, ESP_TLS_ERR_TYPE_SYSTEM, &tls_system_error);
        (void)esp_tls_get_and_clear_error_type(
            error_handle, ESP_TLS_ERR_TYPE_MBEDTLS, &tls_mbedtls_error);
        (void)esp_tls_get_and_clear_error_type(
            error_handle, ESP_TLS_ERR_TYPE_ESP, &tls_esp_error);
    }

    ESP_LOGE(TAG,
             "WSS TLS write probe kind=%s phase=%s opcode=0x%x frame_payload=%u "
             "progress=%u/%u result=%d errno=%d would_block=%u "
             "retries=%" PRIu32 " elapsed_ms=%" PRIi64 " "
             "tls_system=%d tls_mbedtls=-0x%x tls_esp=0x%x",
             failure_kind, phase, opcode, (unsigned)frame_payload_len, (unsigned)sent,
             (unsigned)requested, result, saved_errno, (unsigned)would_block,
             retries, elapsed_us >= 0 ? elapsed_us / 1000LL : -1,
             tls_system_error,
             tls_mbedtls_error < 0 ? -tls_mbedtls_error : tls_mbedtls_error,
             tls_esp_error);
    wss_log_session_probe("tls_write_failure");
}

/** esp-tls 在超时时可能返回 WANT_READ/WANT_WRITE，也可能保留 socket errno。 */
static bool wss_tls_would_block(int result)
{
    return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE ||
           (result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

static int wss_tls_write_once(void *ctx, const uint8_t *data, size_t len,
                              int *system_error)
{
    (void)ctx;
    errno = 0;
    int result = s_tls != NULL
                     ? esp_tls_conn_write(s_tls, (const char *)data, len)
                     : ESP_FAIL;
    *system_error = errno;
    return result;
}

static int64_t wss_tls_write_now_us(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}

static void wss_tls_write_wait_once(void *ctx)
{
    (void)ctx;
    /* 一个调度 tick 避免暂时不可写时忙等；帧级绝对期限负责保证有界。 */
    vTaskDelay(1);
}

static bool wss_tls_write_is_transient(void *ctx, int result,
                                       int system_error)
{
    (void)ctx;
    return result == ESP_TLS_ERR_SSL_WANT_READ ||
           result == ESP_TLS_ERR_SSL_WANT_WRITE ||
           (result == -1 &&
            (system_error == EAGAIN || system_error == EWOULDBLOCK));
}

static const wss_tx_writer_ops_t s_tls_writer_ops = {
    .ctx = NULL,
    .write = wss_tls_write_once,
    .now_us = wss_tls_write_now_us,
    .wait_once = wss_tls_write_wait_once,
    .is_transient = wss_tls_write_is_transient,
};

/** 设置 socket 读取超时。HTTP 升级和实时会话使用不同的时间预算。 */
static void wss_set_receive_timeout(int sockfd, unsigned timeout_ms)
{
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000U),
        .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
    };
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGW(TAG, "setsockopt(SO_RCVTIMEO=%u ms) failed", timeout_ms);
    }
}

/* ------------------------------------------------------------------ */
/* 基础 TLS 字节流读写                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief 循环写出完整数据块。
 *
 * TLS 是字节流，单次 write 可能只完成一部分；本函数保证要么全部写出，
 * 要么返回失败由调用者结束会话。WANT_READ、WANT_WRITE 和 socket EAGAIN
 * 保持当前地址及剩余长度不变，在整个 WebSocket 帧共享的绝对期限内重试；
 * 只有期限耗尽或永久错误才结束会话。
 *
 * @param[in] data 数据首地址，不允许为 NULL。
 * @param[in] len  数据长度。
 * @return ESP_OK 全部写出；ESP_FAIL 会话无效或写出失败。
 */
static esp_err_t wss_tls_write_all(const void *data, size_t len,
                                   const char *phase, int opcode,
                                   size_t frame_payload_len,
                                   int64_t frame_deadline_us)
{
    if (s_tls == NULL) return ESP_FAIL;

    wss_tx_write_stats_t stats;
    wss_tx_write_result_t result = wss_tx_write_all(
        &s_tls_writer_ops, data, len, frame_deadline_us, &stats);
    int64_t elapsed_us = stats.finished_us - stats.started_us;
    if (result == WSS_TX_WRITE_OK) {
        if (stats.transient_retries > 0) {
            ESP_LOGW(TAG,
                     "WSS TLS write recovered phase=%s opcode=0x%x frame_payload=%u "
                     "retries=%" PRIu32 " elapsed_ms=%" PRIi64,
                     phase, opcode, (unsigned)frame_payload_len,
                     stats.transient_retries,
                     elapsed_us >= 0 ? elapsed_us / 1000LL : -1);
        }
        return ESP_OK;
    }

    int probe_result = stats.transient_retries > 0
                           ? stats.last_transient_result : stats.last_result;
    int probe_system_error = stats.transient_retries > 0
                                 ? stats.last_transient_system_error
                                 : stats.last_system_error;
    bool would_block = wss_tls_write_is_transient(
        NULL, probe_result, probe_system_error);
    wss_log_tls_write_probe(
        result == WSS_TX_WRITE_TIMEOUT ? "timeout" : "fatal",
        phase, opcode, frame_payload_len, probe_result,
        probe_system_error, would_block, stats.bytes_sent, len,
        stats.transient_retries, elapsed_us);
    s_last_write_end_reason = result == WSS_TX_WRITE_TIMEOUT
                                  ? WSS_TRANSPORT_END_TX_STALL
                                  : WSS_TRANSPORT_END_TX_ERROR;
    return ESP_FAIL;
}

/**
 * @brief 循环读入完整数据块。
 *
 * 空闲读取超时（EAGAIN）只容忍有限次数：帧中途超时说明对端长时间未补齐数据，
 * 超过预算后按链路故障处理，避免会话任务永久挂起。
 *
 * @param[out] data 接收缓冲区，不允许为 NULL。
 * @param[in]  len  读取长度。
 * @return ESP_OK 已读满；ESP_FAIL 会话无效、EOF 或读取失败。
 */
static esp_err_t wss_tls_read_exact(void *data, size_t len)
{
    size_t got = 0;
    unsigned eagain_budget = WSS_READ_EAGAIN_BUDGET;
    while (got < len) {
        /* 优先消费握手阶段保留下来的首帧余量，再回到 TLS 字节流。 */
        if (s_rx_extra_pos < s_rx_extra_len) {
            size_t avail = s_rx_extra_len - s_rx_extra_pos;
            size_t take = len - got;
            if (take > avail) {
                take = avail;
            }
            memcpy((char *)data + got, s_rx_extra + s_rx_extra_pos, take);
            s_rx_extra_pos += take;
            got += take;
            if (s_rx_extra_pos >= s_rx_extra_len) {
                s_rx_extra_pos = 0;
                s_rx_extra_len = 0;
            }
            continue;
        }
        if (s_tls == NULL) {
            return ESP_FAIL;
        }
        int n = esp_tls_conn_read(s_tls, (char *)data + got, len - got);
        if (n == 0) {
            return ESP_FAIL;
        }
        if (n < 0) {
            if (wss_tls_would_block(n) && eagain_budget > 0) {
                eagain_budget--;
                continue;
            }
            return ESP_FAIL;
        }
        got += (size_t)n;
        eagain_budget = WSS_READ_EAGAIN_BUDGET;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* WebSocket 客户端帧（RFC 6455）                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief 发送一帧 WebSocket 消息。
 *
 * 客户端帧必须掩码：随机 4 字节 key，载荷逐字节 XOR；长度按 126/127 扩展编码。
 * 本函数是发送路径的公共入口（保活 PING、PING 应答、CLOSE 应答与公开的
 * wss_transport_send_now 都经过这里），因此在此统一校验帧合法性：
 * opcode 只能是已定义值；控制帧载荷不得超过 125 字节；载荷非空时必须提供
 * 有效缓冲。
 *
 * @param[in] opcode  帧操作码（0x0 续帧、0x1 文本、0x2 二进制、0x8 CLOSE、
 *                    0x9 PING、0xA PONG；保留值拒绝）。
 * @param[in] payload 载荷首地址，len 为 0 时可为 NULL。
 * @param[in] len     载荷长度，不允许超过 WSS_TRANSPORT_MAX_PAYLOAD；
 *                    控制帧不允许超过 125。
 * @return ESP_OK 发送完成。
 * @return ESP_ERR_INVALID_ARG 保留操作码、控制帧超长、载荷超长或载荷非空但
 *         payload 为 NULL。
 * @return ESP_FAIL 会话无效或写出失败。
 */
static esp_err_t wss_ws_send(uint8_t opcode, const uint8_t *payload, size_t len)
{
    s_last_write_end_reason = WSS_TRANSPORT_END_NONE;
    if (s_tls == NULL) {
        s_last_write_end_reason = WSS_TRANSPORT_END_TX_ERROR;
        return ESP_FAIL;
    }
    switch (opcode) {                   /* RFC 6455 5.2：只允许已定义的操作码 */
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x8:
    case 0x9:
    case 0xA:
        break;
    default:
        ESP_LOGW(TAG, "Refusing to send WS frame with reserved opcode 0x%x", opcode);
        return ESP_ERR_INVALID_ARG;
    }
    if ((opcode & 0x08) != 0 && len > 125) {    /* RFC 6455 5.5：控制帧 ≤125 字节 */
        ESP_LOGW(TAG, "Refusing to send oversized WS control frame (%u bytes)",
                 (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }
    if (len > WSS_TRANSPORT_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Refusing to send oversized WS frame (%u bytes)", (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }
    if (len > 0 && payload == NULL) {
        ESP_LOGW(TAG, "Refusing to send WS frame with NULL payload");
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hdr[WSS_FRAME_HDR_SIZE];
    size_t h = 0;
    hdr[h++] = 0x80 | opcode;

    uint8_t mask_key[4];
    esp_fill_random(mask_key, sizeof(mask_key));

    if (len < 126) {
        hdr[h++] = 0x80 | (uint8_t)len;
    } else if (len <= 0xFFFF) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)(len & 0xFF);
    } else {
        hdr[h++] = 0x80 | 127;
        uint64_t l = len;
        for (int i = 7; i >= 0; i--) {
            hdr[h++] = (uint8_t)(l >> (i * 8));
        }
    }
    memcpy(hdr + h, mask_key, sizeof(mask_key)); /* 客户端帧必须掩码 */
    h += sizeof(mask_key);

    int64_t frame_deadline_us = esp_timer_get_time() +
                                (int64_t)WSS_FRAME_WRITE_DEADLINE_MS * 1000LL;
    if (wss_tls_write_all(hdr, h, "ws_header", opcode, len,
                          frame_deadline_us) != ESP_OK) {
        return ESP_FAIL;
    }
    if (len > 0) {
        /* 掩码缓冲为静态复用，避免每帧在栈上分配 1200 B。安全前提：所有调用方
         * 都在会话任务上下文中（未连前 s_tls 为空直接失败，会话仅由该任务推进；
         * 公共 send_now 也只允许在回调内调用），故无需加锁；任何跨任务直接调用
         * wss_ws_send 都会破坏这一假定。 */
        static uint8_t masked[WSS_TRANSPORT_MAX_PAYLOAD];
        for (size_t i = 0; i < len; i++) {
            masked[i] = payload[i] ^ mask_key[i % 4];
        }
        if (wss_tls_write_all(masked, len, "ws_payload", opcode, len,
                              frame_deadline_us) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    s_last_tx_us = esp_timer_get_time();
    s_tx_frames++;
    s_tx_payload_bytes += len;
    return ESP_OK;
}

/**
 * @brief 发送 CLOSE 帧（RFC 6455 5.5.1）。
 *
 * 客户端帧自动掩码；code 为 0 时发送空载荷 CLOSE（未携带状态码），否则携带
 * 2 字节网络序状态码。
 *
 * @param[in] code 状态码（主机字节序），0 表示不带状态码。
 * @return ESP_OK 发送完成；ESP_FAIL 会话无效或写出失败。
 */
static esp_err_t wss_send_close(uint16_t code)
{
    uint8_t payload[2];
    size_t len = 0;
    if (code != 0) {
        payload[0] = (uint8_t)(code >> 8);
        payload[1] = (uint8_t)(code & 0xFF);
        len = sizeof(payload);
    }
    return wss_ws_send(0x8, payload, len);
}

static void wss_send_protocol_close(uint16_t code)
{
    wss_set_owner_end_reason(WSS_TRANSPORT_END_PROTOCOL_ERROR);
    (void)wss_send_close(code);
}

/**
 * @brief 回送服务端的完整 CLOSE 载荷，并有限等待对端关闭底层连接。
 *
 * RFC 6455 建议关闭响应回显收到的状态码；这里直接回显完整合法载荷，兼容会校验
 * reason 的服务端。写成功后不能立刻销毁带未读数据的 socket，否则部分 TCP 栈会
 * 以 RST 结束连接，使已排队的 CLOSE 对端不可见。等待时间受 20 ms 接收超时和
 * CONFIG_WSS_CLOSE_WAIT_MS 双重约束，不会卡住重连任务。
 */
static esp_err_t wss_reply_close_and_wait(const uint8_t *payload, size_t len)
{
    esp_err_t err = wss_ws_send(0x8, payload, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WSS CLOSE reply send failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WSS CLOSE reply sent (%u-byte payload)", (unsigned)len);
    int64_t deadline_us = esp_timer_get_time() +
                          (int64_t)CONFIG_WSS_CLOSE_WAIT_MS * 1000LL;
    uint8_t discard[32];
    while (s_tls != NULL && esp_timer_get_time() < deadline_us) {
        int n = esp_tls_conn_read(s_tls, discard, sizeof(discard));
        if (n == 0) {
            ESP_LOGI(TAG, "WSS peer completed close handshake");
            return ESP_OK;
        }
        if (n < 0 && !wss_tls_would_block(n)) {
            ESP_LOGW(TAG, "WSS peer shutdown read failed after CLOSE reply");
            return ESP_FAIL;
        }
    }
    ESP_LOGW(TAG, "WSS peer did not close within %d ms; forcing transport cleanup",
             CONFIG_WSS_CLOSE_WAIT_MS);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 接收一帧 WebSocket 消息。
 *
 * 服务端帧不得掩码；RFC 6455 分片帧（FIN=0）不再在此处拒绝，由会话循环跨帧
 * 重组。只把帧首字节的空闲超时报告给调用者（idle_out），让会话循环把它当作
 * 队列处理与保活窗口；帧中途失败一律按链路故障返回。握手阶段保留的余量会
 * 被优先消费，因此不会产生空闲超时。
 *
 * @param[out] opcode_out 帧操作码。
 * @param[out] payload    载荷缓冲区，容量 cap 字节。
 * @param[in]  cap        载荷缓冲区容量。
 * @param[out] len_out    载荷长度。
 * @param[out] idle_out   是否只是等待帧首字节的空闲超时。
 * @param[out] fin_out    该帧是否携带 FIN（即是否为消息的最后一帧）。
 * @return ESP_OK 收到一帧（idle_out=false）或空闲超时（idle_out=true）；
 * @return ESP_FAIL 链路故障或非法帧。
 */
static esp_err_t wss_ws_recv(uint8_t *opcode_out, uint8_t *payload, size_t cap,
                             size_t *len_out, bool *idle_out, bool *fin_out)
{
    *idle_out = false;
    *fin_out = false;
    if (opcode_out == NULL || payload == NULL || len_out == NULL || fin_out == NULL ||
        s_tls == NULL) {
        return ESP_FAIL;
    }

    uint8_t first;
    if (s_rx_extra_pos < s_rx_extra_len) {
        /* 握手余量里已有帧字节：直接取用，不走 socket 读（不产生空闲超时）。 */
        first = s_rx_extra[s_rx_extra_pos++];
        if (s_rx_extra_pos >= s_rx_extra_len) {
            s_rx_extra_pos = 0;
            s_rx_extra_len = 0;
        }
    } else {
        int n = esp_tls_conn_read(s_tls, &first, 1);
        if (n == 0) {
            return ESP_FAIL;
        }
        if (n < 0) {
            if (wss_tls_would_block(n)) {
                *idle_out = true;
                return ESP_OK;
            }
            return ESP_FAIL;
        }
    }

    uint8_t hdr[2];
    hdr[0] = first;
    if (wss_tls_read_exact(hdr + 1, 1) != ESP_OK) {
        return ESP_FAIL;
    }
    uint8_t op = hdr[0] & 0x0F;
    *fin_out = (hdr[0] & 0x80) != 0;
    if ((hdr[0] & 0x70) != 0) {             /* RSV1-3：未协商任何扩展 */
        ESP_LOGW(TAG, "WS frame with unsupported RSV extension bits rejected");
        wss_send_protocol_close(1002);
        return ESP_FAIL;
    }
    /* 扩展长度先按 uint64_t 解析（ESP32 的 size_t 只有 32 位，直接左移写入
     * size_t 会让恶意超长声明截断，随后按错误长度读取载荷造成协议流错位）。 */
    uint64_t len64 = hdr[1] & 0x7F;
    if (len64 == 126) {
        uint8_t b[2];
        if (wss_tls_read_exact(b, sizeof(b)) != ESP_OK) {
            return ESP_FAIL;
        }
        len64 = ((uint64_t)b[0] << 8) | b[1];
        if (len64 < 126) {                  /* RFC 6455 5.2：长度必须使用最短编码 */
            ESP_LOGW(TAG, "WS frame with non-minimal length encoding rejected");
            wss_send_protocol_close(1002);
            return ESP_FAIL;
        }
    } else if (len64 == 127) {
        uint8_t b[8];
        if (wss_tls_read_exact(b, sizeof(b)) != ESP_OK) {
            return ESP_FAIL;
        }
        if ((b[0] & 0x80) != 0) {           /* RFC 6455：64 位长度最高位必须为 0 */
            ESP_LOGW(TAG, "WS frame with high bit set in 64-bit length rejected");
            wss_send_protocol_close(1002);
            return ESP_FAIL;
        }
        len64 = 0;
        for (int i = 0; i < 8; i++) {
            len64 = (len64 << 8) | b[i];
        }
        if (len64 <= 0xFFFF) {              /* RFC 6455 5.2：长度必须使用最短编码 */
            ESP_LOGW(TAG, "WS frame with non-minimal length encoding rejected");
            wss_send_protocol_close(1002);
            return ESP_FAIL;
        }
    }
    if ((hdr[1] & 0x80) != 0) {             /* RFC 6455：服务端帧不得掩码 */
        ESP_LOGW(TAG, "Masked server WS frame rejected");
        wss_send_protocol_close(1002);
        return ESP_FAIL;
    }
    if (op >= 0x8 && len64 > 125) {         /* RFC 6455：控制帧载荷不得超过 125 */
        ESP_LOGW(TAG, "Oversized WS control frame (%" PRIu64 ")", len64);
        wss_send_protocol_close(1002);
        return ESP_FAIL;
    }
    /* 读取载荷前先把超长声明拒掉，再安全收窄为 size_t。 */
    if (len64 > cap) {
        ESP_LOGW(TAG, "Oversized WS frame (%" PRIu64 ")", len64);
        wss_send_protocol_close(1002);
        return ESP_FAIL;
    }
    size_t len = (size_t)len64;
    if (wss_tls_read_exact(payload, len) != ESP_OK) {
        return ESP_FAIL;
    }
    *opcode_out = op;
    *len_out = len;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* TLS 连接 + HTTP 升级握手                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 解析 WSS Bearer token：优先设备通用 token，为空时回退 WSS 专用配置。
 *
 * @return token 字符串指针，可能为空串。
 */
static const char *wss_token(void)
{
#if defined(CONFIG_COMM_DEVICE_AUTH_TOKEN_VALUE)
    if (CONFIG_COMM_DEVICE_AUTH_TOKEN_VALUE[0] != '\0') {
        return CONFIG_COMM_DEVICE_AUTH_TOKEN_VALUE;
    }
#endif
    return CONFIG_WSS_TOKEN;
}

/** 大小写不敏感的 ASCII 等长比较。 */
static bool wss_ascii_ci_equal(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca += 'a' - 'A';
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 'a' - 'A';
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

/** 判断头部行的字段名是否等于给定名称（大小写不敏感，容忍名字后空白）。 */
static bool wss_header_name_is(const char *line, size_t name_len, const char *name)
{
    while (name_len > 0 && (line[name_len - 1] == ' ' || line[name_len - 1] == '\t')) {
        name_len--;
    }
    return name_len == strlen(name) && wss_ascii_ci_equal(line, name, name_len);
}

/**
 * @brief 判断头部值中是否含有给定逗号分隔 token（大小写不敏感）。
 *
 * 例如 Connection: keep-alive, Upgrade 必须视为包含 Upgrade。
 *
 * @param[in] value     头部值首地址。
 * @param[in] value_len 头部值长度（不含行尾 CRLF）。
 * @param[in] token     目标 token，不允许为 NULL。
 * @return true 值中存在该 token；false 不存在。
 */
static bool wss_value_has_token(const char *value, size_t value_len, const char *token)
{
    size_t token_len = strlen(token);
    size_t pos = 0;
    while (pos < value_len) {
        while (pos < value_len && (value[pos] == ',' || value[pos] == ' ' || value[pos] == '\t')) {
            pos++;
        }
        size_t start = pos;
        while (pos < value_len && value[pos] != ',') {
            pos++;
        }
        size_t tok_len = pos - start;
        while (tok_len > 0 &&
               (value[start + tok_len - 1] == ' ' || value[start + tok_len - 1] == '\t')) {
            tok_len--;
        }
        if (tok_len == token_len && wss_ascii_ci_equal(value + start, token, tok_len)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 以请求 nonce 计算并比对 Sec-WebSocket-Accept。
 *
 * accept = base64(SHA1(nonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))，
 * 与响应头逐字节比较，杜绝任何未认证的伪造升级响应。
 *
 * @param[in] key_b64   请求中发送的 Sec-WebSocket-Key（base64 文本）。
 * @param[in] value     响应头 Sec-WebSocket-Accept 的值。
 * @param[in] value_len 响应头值的长度。
 * @return true 计算值与响应一致；false 不一致或计算失败。
 */
static bool wss_ws_accept_matches(const char *key_b64, const char *value, size_t value_len)
{
    static const char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t key_len = strlen(key_b64);
    char input[64 + sizeof(magic)];
    if (key_len >= sizeof(input) - sizeof(magic) + 1) {
        return false;
    }
    memcpy(input, key_b64, key_len);
    memcpy(input + key_len, magic, sizeof(magic) - 1);

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (info == NULL) {
        return false;
    }
    uint8_t digest[20];
    /* PSA 可能尚未初始化；psa_crypto_init() 幂等，重复调用代价可忽略。 */
    (void)psa_crypto_init();
    if (mbedtls_md(info, (const unsigned char *)input,
                   key_len + sizeof(magic) - 1, digest) != 0) {
        return false;
    }
    uint8_t b64[32];
    size_t out_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &out_len, digest, sizeof(digest)) != 0) {
        return false;
    }
    return out_len == value_len && memcmp(b64, value, value_len) == 0;
}

/**
 * @brief 严格校验 RFC 6455 升级响应。
 *
 * 状态行必须恰为 "HTTP/1.x 101"；Upgrade: websocket、Connection 含 Upgrade
 * token；Sec-WebSocket-Accept 必须与请求 nonce 的计算值一致。
 *
 * @param[in] resp      响应头部首地址。
 * @param[in] resp_len  响应头部长度（含末尾 "\r\n\r\n"）。
 * @param[in] key_b64   请求中的 Sec-WebSocket-Key。
 * @return true 升级响应合法；false 任何一项不满足。
 */
static bool wss_ws_validate_response(const char *resp, size_t resp_len, const char *key_b64)
{
    static const char status_1_1[] = "HTTP/1.1 101";
    static const char status_1_0[] = "HTTP/1.0 101";
    if (resp_len < sizeof(status_1_1) ||
        !((memcmp(resp, status_1_1, sizeof(status_1_1) - 1) == 0 &&
           (resp[sizeof(status_1_1) - 1] == ' ' || resp[sizeof(status_1_1) - 1] == '\r')) ||
          (memcmp(resp, status_1_0, sizeof(status_1_0) - 1) == 0 &&
           (resp[sizeof(status_1_0) - 1] == ' ' || resp[sizeof(status_1_0) - 1] == '\r')))) {
        return false;
    }
    const char *line_end = strstr(resp, "\r\n");
    if (line_end == NULL) {
        return false;
    }

    bool has_upgrade = false;
    bool has_connection = false;
    bool has_accept = false;
    bool accept_ok = false;
    const char *p = line_end + 2;
    while (p < resp + resp_len) {
        const char *eol = strstr(p, "\r\n");
        if (eol == NULL) {
            return false;
        }
        size_t line_len = (size_t)(eol - p);
        if (line_len == 0) {
            break;
        }
        const char *colon = memchr(p, ':', line_len);
        if (colon == NULL) {
            return false;
        }
        size_t name_len = (size_t)(colon - p);
        const char *val = colon + 1;
        while (val < eol && (*val == ' ' || *val == '\t')) {
            val++;
        }
        size_t val_len = (size_t)(eol - val);
        while (val_len > 0 && (val[val_len - 1] == ' ' || val[val_len - 1] == '\t')) {
            val_len--;
        }
        if (wss_header_name_is(p, name_len, "Upgrade")) {
            has_upgrade = val_len == strlen("websocket") &&
                          wss_ascii_ci_equal(val, "websocket", val_len);
        } else if (wss_header_name_is(p, name_len, "Connection")) {
            has_connection = wss_value_has_token(val, val_len, "Upgrade");
        } else if (wss_header_name_is(p, name_len, "Sec-WebSocket-Accept")) {
            has_accept = true;
            accept_ok = wss_ws_accept_matches(key_b64, val, val_len);
        }
        p = eol + 2;
    }
    return has_upgrade && has_connection && has_accept && accept_ok;
}

/**
 * @brief 在已建立的 TLS 连接上完成 WebSocket HTTP 升级握手。
 *
 * 随机 16 字节 Sec-WebSocket-Key 经 base64 编码，请求头携带
 * Authorization: Bearer <token>；按 RFC 6455 严格校验升级响应，并把
 * 响应头之后同一读取带回的首帧字节保留给帧解析。
 *
 * @return ESP_OK 升级成功；ESP_FAIL 请求构造、写出或响应校验失败。
 */
static esp_err_t wss_ws_handshake(void)
{
    if (s_tls == NULL) {
        return ESP_FAIL;
    }

    uint8_t rnd[16];
    uint8_t keyb64[32];
    size_t outlen = 0;
    esp_fill_random(rnd, sizeof(rnd));
    if (mbedtls_base64_encode(keyb64, sizeof(keyb64), &outlen, rnd, sizeof(rnd)) != 0 ||
        outlen >= sizeof(keyb64)) {
        return ESP_FAIL;
    }
    keyb64[outlen] = '\0';

    const char *token = wss_token();
    if (token[0] == '\0') {
        ESP_LOGW(TAG, "WSS token is empty; server may reject the upgrade");
    }

    char req[WSS_REQ_BUF_SIZE];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "Authorization: Bearer %s\r\n"
                     "\r\n",
                     CONFIG_WSS_PATH, CONFIG_WSS_SERVER_HOST, CONFIG_WSS_SERVER_PORT,
                     keyb64, token);
    if (n <= 0 || (size_t)n >= sizeof(req)) {
        ESP_LOGE(TAG, "WSS upgrade request too long");
        return ESP_FAIL;
    }
    int64_t handshake_write_deadline_us = esp_timer_get_time() +
        (int64_t)WSS_FRAME_WRITE_DEADLINE_MS * 1000LL;
    if (wss_tls_write_all(req, (size_t)n, "http_upgrade", -1,
                          (size_t)n, handshake_write_deadline_us) != ESP_OK) {
        return ESP_FAIL;
    }

    char buf[WSS_HANDSHAKE_BUF_SIZE];
    size_t got = 0;
    unsigned eagain_budget = WSS_HANDSHAKE_EAGAIN_BUDGET;
    const char *hdr_end = NULL;
    while (got < sizeof(buf) - 1) {
        int r = esp_tls_conn_read(s_tls, buf + got, sizeof(buf) - 1 - got);
        if (r == 0) {
            return ESP_FAIL;
        }
        if (r < 0) {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && eagain_budget > 0) {
                eagain_budget--;
                continue;
            }
            return ESP_FAIL;
        }
        got += (size_t)r;
        buf[got] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end != NULL) {
            break;
        }
    }
    if (hdr_end == NULL) {
        ESP_LOGE(TAG, "WSS handshake failed: response headers incomplete or too long");
        return ESP_FAIL;
    }
    size_t header_len = (size_t)(hdr_end - buf) + 4;
    /* 一次 TLS 读取可能同时带回 HTTP 头与首个 WebSocket 帧：头结束后的余量
     * 必须保留给帧解析，丢弃会导致后续解析错位并断线。 */
    if (got > header_len) {
        memcpy(s_rx_extra, buf + header_len, got - header_len);
        s_rx_extra_len = got - header_len;
        s_rx_extra_pos = 0;
    }
    if (!wss_ws_validate_response(buf, header_len, (const char *)keyb64)) {
        ESP_LOGE(TAG, "WSS handshake failed: invalid RFC 6455 upgrade response: %.*s",
                 (int)header_len, buf);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 建立 TLS 连接并完成 WebSocket 升级握手。
 *
 * @return true 连接并升级成功，s_tls 已指向有效会话；
 * @return false 连接、握手或升级失败，资源已释放。
 */
static bool wss_connect(void)
{
    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        return false;
    }
    const char *host = CONFIG_WSS_SERVER_HOST;
    int ret = esp_tls_conn_new_sync(host, (int)strlen(host), CONFIG_WSS_SERVER_PORT,
                                    &s_tls_cfg, tls);
    if (ret != 1) {
        ESP_LOGW(TAG, "WSS TLS connect failed (%d)", ret);
        (void)esp_tls_conn_destroy(tls);
        return false;
    }

    /* TCP 与 TLS 握手已完成。HTTP 升级仍使用较长读取超时；升级成功后再
     * 收紧到一帧周期，避免实时 MIC 队列在 recv 中积压。 */
    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(tls, &sockfd) == ESP_OK && sockfd >= 0) {
        wss_set_receive_timeout(sockfd, WSS_HANDSHAKE_READ_TIMEOUT_MS);
        /* 单次 send 由 SO_SNDTIMEO 限制；返回暂时错误后仍保持同一写入区间，
         * 由 WebSocket 帧共享的 2 秒绝对期限限制总重试时间。 */
        struct timeval wtv;
        wtv.tv_sec = WSS_WRITE_TIMEOUT_MS / 1000;
        wtv.tv_usec = (WSS_WRITE_TIMEOUT_MS % 1000) * 1000;
        if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &wtv, sizeof(wtv)) != 0) {
            ESP_LOGW(TAG, "setsockopt(SO_SNDTIMEO) failed; writes may block longer");
        }
        /* TCP keepalive 与应用层 Ping/Pong 互补：半开链路可被底层探测提前发现。 */
        int keepalive = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) != 0) {
            ESP_LOGW(TAG, "setsockopt(SO_KEEPALIVE) failed");
        }
    }

    /* 新会话开始前清空上一会话可能残留的握手余量。 */
    s_rx_extra_len = 0;
    s_rx_extra_pos = 0;

    s_tls = tls;
    if (wss_ws_handshake() != ESP_OK) {
        (void)esp_tls_conn_destroy(s_tls);
        s_tls = NULL;
        return false;
    }
    if (sockfd >= 0) {
        wss_set_receive_timeout(sockfd, WSS_READ_TIMEOUT_MS);
    }
    ESP_LOGI(TAG, "WSS connected & authenticated (wss://%s:%d%s)",
             CONFIG_WSS_SERVER_HOST, CONFIG_WSS_SERVER_PORT, CONFIG_WSS_PATH);
    return true;
}

/* ------------------------------------------------------------------ */
/* 会话任务与命令队列                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief 排空外部命令队列（只负责把不透明条目交给上层回调）。
 *
 * 每条条目在会话任务上下文中执行；上层回调内可直接调用 wss_transport_send_now()。
 */
static void wss_drain_queue(void)
{
    /* Bounded batches leave receive/ping/poll a turn even under PCM pressure. */
    QueueHandle_t queues[] = {s_control_queue, s_cmd_queue};
    for (size_t q = 0; q < 2 && !s_session_failed; ++q) {
        for (unsigned n = 0; n < 4; ++n) {
            if (xQueueReceive(queues[q], s_queue_item, 0) != pdTRUE) break;
            s_config.on_queue_item(s_queue_item, s_config.queue_item_size);
            if (s_session_failed) break;
        }
    }
}

static bool wss_apply_requested_end(void)
{
    wss_transport_end_reason_t reason = WSS_TRANSPORT_END_NONE;
    if (!wss_take_requested_end(&reason)) return false;
    wss_set_owner_end_reason(reason);
    ESP_LOGW(TAG, "WSS owner accepted session end request: %s",
             wss_transport_end_reason_name(reason));
    return true;
}

/** 接收一轮之后处理有界上行批次和业务轮询，防止上行压力遮住对端 CLOSE。 */
static bool wss_service_outbound(void)
{
    if (wss_apply_requested_end()) return false;
    wss_drain_queue();
    if (wss_apply_requested_end()) return false;
    if (!s_session_failed && s_config.on_poll != NULL) {
        s_config.on_poll();
    }
    if (wss_apply_requested_end()) return false;
    if (s_session_failed) {
        wss_set_owner_end_reason(WSS_TRANSPORT_END_APPLICATION_ERROR);
        ESP_LOGW(TAG, "WSS session failed during outbound handling: %s",
                 wss_transport_end_reason_name(s_session_end_reason));
        return false;
    }
    return true;
}

/**
 * @brief 校验文本载荷是否为合法 UTF-8（RFC 3629，RFC 6455 8.1 的要求）。
 *
 * 拒绝：孤立续字节、过短/过长编码、U+D800-DFFF 代理区、超过 U+10FFFF 的码点。
 *
 * @param[in] data 载荷首地址。
 * @param[in] len  载荷长度。
 * @return true 合法 UTF-8；false 非法。
 */
static bool wss_text_is_valid_utf8(const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t b = data[i];
        if (b < 0x80) {
            i++;
        } else if ((b & 0xE0) == 0xC0) {
            /* 2 字节序列：0xC2-0xDF + 续字节（0xC0/0xC1 是过短编码） */
            if (b < 0xC2 || i + 1 >= len || (data[i + 1] & 0xC0) != 0x80) {
                return false;
            }
            i += 2;
        } else if ((b & 0xF0) == 0xE0) {
            /* 3 字节序列：排除过短（E0 80-9F）与代理区（ED A0-BF） */
            if (i + 2 >= len ||
                (data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80) {
                return false;
            }
            if ((b == 0xE0 && data[i + 1] < 0xA0) || (b == 0xED && data[i + 1] >= 0xA0)) {
                return false;
            }
            i += 3;
        } else if ((b & 0xF8) == 0xF0) {
            /* 4 字节序列：排除过短（F0 80-8F）、F5-FF 与超过 U+10FFFF（F4 90-BF） */
            if (b > 0xF4 || i + 3 >= len ||
                (data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80 ||
                (data[i + 3] & 0xC0) != 0x80) {
                return false;
            }
            if ((b == 0xF0 && data[i + 1] < 0x90) || (b == 0xF4 && data[i + 1] >= 0x90)) {
                return false;
            }
            i += 4;
        } else {
            return false;                   /* 0x80-0xBF 孤立续字节 */
        }
    }
    return true;
}

/**
 * @brief 判断 CLOSE 帧状态码是否合法（RFC 6455 7.4 与 IANA WebSocket 注册表）。
 *
 * 合法：1000-1014 中除保留码 1004（保留）、1005（无状态码）、1006（异常关闭）
 * 之外的已注册码，以及 3000-4999（应用/私有用途）。其余值（<1000、
 * 1016-2999 未注册段、1015）视为非法，收到时必须按协议错误关闭。
 *
 * @param[in] code 状态码。
 * @return true 合法；false 非法。
 */
static bool wss_close_code_is_valid(uint16_t code)
{
    if (code >= 3000 && code <= 4999) {
        return true;
    }
    if (code >= 1000 && code <= 1014) {
        return code != 1004 && code != 1005 && code != 1006;
    }
    return false;
}

/**
 * @brief 运行一次完整的 WSS 会话，直到链路故障、对端关闭或保活超时。
 *
 * 循环结构：先接收一帧，再有界处理命令队列；帧首字节的空闲超时是队列处理与
 * 保活窗口。接收优先使持续 MIC 上行不能遮住服务端 CLOSE。空闲本身不代表断线：服务端静默达到
 * CONFIG_WSS_KEEPALIVE_INTERVAL_SECONDS 秒后客户端主动发 PING；发出 PING 后
 * 只要在 CONFIG_WSS_PONG_TIMEOUT_SECONDS 内收到任意下行帧（PONG 或其他帧都
 * 证明链路存活）就取消待定探测并重置保活计时，只有该窗口内完全没有下行帧才
 * 判定链路死亡。服务端分片消息（RFC 6455）跨帧重组后再交给上层回调。
 */
static void wss_run_session(void)
{
    /* 会话级状态复位：写失败标志、分片重组进度与保活计时。 */
    s_session_failed = false;
    s_session_end_reason = WSS_TRANSPORT_END_NONE;
    s_last_write_end_reason = WSS_TRANSPORT_END_NONE;
    s_msg_active = false;
    s_msg_len = 0;
    int64_t last_rx_us = esp_timer_get_time();
    int64_t last_ping_us = last_rx_us;
    bool pong_pending = false;
    s_session_started_us = last_rx_us;
    s_last_rx_us = last_rx_us;
    s_last_tx_us = last_rx_us;
    s_tx_frames = 0;
    s_tx_payload_bytes = 0;
    s_rx_frames = 0;

    /* Do not replay audio or control requests left by a disconnected session. */
    xQueueReset(s_cmd_queue);
    xQueueReset(s_control_queue);
    portENTER_CRITICAL(&s_start_lock);
    s_requested_end_reason = WSS_TRANSPORT_END_NONE;
    s_session_ready = true;
    portEXIT_CRITICAL(&s_start_lock);

    if (s_config.on_session_start != NULL) {
        s_config.on_session_start();
    }

    while (s_tls != NULL && !s_session_failed) {
        (void)ulTaskNotifyTake(pdTRUE, 0);
        if (wss_apply_requested_end()) break;
        /* 先收一帧再处理上行：持续 MIC 上传时也要优先看到服务端 CLOSE，避免服务端
         * 停止读取后，本机先因排队 PCM 写失败而跳过关闭握手。空闲读取仅阻塞 20ms。 */
        uint8_t op = 0;
        /* 会话任务是唯一使用者，因此可复用静态缓冲以避免占用任务栈。 */
        static uint8_t frame_payload[WSS_TRANSPORT_MAX_PAYLOAD + 1];
        size_t len = 0;
        bool idle = false;
        bool fin = true;
        if (wss_ws_recv(&op, frame_payload, sizeof(frame_payload) - 1,
                        &len, &idle, &fin) != ESP_OK) {
            wss_set_owner_end_reason(WSS_TRANSPORT_END_RX_ERROR);
            ESP_LOGW(TAG, "WSS receive failed; closing session");
            break;
        }
        if (wss_apply_requested_end()) break;

        if (idle) {
            /* 空闲窗口：处理上行和主动保活，不把空闲当作断线。 */
            if (!wss_service_outbound()) {
                break;
            }
            int64_t now_us = esp_timer_get_time();
            if (pong_pending) {
                int64_t pong_timeout_us = (int64_t)CONFIG_WSS_PONG_TIMEOUT_SECONDS * 1000000LL;
                if (now_us - last_ping_us >= pong_timeout_us) {
                    wss_set_owner_end_reason(WSS_TRANSPORT_END_KEEPALIVE_TIMEOUT);
                    ESP_LOGW(TAG, "WSS keepalive: no downlink frame within %d s after PING; reconnecting",
                             CONFIG_WSS_PONG_TIMEOUT_SECONDS);
                    break;
                }
            } else {
                int64_t keepalive_us = (int64_t)CONFIG_WSS_KEEPALIVE_INTERVAL_SECONDS * 1000000LL;
                if (now_us - last_rx_us >= keepalive_us) {
                    if (wss_ws_send(0x9, NULL, 0) != ESP_OK) {
                        wss_set_owner_end_reason(
                            s_last_write_end_reason != WSS_TRANSPORT_END_NONE
                                ? s_last_write_end_reason
                                : WSS_TRANSPORT_END_TX_ERROR);
                        ESP_LOGW(TAG, "WSS keepalive PING send failed; reconnecting");
                        break;
                    }
                    last_ping_us = now_us;
                    pong_pending = true;
                }
            }
            continue;
        }

        /* 有下行帧：链路确认存活；取消待定 PONG 探测并重置保活计时。RFC 6455
         * 只要求对 PING 回 PONG，但任何下行帧都能证明链路可用，因此不限于 PONG。 */
        last_rx_us = esp_timer_get_time();
        s_last_rx_us = last_rx_us;
        s_rx_frames++;
        pong_pending = false;

        if (op >= 0x8) {                    /* 控制帧：CLOSE/PING/PONG */
            if (!fin || len > 125) {
                ESP_LOGW(TAG, "Invalid WS control frame (fragmented or oversized)");
                wss_send_protocol_close(1002);
                break;
            }
            if (op == 0x9) {                /* PING -> PONG */
                if (wss_ws_send(0xA, frame_payload, len) != ESP_OK) {
                    wss_set_owner_end_reason(
                        s_last_write_end_reason != WSS_TRANSPORT_END_NONE
                            ? s_last_write_end_reason
                            : WSS_TRANSPORT_END_TX_ERROR);
                    break;
                }
                if (!wss_service_outbound()) break;
                continue;
            }
            if (op == 0x8) {                /* CLOSE：先校验载荷，再回送 CLOSE（RFC 6455 5.5.1） */
                uint16_t close_code = 0;
                if (len == 1) {             /* RFC 6455 5.5.1：CLOSE 载荷要么为空要么 ≥2 字节 */
                    ESP_LOGW(TAG, "Invalid WS CLOSE frame (1-byte payload); closing with 1002");
                    wss_send_protocol_close(1002);
                    break;
                }
                if (len >= 2) {
                    close_code = (uint16_t)(((uint16_t)frame_payload[0] << 8) |
                                            frame_payload[1]);
                    if (!wss_close_code_is_valid(close_code)) {
                        ESP_LOGW(TAG, "Invalid WS CLOSE status code 0x%04X; closing with 1002",
                                 close_code);
                        wss_send_protocol_close(1002);
                        break;
                    }
                    /* RFC 6455 5.5.1：状态码之后的 reason 必须是合法 UTF-8 */
                    if (!wss_text_is_valid_utf8(frame_payload + 2, len - 2)) {
                        ESP_LOGW(TAG, "WS CLOSE reason is not valid UTF-8; closing with 1002");
                        wss_send_protocol_close(1002);
                        break;
                    }
                }
                ESP_LOGI(TAG, "WSS server sent CLOSE (code 0x%04X); replying CLOSE", close_code);
                wss_set_owner_end_reason(WSS_TRANSPORT_END_PEER_CLOSE);
                portENTER_CRITICAL(&s_start_lock);
                s_session_ready = false;    /* 拒绝关闭等待期间产生的新上行条目。 */
                portEXIT_CRITICAL(&s_start_lock);
                (void)wss_reply_close_and_wait(frame_payload, len);
                break;
            }
            if (op != 0xA) {                /* 保留控制操作码 0xB-0xF：协议错误（RFC 6455 5.5） */
                ESP_LOGW(TAG, "Reserved WS control opcode 0x%x rejected", op);
                wss_send_protocol_close(1002);
                break;
            }
            if (!wss_service_outbound()) break;
            continue;
        }

        /* 数据帧：RFC 6455 允许分片，跨帧重组后再处理完整消息。 */
        if (s_msg_active) {
            if (op != 0x0) {                /* 重组过程中不允许新消息帧 */
                ESP_LOGW(TAG, "New data frame during fragmented message; closing session");
                wss_send_protocol_close(1002);
                break;
            }
        } else {
            if (op == 0x0) {
                ESP_LOGW(TAG, "Continuation frame without a message start; closing session");
                wss_send_protocol_close(1002);
                break;
            }
            if (op != 0x1 && op != 0x2) {   /* 保留数据操作码 0x3-0x7：协议错误（RFC 6455 5.5） */
                ESP_LOGW(TAG, "Reserved WS data opcode 0x%x rejected", op);
                wss_send_protocol_close(1002);
                break;
            }
            s_msg_active = true;
            s_msg_opcode = op;
            s_msg_len = 0;
        }
        if (len > WSS_TRANSPORT_MAX_PAYLOAD - s_msg_len) {
            ESP_LOGW(TAG, "Reassembled WS message exceeds payload cap");
            wss_send_protocol_close(1002);
            break;
        }
        memcpy(s_msg_payload + s_msg_len, frame_payload, len);
        s_msg_len += len;
        if (!fin) {
            continue;
        }

        uint8_t msg_opcode = s_msg_opcode;
        size_t msg_len = s_msg_len;
        s_msg_active = false;
        s_msg_len = 0;
        if (msg_opcode == 0x1) {
            /* RFC 6455 8.1：文本消息必须携带合法 UTF-8，否则以 1007 失败连接。 */
            if (!wss_text_is_valid_utf8(s_msg_payload, msg_len)) {
                ESP_LOGW(TAG, "WS text message is not valid UTF-8; closing with 1007");
                wss_send_protocol_close(1007);
                break;
            }
            if (s_config.on_text != NULL) {
                s_config.on_text(s_msg_payload, msg_len);
                if (s_session_failed) {
                    break;
                }
            }
        } else if (msg_opcode == 0x2) {
            if (s_config.on_binary != NULL) {
                s_config.on_binary(s_msg_payload, msg_len);
                if (s_session_failed) {
                    break;
                }
            }
        }
        /* 服务端其他数据帧无下行用途，直接忽略；每个接收轮次后再处理上行。 */
        if (!wss_service_outbound()) {
            break;
        }
    }

    portENTER_CRITICAL(&s_start_lock);
    s_session_ready = false;
    portEXIT_CRITICAL(&s_start_lock);
    wss_transport_end_reason_t pending_reason = WSS_TRANSPORT_END_NONE;
    if (wss_take_requested_end(&pending_reason)) {
        wss_set_owner_end_reason(pending_reason);
    }
    if (s_session_end_reason == WSS_TRANSPORT_END_NONE) {
        wss_set_owner_end_reason(s_session_failed
                                     ? WSS_TRANSPORT_END_APPLICATION_ERROR
                                     : WSS_TRANSPORT_END_RX_ERROR);
    }
    wss_log_session_probe(wss_transport_end_reason_name(s_session_end_reason));
    (void)esp_tls_conn_destroy(s_tls);
    s_tls = NULL;
    s_msg_active = false;
    s_msg_len = 0;
    /* 会话结束通知：在销毁句柄、s_tls 置空之后、重连等待之前调用，仍处于会话任务
     * 上下文。上层借此复位会话级业务状态（如关闭 MIC 流、停止扬声器）；此时网络
     * 已不可用，任何基于"会话仍健康"的发送都会失败，属预期。 */
    if (s_config.on_session_end != NULL) {
        s_config.on_session_end(s_session_end_reason);
    }
    xQueueReset(s_cmd_queue);
    xQueueReset(s_control_queue);
    ESP_LOGW(TAG, "WSS session ended reason=%s; reconnecting in %d s",
             wss_transport_end_reason_name(s_session_end_reason),
             CONFIG_WSS_RECONNECT_INTERVAL_SECONDS);
}

/**
 * @brief 会话任务主体：连接 -> 会话 -> 退避 -> 重连，永不退出。
 *
 * 这是连接状态机的驱动器：wss_connect() 对应"连接中"阶段，wss_run_session() 对应
 * "已连接/会话中"阶段，两者之间与每次会话结束之后的 vTaskDelay(RECONNECT_INTERVAL)
 * 对应"重连等待"阶段。循环无退出条件，也无需退出条件——客户端生命周期与设备一致；
 * 参数未使用。
 *
 * @param[in] parameter FreeRTOS 任务参数，本实现未使用。
 */
static void wss_session_task(void *parameter)
{
    (void)parameter;
    for (;;) {
        if (wss_connect()) {
            wss_run_session();
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)CONFIG_WSS_RECONNECT_INTERVAL_SECONDS * 1000U));
    }
}

/* ------------------------------------------------------------------ */
/* 公共接口                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 一次性启动 WSS 客户端：分配条目缓冲、命令队列，并创建唯一会话任务。
 *
 * 资源（s_queue_item、s_cmd_queue、s_session_task）只在首次成功时分配；之后无论
 * 网络生命周期如何重试，重复调用都直接返回 ESP_OK 复用既有实例，绝不会出现第二
 * 份任务或队列——这正是参数校验之后立刻就做幂等判定的原因。
 *
 * 任务创建成功后不再等待连接结果：wss_session_task() 自行连接并在失败/会话结束后
 * 按配置退避重连。本函数不阻塞，可被任意普通任务（如 IP 就绪回调）在取得 IPv4 后
 * 调用；不允许在中断上下文调用。
 *
 * @return ESP_OK 已启动或已处于启动完成态。
 * @return ESP_ERR_INVALID_ARG 配置为空，或缺 on_queue_item / 空尺寸 / 空深度。
 * @return ESP_ERR_INVALID_STATE 已有一次启动仍在进行中（尚未到达 finish）。
 * @return ESP_ERR_NO_MEM 条目缓冲、队列或任务创建失败。
 */
esp_err_t wss_transport_start(const wss_transport_config_t *config)
{
    if (config == NULL || config->on_queue_item == NULL ||
        config->queue_item_size == 0U || config->queue_depth == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_start_lock);
    if (s_started) {
        portEXIT_CRITICAL(&s_start_lock);
        return ESP_OK;
    }
    if (s_starting) {
        portEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_starting = true;
    portEXIT_CRITICAL(&s_start_lock);

    /* 信任锚与超时配置只需填充一次；每次重连复用同一配置。
     * 自签服务器（如 172.20.10.2）：证书链由 ca_cert.pem 校验，
     * 服务器证书 CN 是 "Voice Robot Local CA"，与主机名不匹配，
     * 跳过 CN（Identity)检查——身份已由信任锚链绑定。
     * 若服务器证书含该 IP 的 SAN，可移除本行。 */
    s_tls_cfg.cacert_buf = ca_cert_pem_start;
    s_tls_cfg.cacert_bytes = (unsigned int)(ca_cert_pem_end - ca_cert_pem_start);
    s_tls_cfg.skip_common_name = true;
    s_tls_cfg.timeout_ms = WSS_CONNECT_TIMEOUT_MS;
    s_config = *config;

    esp_err_t err = ESP_OK;
    if (s_queue_item == NULL) {
        s_queue_item = malloc(config->queue_item_size);
        if (s_queue_item == NULL) {
            err = ESP_ERR_NO_MEM;
            goto finish;
        }
    }
    if (s_cmd_queue == NULL) {
        s_cmd_queue = xQueueCreate(config->queue_depth, config->queue_item_size);
    }
    if (s_cmd_queue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto finish;
    }
    if (s_control_queue == NULL) s_control_queue = xQueueCreate(4, config->queue_item_size);
    if (s_control_queue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto finish;
    }
    if (xTaskCreate(wss_session_task, "wss_transport", (uint32_t)CONFIG_WSS_TASK_STACK_SIZE,
                    NULL, 4, &s_session_task) != pdPASS) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        vQueueDelete(s_control_queue);
        s_control_queue = NULL;
        err = ESP_ERR_NO_MEM;
        goto finish;
    }
    ESP_LOGI(TAG, "WSS transport client started");

finish:
    portENTER_CRITICAL(&s_start_lock);
    s_starting = false;
    s_started = (err == ESP_OK);
    portEXIT_CRITICAL(&s_start_lock);
    return err;
}

/**
 * @brief 把一条不透明命令的"副本"放入有界命令队列，非阻塞。
 *
 * 任意普通任务可调用（MQTT 事件任务、MIC 采集等）；xQueueSend 内部按
 * queue_item_size 整块复制条目，因此入队后调用方的本地缓冲即可复用。队列满时
 * 返回 ESP_ERR_NO_MEM，由调用方决定丢弃策略（如 MIC 帧被丢弃并记日志）；绝不在
 * 队列上阻塞等待空间，避免阻塞住采集任务。
 *
 * @return 见头文件 wss_transport.h：ESP_OK/ESP_ERR_INVALID_ARG/
 *         ESP_ERR_INVALID_SIZE/ESP_ERR_NO_MEM/ESP_ERR_INVALID_STATE。
 */
esp_err_t wss_transport_enqueue(const void *item, size_t item_size)
{
    if (item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_start_lock);
    bool ready = s_session_ready;
    portEXIT_CRITICAL(&s_start_lock);
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (item_size != s_config.queue_item_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (xQueueSend(s_cmd_queue, item, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wss_transport_enqueue_control(const void *item, size_t item_size)
{
    if (item == NULL) return ESP_ERR_INVALID_ARG;
    if (s_control_queue == NULL) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_start_lock);
    bool ready = s_session_ready;
    portEXIT_CRITICAL(&s_start_lock);
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (item_size != s_config.queue_item_size) return ESP_ERR_INVALID_SIZE;
    return xQueueSend(s_control_queue, item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void wss_transport_fail_session(void)
{
    wss_set_owner_end_reason(WSS_TRANSPORT_END_APPLICATION_ERROR);
    s_session_failed = true;
}

esp_err_t wss_transport_request_session_end(wss_transport_end_reason_t reason)
{
    if (reason <= WSS_TRANSPORT_END_NONE ||
        reason >= WSS_TRANSPORT_END_REASON_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    TaskHandle_t owner = NULL;
    portENTER_CRITICAL(&s_start_lock);
    if (!s_session_ready) {
        portEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_requested_end_reason == WSS_TRANSPORT_END_NONE) {
        s_requested_end_reason = reason;
    }
    /* 立即拒绝新控制作业；TLS teardown 仍只能由 owner 执行。 */
    s_session_ready = false;
    owner = s_session_task;
    portEXIT_CRITICAL(&s_start_lock);
    if (owner != NULL) xTaskNotifyGive(owner);
    return ESP_OK;
}

const char *wss_transport_end_reason_name(wss_transport_end_reason_t reason)
{
    switch (reason) {
    case WSS_TRANSPORT_END_NONE: return "none";
    case WSS_TRANSPORT_END_PEER_CLOSE: return "peer_close";
    case WSS_TRANSPORT_END_RX_ERROR: return "rx_error";
    case WSS_TRANSPORT_END_TX_ERROR: return "tx_error";
    case WSS_TRANSPORT_END_TX_STALL: return "tx_stall";
    case WSS_TRANSPORT_END_KEEPALIVE_TIMEOUT: return "keepalive_timeout";
    case WSS_TRANSPORT_END_PROTOCOL_ERROR: return "protocol_error";
    case WSS_TRANSPORT_END_APPLICATION_ERROR: return "application_error";
    case WSS_TRANSPORT_END_AUDIO_OVERFLOW: return "audio_overflow";
    case WSS_TRANSPORT_END_REASON_COUNT:
    default: return "unknown";
    }
}

/**
 * @brief 在会话任务上下文中直接发送一帧 WebSocket 消息（封装 wss_ws_send）。
 *
 * 设计上只允许在 on_text / on_queue_item 回调内调用：这两类回调本身就是会话任务
 * 驱动的，因此与 wss_ws_send 内部的静态掩码缓冲、以及 s_tls 句柄的访问天然串行，
 * 无需额外加锁。多任务并发直接调用 wss_ws_send 不是线程安全的（违背上面约定）。
 *
 * 错误语义区分两类：参数非法（ESP_ERR_INVALID_ARG，由 wss_ws_send 校验返回）不
 * 影响链路；只有会话级写失败（ESP_FAIL，含永久错误或帧写期限耗尽）才置位
 * s_session_failed，让会话循环据此关闭并重连。上层因此在推送文件失败时只需返回
 * 失败，纠错交给传输层。
 */
esp_err_t wss_transport_send_now(uint8_t opcode, const uint8_t *payload, size_t len)
{
    esp_err_t err = wss_ws_send(opcode, payload, len);
    if (err != ESP_OK) {
        /* 只有会话级写失败才标记链路故障并重连；参数非法不是会话故障。 */
        if (err == ESP_FAIL) {
            wss_set_owner_end_reason(
                s_last_write_end_reason != WSS_TRANSPORT_END_NONE
                    ? s_last_write_end_reason
                    : WSS_TRANSPORT_END_TX_ERROR);
            s_session_failed = true;
        }
        return err;
    }
    return ESP_OK;
}
