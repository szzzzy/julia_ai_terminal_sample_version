/**
 * @file    http_downloader.c
 * @brief   公共 HTTPS 数据面下载器实现。
 *
 * 当前唯一调用方是音频素材下载（`main/audio/audio_engine.c`）；OTA 下载仍在
 * `ota_engine.c` 内自带一套 HTTP 循环，两者各写一份状态码/长度/ETag/Range 策略，
 * 只共用响应头采集与 Content-Range 解析。改这里时必须同步核对 OTA 侧，反之亦然。
 *
 * 本模块负责的连接与校验：
 * - 连接建立与 TLS 失败分类；
 * - Range 断点续传与 200/416 回退、ETag 一致性校验；
 * - HTTP 状态码、Content-Length、Content-Range 校验；
 * - 带空读超时的读取循环与完整 body 判定。
 *
 * 业务差异（写入目标、素材校验、断点检查点、进度上报）通过 sink 回调注入；断点被
 * 重置（ETag 变化或服务器忽略 Range）时通过 restart 回调通知调用方重建持久化记录。
 *
 * 模块关系：
 * - 依赖 esp_http_client 完成传输与 chunked 解码（传输编码对上层透明）；
 * - 依赖 ota_stability 的 Content-Range 解析（下载器再校验区间与清单长度一致）；
 * - 不解析 JSON、不访问 OTA/数据分区、不写 NVS；不重入（见 http_downloader_run）。
 */
#include "http_downloader.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "esp_http_client.h"
#include "esp_tls_errors.h"

#include "ota_stability.h"

static const char *TAG = "http_downloader";

/** 网络读取缓冲区大小，单位为字节；也是 sink 每次回调的最大数据长度。 */
#define HTTP_DOWNLOADER_BUFFER_SIZE 1024

/** 连续空读的上限；每次空读间隔 10 ms，达到后视为网络无响应（约 6 秒）。 */
#define HTTP_DOWNLOADER_MAX_EMPTY_READS 600U

/**
 * 下载缓冲区，单实例：本模块不支持并发或重入，同一时刻只有一个下载任务。
 * sink 返回后其中的数据可能被下一次读取覆盖，回调不得保留该指针。
 */
static char s_buffer[HTTP_DOWNLOADER_BUFFER_SIZE];

esp_err_t http_downloader_collect_headers(esp_http_client_event_t *event)
{
    if (event != NULL && event->event_id == HTTP_EVENT_ON_HEADER)
        download_response_header(event->user_data, event->header_key, event->header_value);
    return ESP_OK;
}

/** 构建系统嵌入的服务器根证书起始地址（main/CMakeLists.txt EMBED_TXTFILES）。 */
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");

/**
 * @brief 保留可靠的 TLS 握手/证书失败原因，不并入通用网络分类。当 ESP-IDF
 * 未提供 TLS 校验证据时，DNS、socket 和超时失败仍归为 NETWORK_TIMEOUT。
 */
static native_ota_failure_reason_t http_downloader_open_failure_reason(
    esp_http_client_handle_t client)
{
    int tls_code = 0;
    int tls_flags = 0;
    esp_err_t tls_err = esp_http_client_get_and_clear_last_tls_error(
        client, &tls_code, &tls_flags);
    if (tls_flags != 0 || tls_err == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED ||
        tls_err == ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED) {
        ESP_LOGE(TAG, "HTTPS TLS verification/handshake failed: esp_err=0x%x tls=0x%x flags=0x%x",
                 (unsigned)tls_err, (unsigned)tls_code, (unsigned)tls_flags);
        return NATIVE_OTA_FAILURE_TLS_VERIFY_FAILED;
    }
    return NATIVE_OTA_FAILURE_NETWORK_TIMEOUT;
}

/**
 * @brief 执行一次带断点续传能力的 HTTPS 数据面下载。
 *
 * 分两阶段：
 * 1. 建连协商阶段（for 循环，最多两次）：初始化并打开 HTTP 客户端，读响应头，
 *    校验状态码 / Content-Length / Content-Range / ETag。若服务器忽略 Range
 *    （返回 200/416）或断点的 ETag 变化，则先调用 restart_cb、把 resume 归零，
 *    然后重新建连全量下载；每次重试都新建客户端，避免复用上一次的连接状态。
 * 2. 读取阶段：循环读 body 块并同步交给 sink，带空读超时；sink 返回错误立即中止，
 *    错误码原样透传（不改写 failure_reason）。
 *
 * 失败统一走 cleanup 释放连接资源；本函数在普通任务上下文运行并阻塞到结束。
 */
esp_err_t http_downloader_run(const http_downloader_config_t *config,
                              http_downloader_result_t *result)
{
    if (config == NULL || result == NULL || config->url == NULL || config->sink == NULL ||
        (config->resume_offset > 0U &&
         (config->expected_size == 0U || config->resume_offset >= config->expected_size))) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->failure_reason = NATIVE_OTA_FAILURE_NONE;

    /* 统一出口的返回值；失败路径先记录错误再 goto cleanup 释放连接。 */
    esp_err_t err_out = ESP_OK;

    /* resume 状态只属于下载器内部：restart 后归零并全量重试。 */
    bool resume = config->resume_offset > 0U;
    size_t resume_offset = config->resume_offset;
    esp_http_client_handle_t client = NULL;
    bool client_open = false;
    download_response_headers_t headers;

    /* 最多允许一次“服务器忽略 Range 后从零重试”，避免把完整对象追加到旧偏移。 */
    for (unsigned http_attempt = 0; http_attempt < 2; ++http_attempt) {
        memset(&headers, 0, sizeof(headers));
        result->etag[0] = '\0';
        /* 每次重试都创建新的 HTTP 客户端，确保上一次连接的响应状态不会被复用。 */
        esp_http_client_config_t http_config = {
            .url = config->url,
            .cert_pem = config->cert_pem != NULL ?
                        config->cert_pem : (const char *)server_cert_pem_start,
            .timeout_ms = config->timeout_ms,
            .keep_alive_enable = true,
            .event_handler = http_downloader_collect_headers,
            .user_data = &headers,
        };
        if (config->skip_cert_common_name_check) {
            http_config.skip_cert_common_name_check = true;
        }

        client = esp_http_client_init(&http_config);
        if (client == NULL) {
            result->failure_reason = NATIVE_OTA_FAILURE_NETWORK_TIMEOUT;
            err_out = ESP_FAIL;
            goto cleanup;
        }
        if (resume) {
            /* Range 起点必须等于已持久化的对象前缀长度，服务器返回的数据才能直接追加。 */
            char range_header[64];
            int range_len = snprintf(range_header, sizeof(range_header), "bytes=%zu-",
                                     resume_offset);
            if (range_len <= 0 || (size_t)range_len >= sizeof(range_header) ||
                esp_http_client_set_header(client, "Range", range_header) != ESP_OK) {
                result->failure_reason = NATIVE_OTA_FAILURE_RANGE_MISMATCH;
                err_out = ESP_FAIL;
                goto cleanup;
            }
        }

        /* 打开连接并发送请求头（write_len=0：GET 无请求体）；失败按 TLS/网络分类。 */
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
            result->failure_reason = http_downloader_open_failure_reason(client);
            err_out = err;
            goto cleanup;
        }
        client_open = true;

        /* content_length 为服务器声明的 body 长度；0 表示未知，常见于 chunked 响应。 */
        int64_t content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "Failed to fetch HTTP headers: %" PRId64, content_length);
            result->failure_reason = NATIVE_OTA_FAILURE_NETWORK_TIMEOUT;
            err_out = ESP_FAIL;
            goto cleanup;
        }
        if (content_length == 0) {
            ESP_LOGW(TAG, "HTTP response has no Content-Length; manifest size and complete-body checks will be used");
        }
        /* status_code 与 content_length 共同决定当前响应是完整下载还是可续传响应。 */
        int status_code = esp_http_client_get_status_code(client);
        /* ETag 与下方 Content-Range 都由 HTTP_EVENT_ON_HEADER 在本次响应内采集
         * （http_downloader_collect_headers），不依赖任何 SDK 编译开关。
         * 服务器不返回 ETag 时 response_etag 为 NULL：带 expected_etag 的续传会比对
         * 不到，于是退化为从零重试；无法确认断点对应的对象是否仍是同一份内容。 */
        if (headers.invalid) {
            result->failure_reason = NATIVE_OTA_FAILURE_HTTP_STATUS_INVALID;
            err_out = ESP_FAIL;
            goto cleanup;
        }
        const char *response_etag = headers.etag[0] != '\0' ? headers.etag : NULL;
        /* 两侧 ETag 容量相同（均为 128 字节），拷贝不会截断，只补一个显式终止符。 */
        if (response_etag != NULL && response_etag[0] != '\0') {
            strncpy(result->etag, response_etag, sizeof(result->etag) - 1U);
            result->etag[sizeof(result->etag) - 1U] = '\0';
        }

        /* 断点对象身份变化或服务器无法续传时，通知调用方重置后全量重试。
         * expected_etag 与响应 ETag 做严格字符串比较，不做弱比较归一化：服务器换了
         * ETag 形式（例如加了 W/ 前缀或引号变化）会被当作对象已替换并重下。
         * expected_etag 为 NULL 或空串表示本次不校验断点对象身份。 */
        bool restart_required = false;
        if (resume && config->expected_etag != NULL && config->expected_etag[0] != '\0' &&
            (response_etag == NULL || strcmp(config->expected_etag, response_etag) != 0)) {
            ESP_LOGW(TAG, "HTTP ETag changed while resuming; restarting from zero");
            restart_required = true;
        } else if (resume && (status_code == 200 || status_code == 416)) {
            ESP_LOGW(TAG, "Server cannot honor Range (status=%d); restarting from zero", status_code);
            restart_required = true;
        }

        if (restart_required) {
            (void)esp_http_client_close(client);
            (void)esp_http_client_cleanup(client);
            client = NULL;
            client_open = false;
            result->restarted = true;
            /* restart_cb 必须在开始新的全量会话之前把持久化断点清零，否则写入偏移仍
             * 指向上一次的部分镜像。回调失败时客户端已释放，可直接返回而不经过 cleanup。 */
            if (config->restart_cb != NULL) {
                esp_err_t restart_err = config->restart_cb(config->restart_ctx);
                if (restart_err != ESP_OK) {
                    return restart_err;
                }
            }
            resume = false;
            resume_offset = 0;
            continue;
        }

        if (resume) {
            if (status_code != 206) {
                ESP_LOGW(TAG, "Range request returned transient/invalid HTTP status=%d", status_code);
                result->failure_reason = NATIVE_OTA_FAILURE_HTTP_STATUS_INVALID;
                err_out = ESP_FAIL;
                goto cleanup;
            }
            if (content_length > 0 &&
                content_length != (int64_t)(config->expected_size - resume_offset)) {
                ESP_LOGE(TAG, "Range response invalid: status=%d length=%" PRId64
                         " expected=%zu", status_code, content_length,
                         config->expected_size - resume_offset);
                result->failure_reason = NATIVE_OTA_FAILURE_RANGE_MISMATCH;
                err_out = ESP_FAIL;
                goto cleanup;
            }
            /* Content-Range 进一步确认响应覆盖的区间和完整对象大小。只接受“终点正好是
             * 完整对象末字节”的单区间：多区间或尾部截断的 206 无法直接追加到断点。 */
            const char *content_range = headers.content_range;
            size_t range_start = 0;
            size_t range_end = 0;
            size_t range_total = 0;
            if (!ota_stability_parse_content_range(content_range, &range_start, &range_end,
                                                   &range_total) ||
                range_start != resume_offset || range_end != config->expected_size - 1U ||
                range_total != config->expected_size) {
                ESP_LOGE(TAG, "Content-Range does not match resume offset");
                result->failure_reason = NATIVE_OTA_FAILURE_RANGE_MISMATCH;
                err_out = ESP_FAIL;
                goto cleanup;
            }
            result->total_size = range_total;
        } else {
            if (status_code != 200) {
                ESP_LOGW(TAG, "Full download request returned transient/invalid HTTP status=%d",
                         status_code);
                result->failure_reason = NATIVE_OTA_FAILURE_HTTP_STATUS_INVALID;
                err_out = ESP_FAIL;
                goto cleanup;
            }
            /* content_length 为 0 表示 chunked：长度校验被跳过，完整性只能靠 complete
             * 标志和调用方对总长度/摘要的核对。 */
            if (config->expected_size > 0 && content_length > 0 &&
                content_length != (int64_t)config->expected_size) {
                ESP_LOGE(TAG, "Full download response invalid: status=%d length=%" PRId64
                         " expected=%zu", status_code, content_length, config->expected_size);
                result->failure_reason = NATIVE_OTA_FAILURE_HTTP_STATUS_INVALID;
                err_out = ESP_FAIL;
                goto cleanup;
            }
            if (content_length > 0) {
                result->total_size = (size_t)content_length;
            }
        }
        result->status_code = status_code;
        result->ranged = resume;
        break;
    }

    /* 防御性校验：当前流程在第二次尝试必然 break 或 goto cleanup，正常不会落到这里；
     * 保留以防将来把“从零回退”次数改成可循环时漏处理无成功会话的情形。 */
    if (client == NULL) {
        result->failure_reason = NATIVE_OTA_FAILURE_NETWORK_TIMEOUT;
        err_out = ESP_FAIL;
        goto cleanup;
    }

    /* 网络暂时没有数据时允许短暂重试，但连续空读超过约 6 s 即判定连接失效。 */
    unsigned empty_reads = 0;
    while (1) {
        int data_read = esp_http_client_read(client, s_buffer, HTTP_DOWNLOADER_BUFFER_SIZE);
        if (data_read == -ESP_ERR_HTTP_EAGAIN) {
            if (++empty_reads > HTTP_DOWNLOADER_MAX_EMPTY_READS) {
                result->failure_reason = NATIVE_OTA_FAILURE_NETWORK_TIMEOUT;
                err_out = ESP_FAIL;
                goto cleanup;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (data_read < 0) {
            ESP_LOGE(TAG, "HTTPS data read failed: %d", data_read);
            result->failure_reason = NATIVE_OTA_FAILURE_NETWORK_TIMEOUT;
            err_out = ESP_FAIL;
            goto cleanup;
        }
        if (data_read == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (++empty_reads > HTTP_DOWNLOADER_MAX_EMPTY_READS) {
                result->failure_reason = NATIVE_OTA_FAILURE_NETWORK_TIMEOUT;
                err_out = ESP_FAIL;
                goto cleanup;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        empty_reads = 0;

        /* 数据逐块同步交给调用方；sink 返回错误时中止，错误码原样透传。 */
        esp_err_t sink_err = config->sink(config->sink_ctx, (const uint8_t *)s_buffer,
                                          (size_t)data_read);
        if (sink_err != ESP_OK) {
            ESP_LOGW(TAG, "Download sink aborted the transfer: %s", esp_err_to_name(sink_err));
            err_out = sink_err;
            goto cleanup;
        }
        result->bytes_received += (size_t)data_read;
    }

    result->complete = esp_http_client_is_complete_data_received(client);

cleanup:
    if (client != NULL) {
        if (client_open) {
            (void)esp_http_client_close(client);
        }
        (void)esp_http_client_cleanup(client);
    }
    return err_out;
}
