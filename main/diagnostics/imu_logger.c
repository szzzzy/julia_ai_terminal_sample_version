/* Temporary characterization harness. One RAM record, no product FSM/voice tasks.
 * HTTP handlers only enqueue work; this worker alone owns IMU and speaker I/O. */
#include "imu_logger.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "board_audio.h"
#include "julia_avatar.h"
#include "julia_backlight.h"
#include "julia_display.h"
#include "julia_power.h"
#include "network_lifecycle.h"
#include "qmi8658_shared.h"

#define RECORD_US 8000000LL
#define MAX_SAMPLES 1200
#define CSV_BYTES (256 * 1024)
typedef struct { uint32_t t_us, counter; int16_t raw[6]; } sample_t;
typedef struct { bool retry; char label[49], ip[16]; unsigned port; } command_t;
static const char *TAG = "imu_logger";
static sample_t *s_samples;
static char *s_csv;
static size_t s_csv_len;
static QueueHandle_t s_commands;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
static esp_timer_handle_t s_tick;
static const char *s_stage = "idle"; /* protected by s_lock */
static char s_id[24];
static unsigned s_serial;
static uint32_t s_boot_id;
static command_t s_record;

static void stage(const char *value, const char *display)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stage = value;
    xSemaphoreGive(s_lock);
    julia_avatar_set_status_text(display);
    ESP_LOGI(TAG, "%s", value);
}

/* Blocking tone only outside capture; append silence to drain DMA before stop. */
static esp_err_t beep(unsigned count)
{
    int16_t wave[160];
    esp_err_t err = board_audio_speaker_start(16000);
    if (err != ESP_OK) return err;
    for (unsigned n = 0; n < count && err == ESP_OK; ++n) {
        for (unsigned block = 0; block < 25 && err == ESP_OK; ++block) {
            for (unsigned i = 0; i < 160; ++i)
                wave[i] = block < 12 ? (i % 16 < 8 ? 5000 : -5000) : 0;
            err = board_audio_speaker_write((const uint8_t *)wave, sizeof(wave));
        }
    }
    esp_err_t stopped = board_audio_speaker_stop();
    return err == ESP_OK ? stopped : err;
}

static void sample_tick(void *arg)
{
    (void)arg;
    xTaskNotifyGive(s_worker);
}

/* CSV is prepared only after sampling has stopped. Raw counts remain inspectable. */
static bool make_csv(unsigned count, unsigned errors, unsigned raced, unsigned missed,
                     unsigned clipped, bool full, bool cue_ok)
{
    int n = snprintf(s_csv, CSV_BYTES,
        "# {\"id\":\"%s\",\"label\":\"%s\",\"samples\":%u,\"duration_us\":8000000,"
        "\"accel_range_g\":%u,\"gyro_range_dps\":%u,\"odr_code\":6,\"lpf\":false,"
        "\"read_errors\":%u,\"raced_reads\":%u,\"missed_samples\":%u,"
        "\"clipped_samples\":%u,\"buffer_full\":%s,\"end_cue_ok\":%s}\n"
        "seq,t_us,sensor_counter,ax_raw,ay_raw,az_raw,gx_raw,gy_raw,gz_raw,"
        "ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n",
        s_id, s_record.label, count,
        (unsigned)BOARD_IMU_LOGGER_ACCEL_RANGE_G, (unsigned)BOARD_IMU_LOGGER_GYRO_RANGE_DPS,
        errors, raced, missed, clipped,
        full ? "true" : "false", cue_ok ? "true" : "false");
    if (n < 0 || n >= CSV_BYTES) return false;
    size_t used = n;
    for (unsigned i = 0; i < count; ++i) {
        sample_t *r = &s_samples[i];
        n = snprintf(s_csv + used, CSV_BYTES - used,
            "%u,%" PRIu32 ",%" PRIu32 ",%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
            i, r->t_us, r->counter, r->raw[0], r->raw[1], r->raw[2],
            r->raw[3], r->raw[4], r->raw[5],
            r->raw[0] * (BOARD_IMU_LOGGER_ACCEL_RANGE_G / 32768.0),
            r->raw[1] * (BOARD_IMU_LOGGER_ACCEL_RANGE_G / 32768.0),
            r->raw[2] * (BOARD_IMU_LOGGER_ACCEL_RANGE_G / 32768.0),
            r->raw[3] * (BOARD_IMU_LOGGER_GYRO_RANGE_DPS / 32768.0),
            r->raw[4] * (BOARD_IMU_LOGGER_GYRO_RANGE_DPS / 32768.0),
            r->raw[5] * (BOARD_IMU_LOGGER_GYRO_RANGE_DPS / 32768.0));
        if (n < 0 || (size_t)n >= CSV_BYTES - used) return false;
        used += n;
    }
    s_csv_len = used;
    return true;
}

typedef struct { char text[32]; size_t used; } ack_t;
static esp_err_t receive_ack(esp_http_client_event_t *event)
{
    ack_t *ack = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t n = (size_t)event->data_len;
        if (n >= sizeof(ack->text) - ack->used) return ESP_FAIL;
        memcpy(ack->text + ack->used, event->data, n);
        ack->used += n;
        ack->text[ack->used] = 0;
    }
    return ESP_OK;
}

static bool upload(void)
{
    char url[100];
    snprintf(url, sizeof(url), "http://%s:%u/records/%s", s_record.ip, s_record.port, s_id);
    ack_t ack = {0};
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 10000,
        .disable_auto_redirect = true, .event_handler = receive_ack, .user_data = &ack,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "Content-Type", "text/csv; charset=utf-8");
    esp_http_client_set_post_field(client, s_csv, (int)s_csv_len);
    esp_err_t err = esp_http_client_perform(client);
    bool ok = err == ESP_OK && esp_http_client_get_status_code(client) == 200 &&
              strcmp(ack.text, s_id) == 0;
    esp_http_client_cleanup(client);
    return ok;
}

static bool capture(void)
{
    unsigned count = 0, errors = 0, raced = 0, missed = 0, clipped = 0;
    uint32_t last = 0;
    bool have_last = false, full = false;
    stage("preparing", "IMU: PREPARE 3s");
    if (board_imu_logger_configure() != ESP_OK || beep(1) != ESP_OK) {
        (void)board_imu_set_enabled(false);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Sensor settling and acoustic vibration decay. */
    stage("capturing", "IMU: RECORDING 8s");
    (void)ulTaskNotifyTake(pdTRUE, 0);
    if (esp_timer_start_periodic(s_tick, 2000) != ESP_OK) {
        (void)board_imu_set_enabled(false);
        return false;
    }
    int64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < RECORD_US) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        int64_t begin = esp_timer_get_time();
        if (begin - start >= RECORD_US) break;
        sample_t row;
        esp_err_t err = board_imu_logger_read(&row.counter, row.raw);
        int64_t end = esp_timer_get_time();
        if (err == ESP_ERR_NOT_FINISHED) { ++raced; continue; }
        if (err != ESP_OK) { ++errors; continue; }
        if (end - start >= RECORD_US) break;
        if (have_last && row.counter == last) continue;
        if (have_last) {
            uint32_t delta = (row.counter - last) & 0xffffff;
            if (delta > 1) missed += delta - 1;
        }
        last = row.counter; have_last = true;
        row.t_us = (uint32_t)((begin + end) / 2 - start);
        bool clip = false;
        for (unsigned j = 0; j < 6; ++j)
            if (row.raw[j] >= 32760 || row.raw[j] <= -32760) clip = true;
        if (clip) ++clipped;
        s_samples[count++] = row;
        if (count == MAX_SAMPLES) { full = true; break; }
    }
    (void)esp_timer_stop(s_tick);
    (void)board_imu_set_enabled(false);
    stage("finished", "IMU: DONE");
    bool cue_ok = beep(2) == ESP_OK; /* Never part of the recorded interval. */
    ESP_LOGI(TAG, "record=%s samples=%u read_errors=%u missed=%u", s_id, count, errors, missed);
    return make_csv(count, errors, raced, missed, clipped, full, cue_ok);
}

static void worker(void *arg)
{
    (void)arg;
    command_t command;
    while (xQueueReceive(s_commands, &command, portMAX_DELAY) == pdTRUE) {
        if (!command.retry) {
            s_record = command;
            if (!capture()) { stage("error", "IMU: CAPTURE ERROR"); continue; }
        }
        stage("uploading", "IMU: UPLOADING");
        if (upload()) stage("saved", "IMU: SAVED / READY");
        else stage("retry", "IMU: UPLOAD FAILED"); /* CSV and samples remain intact. */
    }
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char text[100];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(text, sizeof(text), "{\"stage\":\"%s\",\"id\":\"%s\"}", s_stage, s_id);
    xSemaphoreGive(s_lock);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, text);
}

/* HTTPD uses an IPv6 listener when LWIP_IPV6 is enabled. IPv4 connections then
 * arrive as ::ffff:a.b.c.d; retain only the IPv4 part for our LAN upload URL. */
static bool format_peer_ipv4(const struct sockaddr_storage *peer, char *ip, size_t size)
{
    if (peer->ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)peer;
        return inet_ntop(AF_INET, &v4->sin_addr, ip, size) != NULL;
    }
#if CONFIG_LWIP_IPV6
    if (peer->ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)peer;
        if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr))
            return inet_ntop(AF_INET, &v6->sin6_addr.s6_addr[12], ip, size) != NULL;
    }
#endif
    return false;
}

static esp_err_t command_handler(httpd_req_t *req)
{
    command_t command = {.retry = strcmp(req->uri, "/retry") == 0};
    if (!command.retry) {
        char body[256];
        if (req->content_len < 1 || req->content_len >= sizeof(body))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body size");
        size_t got = 0;
        while (got < req->content_len) {
            int n = httpd_req_recv(req, body + got, req->content_len - got);
            if (n <= 0) return ESP_FAIL;
            got += n;
        }
        body[got] = 0;
        cJSON *json = cJSON_Parse(body);
        cJSON *label = cJSON_GetObjectItemCaseSensitive(json, "label");
        cJSON *port = cJSON_GetObjectItemCaseSensitive(json, "port");
        bool valid = cJSON_IsString(label) && strlen(label->valuestring) > 0 &&
                     strlen(label->valuestring) < sizeof(command.label) &&
                     cJSON_IsNumber(port) && port->valuedouble == port->valueint &&
                     port->valueint >= 1024 && port->valueint <= 65535;
        if (valid) {
            strcpy(command.label, label->valuestring);
            command.port = port->valueint;
            for (char *p = command.label; *p; ++p)
                if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) valid = false;
        }
        cJSON_Delete(json);
        if (!valid)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid label or upload port");
        struct sockaddr_storage peer = {0};
        socklen_t peer_size = sizeof(peer);
        if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &peer_size))
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot read client address");
        if (!format_peer_ipv4(&peer, command.ip, sizeof(command.ip)))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "IPv4 or mapped IPv4 client required");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool allowed = command.retry ? strcmp(s_stage, "retry") == 0 :
        (strcmp(s_stage, "idle") == 0 || strcmp(s_stage, "saved") == 0 || strcmp(s_stage, "error") == 0);
    if (!allowed) {
        xSemaphoreGive(s_lock);
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "record busy or awaiting upload; use /status or /retry");
    }
    const char *old_stage = s_stage;
    s_stage = "queued";
    if (!command.retry) snprintf(s_id, sizeof(s_id), "%08" PRIx32 "-%06u", s_boot_id, ++s_serial);
    bool queued = xQueueSend(s_commands, &command, 0) == pdTRUE;
    if (!queued) s_stage = old_stage;
    xSemaphoreGive(s_lock);
    if (!queued) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "queue full");
    return status_handler(req);
}

static esp_err_t ip_ready(void *arg)
{
    (void)arg;
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK) return ESP_FAIL;
    char text[64];
    snprintf(text, sizeof(text), "IMU: " IPSTR ":8080", IP2STR(&ip.ip));
    ESP_LOGI(TAG, "%s", text);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool idle = strcmp(s_stage, "idle") == 0;
    xSemaphoreGive(s_lock);
    if (idle) julia_avatar_set_status_text(text);
    return ESP_OK;
}

esp_err_t imu_logger_start(void)
{
    ESP_ERROR_CHECK(julia_backlight_init());
    ESP_ERROR_CHECK(julia_display_init());
    ESP_ERROR_CHECK(julia_avatar_init());
    julia_backlight_set(50);
    julia_avatar_set_status_text("IMU: CONNECTING WIFI");
    ESP_ERROR_CHECK(board_audio_init());
    board_audio_mic_set_enabled(false);
    ESP_ERROR_CHECK(board_imu_init());
    s_samples = heap_caps_calloc(MAX_SAMPLES, sizeof(sample_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_csv = heap_caps_malloc(CSV_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = xSemaphoreCreateMutex();
    s_commands = xQueueCreate(1, sizeof(command_t));
    if (!s_samples || !s_csv || !s_lock || !s_commands) return ESP_ERR_NO_MEM;
    s_boot_id = esp_random();
    if (xTaskCreate(worker, "imu_logger", 6144, NULL, 5, &s_worker) != pdPASS) return ESP_ERR_NO_MEM;
    esp_timer_create_args_t timer = {.callback = sample_tick, .name = "imu_sample"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_tick));
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8080;
    cfg.stack_size = 6144;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    const httpd_uri_t routes[] = {
        {.uri = "/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/start", .method = HTTP_POST, .handler = command_handler},
        {.uri = "/retry", .method = HTTP_POST, .handler = command_handler},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    ESP_ERROR_CHECK(julia_power_runtime_profile_enable());
    ESP_ERROR_CHECK(network_lifecycle_register_ip_ready(ip_ready, NULL));
    ESP_ERROR_CHECK(network_lifecycle_start());
    ESP_LOGI(TAG, "EXPERIMENT ONLY: HTTP :8080; 3s prepare + 8s capture; product FSM disabled");
    return ESP_OK;
}
