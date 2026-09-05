/**
 * @file network_lifecycle.c
 * @brief 让设备启动不依赖热点是否在线，并在网络恢复后自动启动联网服务。
 *
 * 本模块只负责三件事：
 * 1. 连接 Wi-Fi；失败后逐步延长等待时间再试，避免持续占用无线和 CPU；
 * 2. 获得 IPv4 地址后，按顺序启动 MQTT、语音连接和时间同步等服务；
 * 3. 某项服务启动失败时单独重试，不让它阻塞其它联网服务。
 *
 * Wi-Fi driver 事件只更新受锁保护的事实并唤醒 network_lifecycle Task；该 Task 是
 * esp_wifi_connect/disconnect 和服务启动回调的唯一执行者。单次连接看门狗防止
 * GOT_IP/STA_DISCONNECTED 丢失后永久停止重试。
 */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

#include "network_lifecycle.h"
#include "protocol_examples_common.h"

#ifndef CONFIG_NETWORK_WIFI_INITIAL_DIAGNOSTIC_SCAN
#define CONFIG_NETWORK_WIFI_INITIAL_DIAGNOSTIC_SCAN 0
#endif

static const char *TAG = "network_lifecycle";

/**
 * @brief 一项需要在联网后启动的服务及其下一次重试时间。
 *
 * 每项服务独立记录是否已经启动、何时再试以及连续失败次数，因此 MQTT 启动失败
 * 不会改变语音服务的等待时间，反之亦然。
 */
typedef struct {
    network_ip_ready_cb_t callback;
    void *arg;
    bool started_ok;
    int64_t retry_us;
    uint32_t attempt;
} network_service_slot_t;

static TaskHandle_t s_network_task;
static esp_netif_t *s_wifi_netif;
static esp_event_handler_instance_t s_wifi_start_handler;
static esp_event_handler_instance_t s_wifi_disconnect_handler;
static esp_event_handler_instance_t s_got_ip_handler;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
static bool s_wifi_driver_started;
/* 下列连接进度只在持有 s_state_lock 时修改；截止时间使用 esp_timer 单调微秒。 */
static bool s_ip_ready;
static bool s_connect_attempt_pending;
static uint32_t s_retry_attempt;
static int64_t s_next_retry_us = INT64_MAX;
#if CONFIG_NETWORK_WIFI_INITIAL_DIAGNOSTIC_SCAN
static bool s_initial_scan_logged;
#endif

/** IP 就绪服务启动回调注册表；注册只发生在 network_lifecycle_start() 之前。 */
static network_service_slot_t s_slots[NETWORK_MAX_IP_READY_CALLBACKS];
static size_t s_slot_count;

/* 系统通知“热点断开”或“已取得地址”时，只快速记录事实并唤醒后台任务；
 * 真正的连接操作和联网服务启动都在后台执行，避免拖慢 ESP-IDF 的公共事件处理。
 * 多个任务都可能读取连接结果，因此共享记录只在短临界区内更新。
 * 整个程序只保留一套网络生命周期；重复启动不会创建第二套任务和事件监听。 */

/**
 * @brief 网络模块启动到一半失败时，撤销已经创建的资源，使后续可以重新启动。
 *
 * 按创建顺序的反方向撤销事件监听、后台任务和 Wi-Fi 驱动，避免遗留半启动状态。
 * 正常运行不会调用这里。
 */
static void network_lifecycle_cleanup(void)
{
    if (s_got_ip_handler != NULL) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    s_got_ip_handler);
        s_got_ip_handler = NULL;
    }
    if (s_wifi_disconnect_handler != NULL) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                    s_wifi_disconnect_handler);
        s_wifi_disconnect_handler = NULL;
    }
    if (s_wifi_start_handler != NULL) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_START,
                                                    s_wifi_start_handler);
        s_wifi_start_handler = NULL;
    }
    if (s_network_task != NULL) {
        vTaskDelete(s_network_task);
        s_network_task = NULL;
    }
    if (s_wifi_driver_started) {
        (void)esp_wifi_stop();
        s_wifi_driver_started = false;
    }
    (void)esp_wifi_deinit();
    if (s_wifi_netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi_netif);
        s_wifi_netif = NULL;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_started = false;
    s_ip_ready = false;
    s_connect_attempt_pending = false;
    s_retry_attempt = 0;
    s_next_retry_us = INT64_MAX;
    for (size_t i = 0; i < s_slot_count; i++) {
        s_slots[i].started_ok = false;
        s_slots[i].retry_us = INT64_MAX;
        s_slots[i].attempt = 0;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

/**
 * 负向抖动保证实际等待永不超过配置上限；attempt 由调用方分别持有，使 Wi-Fi 和
 * 各服务槽位的失败次数互不影响。达到上限后的计算保持 O(1)，长期离线不会让一次
 * 退避计算随历史失败次数增长。
 */
static uint32_t network_backoff_delay_ms(uint32_t *attempt)
{
    uint32_t base_ms = CONFIG_NETWORK_WIFI_RETRY_BASE_MS;
    uint32_t max_ms = CONFIG_NETWORK_WIFI_RETRY_MAX_MS;
    if (max_ms < base_ms) {
        max_ms = base_ms;
    }

    uint32_t delay_ms = base_ms;
    /* Once the cap is reached, later retries must remain O(1) rather than
     * looping once per historical outage attempt.  31 shifts covers every
     * supported base/max ratio with ample margin. */
    uint32_t shifts = *attempt > 31U ? 31U : *attempt;
    while (shifts-- > 0U && delay_ms < max_ms) {
        if (delay_ms > max_ms / 2U) {
            delay_ms = max_ms;
        } else {
            delay_ms *= 2U;
        }
    }
    if (*attempt != UINT32_MAX) {
        (*attempt)++;
    }

#if CONFIG_NETWORK_WIFI_RETRY_JITTER_PERCENT > 0
    uint32_t jitter_range = (delay_ms * CONFIG_NETWORK_WIFI_RETRY_JITTER_PERCENT) / 100U;
    if (jitter_range > 0U) {
        delay_ms -= esp_random() % (jitter_range + 1U);
    }
#endif
    return delay_ms;
}

static uint32_t network_next_retry_delay_ms(void)
{
    return network_backoff_delay_ms(&s_retry_attempt);
}

/* 所有 Wi-Fi 退避都经此入口更新计数和单调时钟截止点；调用方必须持 s_state_lock，
 * 防止事件回调与生命周期 Task 为同一次失败安排两个不同截止时间。 */
static uint32_t network_schedule_retry_locked(void)
{
    uint32_t delay_ms = network_next_retry_delay_ms();
    s_next_retry_us = esp_timer_get_time() + (int64_t)delay_ms * 1000LL;
    return delay_ms;
}

/** 启动时扫描一次热点，便于区分“找不到热点”和“认证失败”。 */
#if CONFIG_NETWORK_WIFI_INITIAL_DIAGNOSTIC_SCAN
static void network_log_initial_scan(void)
{
    if (s_initial_scan_logged) return;
    s_initial_scan_logged = true;

    wifi_scan_config_t scan = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "diagnostic scan failed: %s", esp_err_to_name(err));
        return;
    }
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) return;
    uint16_t capacity = count > 32U ? 32U : count;
    wifi_ap_record_t *records = capacity ? calloc(capacity, sizeof(*records)) : NULL;
    if (capacity && records == NULL) {
        ESP_LOGW(TAG, "diagnostic scan found %u APs but allocation failed", count);
        return;
    }
    uint16_t returned = capacity;
    err = capacity ? esp_wifi_scan_get_ap_records(&returned, records) : ESP_OK;
    bool target_found = false;
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < returned; ++i) {
            const char *ssid = (const char *)records[i].ssid;
            if (strcmp(ssid, CONFIG_EXAMPLE_WIFI_SSID) == 0) {
                target_found = true;
                ESP_LOGI(TAG, "scan target found ssid=\"%s\" channel=%u rssi=%d auth=%d",
                         ssid, records[i].primary, records[i].rssi, records[i].authmode);
            } else if (i < 10U) {
                ESP_LOGI(TAG, "scan AP[%u] ssid=\"%s\" channel=%u rssi=%d auth=%d",
                         i, ssid, records[i].primary, records[i].rssi, records[i].authmode);
            }
        }
    }
    ESP_LOGI(TAG, "diagnostic scan complete total=%u returned=%u target=%s",
             count, returned, target_found ? "found" : "missing");
    free(records);
}
#endif

/**
 * @brief 把全部服务槽位重置为"待启动"状态。
 *
 * 每次新的 GOT_IP 都重新触发全部回调并清零各自退避计数，与"每个 IP 会话启动
 * 一次服务"的语义一致。必须持 s_state_lock 调用。
 */
static void network_reset_service_slots_locked(void)
{
    for (size_t i = 0; i < s_slot_count; i++) {
        s_slots[i].started_ok = false;
        s_slots[i].retry_us = INT64_MAX;
        s_slots[i].attempt = 0;
    }
}

/**
 * @brief 处理“无线客户端已启动”和“热点连接已断开”两个系统通知。
 *
 * 系统事件处理必须快速返回，因此这里只记录连接结果并唤醒后台任务，
 * 不直接等待下一次连接。
 *
 * - 无线客户端已启动：立即安排第一次连接。
 * - 热点连接已断开：当前地址和联网服务启动结果都失效，稍后按退避策略重连。
 */
static void network_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_STA_START) {
        portENTER_CRITICAL(&s_state_lock);
        /* 把截止时间设到“现在”：任务唤醒后立即发起首次连接尝试。 */
        s_connect_attempt_pending = false;
        s_next_retry_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_state_lock);
        xTaskNotifyGive(s_network_task);
        return;
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        int reason = disconnected != NULL ? disconnected->reason : -1;
        uint32_t delay_ms;
        uint32_t retry_attempt;
        portENTER_CRITICAL(&s_state_lock);
        s_ip_ready = false;
        s_connect_attempt_pending = false;
        /* Every disconnect closes the previous association attempt. Schedule
         * the next one through the capped backoff, whether the failed attempt
         * was initiated at startup or after an online connection. */
        delay_ms = network_schedule_retry_locked();
        retry_attempt = s_retry_attempt;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d); retry #%" PRIu32 " in %" PRIu32 " ms",
                 reason, retry_attempt, delay_ms);
        xTaskNotifyGive(s_network_task);
    }
}

/**
 * @brief 获得 IPv4 地址后，通知后台任务开始启动所有联网服务。
 *
 * 获得地址表示本轮 Wi-Fi 连接成功：重置连接失败次数，并让每项联网服务重新确认
 * 启动。其它网络接口取得地址时不处理，避免把以太网等连接误认成当前 Wi-Fi。
 */
static void network_got_ip_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    const ip_event_got_ip_t *got_ip = event_data;
    if (got_ip == NULL || got_ip->esp_netif != s_wifi_netif) {
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ip_ready = true;
    s_connect_attempt_pending = false;
    s_retry_attempt = 0;
    s_next_retry_us = INT64_MAX;
    network_reset_service_slots_locked();
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "Wi-Fi has IPv4 address " IPSTR "; dispatching registered services",
             IP2STR(&got_ip->ip_info.ip));
    xTaskNotifyGive(s_network_task);
}

/**
 * @brief 计算最近一次需要唤醒的截止时间（微秒）。
 *
 * 服务重试与 Wi-Fi 重连共用同一次阻塞等待；任一时间点到期都会唤醒。
 *
 * @param[in] next_retry_us     下一次 Wi-Fi 重连截止时间。
 * @param[in] service_retry_us  最近的服务回调重试截止时间；无则为 INT64_MAX。
 * @return FreeRTOS 等待 tick；无任何截止时间时为 portMAX_DELAY。
 */
static TickType_t network_wait_ticks_until(int64_t next_retry_us, int64_t service_retry_us)
{
    int64_t deadline_us = next_retry_us < service_retry_us ? next_retry_us : service_retry_us;
    if (deadline_us == INT64_MAX) {
        return portMAX_DELAY;
    }
    int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    uint64_t remaining_ms = ((uint64_t)remaining_us + 999ULL) / 1000ULL;
    TickType_t ticks = pdMS_TO_TICKS(remaining_ms);
    return ticks == 0 ? 1 : ticks;
}

/**
 * @brief 在 IP 就绪窗口内推进所有服务启动槽位。
 *
 * 对每个"到期且未成功"的槽位调用其回调：成功标记完成，失败按独立退避调度
 * 下一次重试。回调在生命周期任务上下文中执行（不在中断/事件回调中）。
 *
 * @return true 本轮至少调用了一个回调；false 没有可调用的回调。
 */
void network_lifecycle_retry_services(void)
{
    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < s_slot_count; ++i) {
        if (!s_slots[i].started_ok) {
            s_slots[i].retry_us = INT64_MAX;
            s_slots[i].attempt = 0;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

static bool network_dispatch_service_slots(void)
{
    bool invoked_any = false;
    for (size_t i = 0; i < s_slot_count; i++) {
        bool invoke = false;
        network_ip_ready_cb_t callback;
        void *arg;
        portENTER_CRITICAL(&s_state_lock);
        if (!s_slots[i].started_ok &&
            (s_slots[i].retry_us == INT64_MAX ||
             esp_timer_get_time() >= s_slots[i].retry_us)) {
            /* 先清截止时间：失败路径会重新调度，成功路径不再需要。 */
            s_slots[i].retry_us = INT64_MAX;
            invoke = true;
            callback = s_slots[i].callback;
            arg = s_slots[i].arg;
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (!invoke) {
            continue;
        }
        invoked_any = true;

        esp_err_t err = callback(arg);
        portENTER_CRITICAL(&s_state_lock);
        if (err == ESP_OK) {
            s_slots[i].started_ok = true;
            s_slots[i].attempt = 0;
            portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGI(TAG, "Registered service %u started", (unsigned)i);
        } else {
            uint32_t delay_ms = network_backoff_delay_ms(&s_slots[i].attempt);
            s_slots[i].retry_us = esp_timer_get_time() + (int64_t)delay_ms * 1000LL;
            portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGW(TAG, "Registered service %u start failed: %s; retry in %" PRIu32 " ms",
                     (unsigned)i, esp_err_to_name(err), delay_ms);
        }
    }
    return invoked_any;
}

/**
 * @brief 计算服务槽位最近的重试截止时间（微秒）。
 *
 * @param[in] ip_ready 当前是否持有 IP；无 IP 时返回 INT64_MAX，槽位不参与等待。
 * @return 最近的槽位重试截止时间；无待重试槽位时为 INT64_MAX。
 */
static int64_t network_service_retry_deadline(bool ip_ready)
{
    int64_t nearest_us = INT64_MAX;
    if (!ip_ready) {
        return nearest_us;
    }
    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < s_slot_count; i++) {
        if (!s_slots[i].started_ok && s_slots[i].retry_us != INT64_MAX &&
            s_slots[i].retry_us < nearest_us) {
            nearest_us = s_slots[i].retry_us;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    return nearest_us;
}

/**
 * @brief 后台网络生命周期任务：推进 Wi-Fi 重连与服务启动的单一循环。
 *
 * 每轮先确认设备是否已经获得网络地址：有地址时启动到期的联网服务；没有地址且
 * 已到重试时间时连接热点。esp_wifi_connect() 返回 ESP_OK 只表示请求已受理，仍须
 * 等待 GOT_IP；看门狗到期会结束没有结果的尝试，再回到同一退避序列。
 *
 * 这个单循环把 Wi-Fi 重连与服务启动的两份计时合并成一次阻塞等待，避免两个任务
 * 竞争调度；服务回调在该任务中执行，因此不会中断任何事件循环。
 */
static void network_lifecycle_task(void *parameter)
{
    (void)parameter;

    while (true) {
        bool ip_ready;
        bool connect_attempt_pending;
        int64_t next_retry_us;
        int64_t service_retry_us;
        portENTER_CRITICAL(&s_state_lock);
        ip_ready = s_ip_ready;
        connect_attempt_pending = s_connect_attempt_pending;
        next_retry_us = s_next_retry_us;
        portEXIT_CRITICAL(&s_state_lock);

        if (ip_ready) {
            if (network_dispatch_service_slots()) {
                /* 回调可能重新调度了自己或后续槽位；立即回到循环重算截止时间。 */
                continue;
            }
        }
        service_retry_us = network_service_retry_deadline(ip_ready);

        if (!ip_ready && next_retry_us != INT64_MAX &&
            esp_timer_get_time() >= next_retry_us) {
            if (connect_attempt_pending) {
                ESP_LOGW(TAG, "Wi-Fi connect attempt timed out after %d ms; restarting",
                         CONFIG_NETWORK_WIFI_CONNECT_TIMEOUT_MS);
                portENTER_CRITICAL(&s_state_lock);
                bool timed_out = !s_ip_ready && s_connect_attempt_pending &&
                                 esp_timer_get_time() >= s_next_retry_us;
                if (timed_out) {
                    s_connect_attempt_pending = false;
                    (void)network_schedule_retry_locked();
                }
                portEXIT_CRITICAL(&s_state_lock);
                /* 先安排重试再请求断开；即使驱动不补发事件，Task 也不会失去截止时间。 */
                if (timed_out) (void)esp_wifi_disconnect();
                continue;
            }
#if CONFIG_NETWORK_WIFI_INITIAL_DIAGNOSTIC_SCAN
            network_log_initial_scan();
#endif
            /* 调用前预约看门狗；成功返回不能覆写已经到达的 GOT_IP/断开事件。 */
            portENTER_CRITICAL(&s_state_lock);
            bool can_connect = !s_ip_ready && !s_connect_attempt_pending;
            if (can_connect) {
                s_connect_attempt_pending = true;
                s_next_retry_us = esp_timer_get_time() +
                    (int64_t)CONFIG_NETWORK_WIFI_CONNECT_TIMEOUT_MS * 1000LL;
            }
            portEXIT_CRITICAL(&s_state_lock);
            if (!can_connect) continue;
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
                portENTER_CRITICAL(&s_state_lock);
                if (s_connect_attempt_pending && !s_ip_ready) {
                    s_connect_attempt_pending = false;
                    (void)network_schedule_retry_locked();
                }
                portEXIT_CRITICAL(&s_state_lock);
            }
            continue;
        }

        (void)ulTaskNotifyTake(pdTRUE,
                               network_wait_ticks_until(next_retry_us, service_retry_us));
    }
}

esp_err_t network_lifecycle_register_ip_ready(network_ip_ready_cb_t callback, void *arg)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_state_lock);
    if (s_started) {
        result = ESP_ERR_INVALID_STATE;
    } else if (s_slot_count < NETWORK_MAX_IP_READY_CALLBACKS) {
        s_slots[s_slot_count].callback = callback;
        s_slots[s_slot_count].arg = arg;
        s_slots[s_slot_count].started_ok = false;
        s_slots[s_slot_count].retry_us = INT64_MAX;
        s_slots[s_slot_count].attempt = 0;
        s_slot_count++;
        result = ESP_OK;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return result;
}

esp_err_t network_lifecycle_start(void)
{
#if !CONFIG_EXAMPLE_CONNECT_WIFI
    ESP_LOGW(TAG, "Wi-Fi lifecycle is disabled by configuration");
    return ESP_ERR_NOT_SUPPORTED;
#else
    portENTER_CRITICAL(&s_state_lock);
    bool already_started = s_started;
    if (!already_started) {
        s_started = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (already_started) {
        return ESP_OK;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wifi_init_cfg);
    if (err != ESP_OK) {
        goto failed;
    }
    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                              network_wifi_event_handler, NULL,
                                              &s_wifi_start_handler);
    if (err != ESP_OK) {
        goto failed;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              network_wifi_event_handler, NULL,
                                              &s_wifi_disconnect_handler);
    if (err != ESP_OK) {
        goto failed;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              network_got_ip_handler, NULL,
                                              &s_got_ip_handler);
    if (err != ESP_OK) {
        goto failed;
    }

    wifi_config_t wifi_cfg = {
        .sta = {
#if !CONFIG_EXAMPLE_WIFI_SSID_PWD_FROM_STDIN
            .ssid = CONFIG_EXAMPLE_WIFI_SSID,
            .password = CONFIG_EXAMPLE_WIFI_PASSWORD,
#endif
            .scan_method = EXAMPLE_WIFI_SCAN_METHOD,
            .sort_method = EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD,
            .threshold.rssi = CONFIG_EXAMPLE_WIFI_SCAN_RSSI_THRESHOLD,
            /* Windows 移动热点可能广播 WPA2/WPA3 过渡模式；扫描阶段允许发现 OPEN
             * 阈值以上的 AP，实际加密方式仍由非空密码和 AP 的 RSN IE 协商。 */
            .threshold.authmode = WIFI_AUTH_OPEN,
            /* Windows Mobile Hotspot advertises WPA2/WPA3 transition mode.
             * Zero-initializing these fields leaves SAE in hunt-and-peck-only
             * mode and does not advertise PMF capability; use the ESP-IDF
             * station example defaults so either WPA2 or WPA3 can negotiate. */
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (err != ESP_OK) {
        goto failed;
    }

    if (xTaskCreate(network_lifecycle_task, "network_lifecycle", 4096, NULL, 4,
                    &s_network_task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        goto failed;
    }
    s_wifi_driver_started = true;
    esp_err_t tx_power_err = esp_wifi_set_max_tx_power(
        (int8_t)(CONFIG_NETWORK_WIFI_MAX_TX_POWER_DBM * 4));
    if (tx_power_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi TX power limit failed: %s", esp_err_to_name(tx_power_err));
    }
    ESP_LOGI(TAG, "Wi-Fi lifecycle started target=\"%s\" tx_limit=%ddBm extra_scan=%u; local application continues while offline",
             CONFIG_EXAMPLE_WIFI_SSID, CONFIG_NETWORK_WIFI_MAX_TX_POWER_DBM,
             (unsigned)CONFIG_NETWORK_WIFI_INITIAL_DIAGNOSTIC_SCAN);
    return ESP_OK;

failed:
    /* Startup failures are logged to the caller and do not abort app_main.
     * Tear down the partial singleton so a deliberate later call can retry
     * initialization without leaking a task, netif, or event registration. */
    ESP_LOGE(TAG, "Unable to start Wi-Fi lifecycle: %s", esp_err_to_name(err));
    network_lifecycle_cleanup();
    return err;
#endif
}
