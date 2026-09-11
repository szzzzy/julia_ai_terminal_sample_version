/** Protocol v2: connection-scoped handshake, acknowledged state snapshots and
 * revision-checked requests. No network operations from FSM observers. */
#include "voice_state_sync.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "julia_fsm_runtime.h"
#include "sdkconfig.h"
#include "wss_transport.h"
#include "ota_control_plane.h"
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
#include "mqtt_comm.h"
#endif

#if CONFIG_JULIA_CLOUD_STATE_SYNC_ENABLE
#define SYNC_RETRY_US 2000000LL
#define SYNC_MAX_SENDS 3U
#define REQUEST_CACHE_SIZE 8U
#define REQUEST_ID_BYTES 64U
#define STATE_MESSAGE_BYTES 512U
static const char *TAG = "voice_state_sync";
static portMUX_TYPE s_sync_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready;
static bool s_active;
static char s_session_id[33];
static char s_device_id[NATIVE_OTA_DEVICE_ID_SIZE];
static char s_pending[STATE_MESSAGE_BYTES];
static char s_pending_request[64];
static uint32_t s_pending_revision;
static bool s_waiting_ack;
static bool s_have_revision;
static uint32_t s_last_revision;
static unsigned s_sends;
static int64_t s_retry_us;
typedef struct {
    char id[REQUEST_ID_BYTES];
    char response[STATE_MESSAGE_BYTES];
} request_result_t;
static request_result_t s_results[REQUEST_CACHE_SIZE];
static unsigned s_result_next;

const char *voice_state_sync_session_id(void) { return s_session_id; }

static void set_ready(bool ready)
{
    portENTER_CRITICAL(&s_sync_lock);
    s_ready = ready;
    portEXIT_CRITICAL(&s_sync_lock);
}

bool voice_state_sync_is_ready(void)
{
    portENTER_CRITICAL(&s_sync_lock);
    bool ready = s_ready;
    portEXIT_CRITICAL(&s_sync_lock);
    return ready;
}

static bool valid_id(const char *id)
{
    if (id == NULL || id[0] == '\0' || strlen(id) >= REQUEST_ID_BYTES) return false;
    for (const char *p = id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
              *p == '.' || *p == ':')) return false;
    }
    return true;
}

static const char *substate(const julia_fsm_snapshot_t *s)
{
    switch (s->s2_sub_state) {
    case JULIA_S2_SUB_STATE_S2_1_LISTENING: return "S2.1";
    case JULIA_S2_SUB_STATE_S2_2_THINKING: return "S2.2";
    case JULIA_S2_SUB_STATE_S2_3_SPEAKING: return "S2.3";
    default: break;
    }
    if (s->s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED) return "S7.1";
    if (s->s7_sub_state == JULIA_S7_SUB_STATE_S7_2_FAULT) return "S7.2";
    return "";
}

static bool wake_required(const julia_fsm_snapshot_t *s)
{
    return s->main_state != JULIA_MAIN_STATE_S1_COMPANION &&
           s->main_state != JULIA_MAIN_STATE_S2_DIALOG &&
           s->main_state != JULIA_MAIN_STATE_S4_INTERACTION;
}

static bool send_text(const char *text)
{
    if (wss_transport_send_now(0x1, (const uint8_t *)text, strlen(text)) == ESP_OK)
        return true;
    set_ready(false);
    s_active = false;
    wss_transport_fail_session();
    return false;
}

static void prepare_state(const char *type, const julia_fsm_snapshot_t *snapshot)
{
    char envelope[192] = "";
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    snprintf(s_pending_request, sizeof(s_pending_request), "%s-%" PRIu32, type, snapshot->revision);
    snprintf(envelope, sizeof(envelope),
             "\"control_protocol\":1,\"device_time_ms\":%" PRIi64 ",\"interaction_id\":\"\",\"request_id\":\"%s\",",
             esp_timer_get_time()/1000LL, s_pending_request);
#endif
    int n = snprintf(s_pending, sizeof(s_pending),
        "{\"type\":\"%s\",%s\"device_id\":\"%s\",\"protocol_version\":2,\"session_id\":\"%s\","
        "\"state_revision\":%" PRIu32 ",\"state\":\"S%d\",\"sub_state\":\"%s\","
        "\"wake_required\":%s,\"companion_remaining_ms\":%" PRIu32 ",\"reason\":\"%s\"}",
        type, envelope, s_device_id, s_session_id, snapshot->revision, (int)snapshot->main_state,
        substate(snapshot), wake_required(snapshot) ? "true" : "false",
        snapshot->companion_remaining_ms, julia_fsm_event_name(snapshot->reason));
    if (n <= 0 || (size_t)n >= sizeof(s_pending)) {
        s_active = false;
        set_ready(false);
        wss_transport_fail_session();
        return;
    }
    s_pending_revision = snapshot->revision;
    s_last_revision = snapshot->revision;
    s_have_revision = true;
    s_waiting_ack = true;
    s_sends = 0;
    s_retry_us = 0;
}

void voice_state_sync_end(void)
{
    set_ready(false);
    s_active = false;
    s_waiting_ack = false;
    s_have_revision = false;
    s_session_id[0] = '\0';
    memset(s_results, 0, sizeof(s_results));
    s_result_next = 0;
}

void voice_state_sync_start(void)
{
    voice_state_sync_end();
    if (native_ota_get_device_id(s_device_id, sizeof(s_device_id)) != ESP_OK) {
        ESP_LOGE(TAG, "stable device identity unavailable; refusing voice session");
        wss_transport_fail_session();
        return;
    }
    uint8_t random_id[16];
    esp_fill_random(random_id, sizeof(random_id));
    for (size_t i = 0; i < sizeof(random_id); ++i)
        snprintf(s_session_id + i * 2U, 3, "%02x", random_id[i]);
    s_active = true;
    julia_fsm_snapshot_t snapshot;
    julia_fsm_runtime_get_snapshot(&snapshot);
    prepare_state("session_sync", &snapshot);
    voice_state_sync_poll();
}

void voice_state_sync_poll(void)
{
    if (!s_active) return;
    if (voice_state_sync_is_ready()) {
        julia_fsm_snapshot_t snapshot;
        julia_fsm_runtime_get_snapshot(&snapshot);
        if (!s_have_revision || snapshot.revision != s_last_revision)
            prepare_state("device_state", &snapshot);
    }
    int64_t now = esp_timer_get_time();
    if (!s_active || !s_waiting_ack || now < s_retry_us) return;
    if (s_sends >= SYNC_MAX_SENDS) {
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
        wss_transport_defer_retry(30);
#endif
        ESP_LOGW(TAG, "cloud state ACK timeout; ending unsynchronized session");
        set_ready(false);
        s_active = false;
        wss_transport_fail_session();
        return;
    }
    if (send_text(s_pending)) {
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
        if (s_sends == 0 && voice_state_sync_is_ready())
            (void)mqtt_comm_publish_voice_status(s_device_id, s_pending, strlen(s_pending));
#endif
        ++s_sends;
        s_retry_us = now + SYNC_RETRY_US;
    }
}

static bool json_revision(const cJSON *root, uint32_t *revision)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "state_revision");
    if (!cJSON_IsNumber(value) || !(value->valuedouble >= 0 &&
        value->valuedouble <= UINT32_MAX)) return false;
    *revision = (uint32_t)value->valuedouble;
    return value->valuedouble == (double)*revision;
}

static void handle_require_wake(const cJSON *root)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    uint32_t revision;
    if (!cJSON_IsString(id) || !valid_id(id->valuestring) ||
        !json_revision(root, &revision)) {
        ESP_LOGW(TAG, "require_wake rejected: invalid request_id/state_revision");
        return;
    }
    for (size_t i = 0; i < REQUEST_CACHE_SIZE; ++i) {
        if (strcmp(s_results[i].id, id->valuestring) == 0) {
            (void)send_text(s_results[i].response);
            return;
        }
    }
    julia_fsm_snapshot_t snapshot;
    julia_fsm_runtime_get_snapshot(&snapshot);
    bool accepted = false;
    const char *reason;
    if (!voice_state_sync_is_ready()) reason = "sync_required";
    else if (revision != snapshot.revision) reason = "stale_state";
    else {
        accepted = julia_fsm_runtime_require_wake(revision) == ESP_OK;
        julia_fsm_runtime_get_snapshot(&snapshot);
        reason = accepted ? "applied" :
            snapshot.revision != revision ? "stale_state" :
            (snapshot.main_state == JULIA_MAIN_STATE_S2_DIALOG ||
             snapshot.main_state == JULIA_MAIN_STATE_S4_INTERACTION)
                ? "interaction_active" : "state_unavailable";
    }
    request_result_t *result = &s_results[s_result_next];
    s_result_next = (s_result_next + 1U) % REQUEST_CACHE_SIZE;
    strcpy(result->id, id->valuestring);
    int n = snprintf(result->response, sizeof(result->response),
        "{\"type\":\"require_wake_ack\",\"device_id\":\"%s\",\"interaction_id\":\"\",\"session_id\":\"%s\",\"request_id\":\"%s\","
        "\"accepted\":%s,\"state\":\"S%d\",\"sub_state\":\"%s\","
        "\"state_revision\":%" PRIu32 ",\"wake_required\":%s,\"reason\":\"%s\"}",
        s_device_id, s_session_id, id->valuestring, accepted ? "true" : "false",
        (int)snapshot.main_state, substate(&snapshot), snapshot.revision,
        wake_required(&snapshot) ? "true" : "false", reason);
    if (n > 0 && (size_t)n < sizeof(result->response)) (void)send_text(result->response);
}

bool voice_state_sync_handle_text(const uint8_t *text, size_t len)
{
    if (len == 0 || text[0] != '{') return false;
    cJSON *root = cJSON_ParseWithLength((const char *)text, len);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) { cJSON_Delete(root); return false; }
    bool sync_ack = strcmp(type->valuestring, "session_sync_ack") == 0;
    bool state_ack = strcmp(type->valuestring, "device_state_ack") == 0;
    bool require = strcmp(type->valuestring, "require_wake") == 0;
    if (!sync_ack && !state_ack && !require) { cJSON_Delete(root); return false; }
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    if (!cJSON_IsString(device) || strcmp(device->valuestring, s_device_id)) {
        ESP_LOGW(TAG, "ignoring control message for a different device");
        cJSON_Delete(root);
        return true;
    }
    if (sync_ack || state_ack) {
        const cJSON *request = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        if (!cJSON_IsString(request) || strcmp(request->valuestring, s_pending_request)) {
            cJSON_Delete(root);
            return true;
        }
    }
    if (sync_ack) {
        const cJSON *protocol=cJSON_GetObjectItemCaseSensitive(root,"control_protocol");
        if (!cJSON_IsNumber(protocol) || protocol->valuedouble!=1) {
            ESP_LOGE(TAG,"cloud control protocol mismatch");
            wss_transport_defer_retry(60);
            s_active=false;set_ready(false);wss_transport_fail_session();
            cJSON_Delete(root);return true;
        }
    }
#endif
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (!s_active || !cJSON_IsString(session) ||
        strcmp(session->valuestring, s_session_id) != 0) {
        ESP_LOGW(TAG, "ignoring control message for a stale voice session");
        cJSON_Delete(root);
        return true;
    }
    if (require) handle_require_wake(root);
    else if (sync_ack && !voice_state_sync_is_ready()) {
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
        const cJSON *accepted = cJSON_GetObjectItemCaseSensitive(root, "accepted");
        uint32_t revision;
        if (cJSON_IsNumber(version) && version->valuedouble == 2 &&
            json_revision(root, &revision) && revision == s_pending_revision &&
            cJSON_IsTrue(accepted)) {
            s_waiting_ack = false;
            set_ready(true);
            /* New state may have been committed during the initial handshake. */
            s_have_revision = false;
            if (julia_fsm_runtime_post_sync(EVT_WSS_CONNECTED) != ESP_OK) {
                set_ready(false);
                s_active = false;
                wss_transport_fail_session();
            }
        } else {
            ESP_LOGW(TAG, "invalid/rejected session_sync_ack; handshake remains pending");
        }
    } else if (state_ack && voice_state_sync_is_ready()) {
        uint32_t revision;
        if (json_revision(root, &revision) && s_waiting_ack &&
            revision == s_pending_revision) s_waiting_ack = false;
    }
    cJSON_Delete(root);
    return true;
}
#else
const char *voice_state_sync_session_id(void) { return ""; }
void voice_state_sync_start(void) {}
void voice_state_sync_end(void) {}
void voice_state_sync_poll(void) {}
bool voice_state_sync_is_ready(void) { return true; }
bool voice_state_sync_handle_text(const uint8_t *text, size_t len)
{ (void)text; (void)len; return false; }
#endif
