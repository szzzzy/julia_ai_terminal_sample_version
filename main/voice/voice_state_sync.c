/** 设备状态同步（control protocol v2）：按连接做握手、带 revision 的状态快照和请求校验。
 *
 * 模块边界：所有状态变更都只在 WSS owner 任务里发生；FSM 观察者只投递事件，不得在这里发起
 * 网络操作。其它任务只能读 is_ready()；session_id 是 owner-only 的，内容会在 start/end 被改写，
 * 不得跨任务读取或缓存。
 *
 * 关闭 CONFIG_JULIA_CLOUD_STATE_SYNC_ENABLE 时本文件退化为空实现：is_ready() 恒为 true、
 * session_id 为空串、handle_text() 恒返回 false，语音链路按无状态同步方式继续工作。
 */
#include "julia_power.h"
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
/* 未收到 ACK 时的重发间隔，单位 us；起始为 2 秒，配合下方 3 次上限约为 6 秒的重试窗口。 */
#define SYNC_RETRY_US 2000000LL
/* 同一条待确认报文最多发送次数（含首发的 1 次和 2 次重发）；超限即认为云端不可同步。 */
#define SYNC_MAX_SENDS 3U
/* require_wake 响应缓存槽数；命中缓存即重发原响应，保证同一 request_id 幂等。 */
#define REQUEST_CACHE_SIZE 8U
/* request_id 与 response 的存储上限，单位字节；超长的 id 作为非法输入直接拒绝。 */
#define REQUEST_ID_BYTES 64U
#define STATE_MESSAGE_BYTES 512U
static const char *TAG = "voice_state_sync";
static portMUX_TYPE s_sync_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready;
static bool s_active;
/* 每次 start 重新生成的会话标识，仅 WSS owner 读取，end 时清空；只用于归属比对，
 * 它不是授权凭据，云端仍须校验设备身份。 */
static char s_session_id[33];
static char s_device_id[NATIVE_OTA_DEVICE_ID_SIZE];
static char s_pending[STATE_MESSAGE_BYTES];
static char s_pending_request[64];
static uint32_t s_pending_revision;
static bool s_waiting_ack;
/* CPU 升频的配对状态：首个待确认请求时获取一次，ACK／会话结束／发送失败三处各释放一次，
 * 覆盖全部退出路径；重复获取会破坏配对计数，因此必须先看这个标志。 */
static bool s_sync_cpu_boost;
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

/* owner-only：返回静态缓冲地址，内容在 start/end 被改写，调用方不得保存该指针或跨任务使用。 */
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

/* 只允许 [A-Za-z0-9_.:-] 且不超过 REQUEST_ID_BYTES 的 id 进入日志、缓存键和 JSON 字段：
 * 这个白名单同时挡住把引号或换行注入报文的构造。 */
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
    bool boosted = julia_power_boost_begin();
    esp_err_t err = wss_transport_send_now(0x1, (const uint8_t *)text, strlen(text));
    if (boosted) julia_power_boost_end();
    if (err == ESP_OK) return true;
    /* 发不出去说明连接已经不可用：先复位就绪状态和会话，再由 WSS owner 统一重连。 */
    set_ready(false);
    s_active = false;
    wss_transport_fail_session();
    return false;
}

/* 生成待确认的状态报文。序列化放不下就放弃本次会话：截断的 JSON 到了云端只会被当成
 * 非法状态，重发也没有意义，不如让上层重连后重新握手。 */
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
    if (!s_sync_cpu_boost) s_sync_cpu_boost = julia_power_boost_begin();
    s_last_revision = snapshot->revision;
    s_have_revision = true;
    s_waiting_ack = true;
    s_sends = 0;
    s_retry_us = 0;
}

/* 结束或失败路径的统一清场：先还掉 CPU 升频，再清就绪、待确认、revision 和会话 id，
 * 使下一条连接必须重新握手，不能复用本会话的任何结论。 */
void voice_state_sync_end(void)
{
    if (s_sync_cpu_boost) { julia_power_boost_end(); s_sync_cpu_boost = false; }
    set_ready(false);
    s_active = false;
    s_waiting_ack = false;
    s_have_revision = false;
    s_session_id[0] = '\0';
    memset(s_results, 0, sizeof(s_results));
    s_result_next = 0;
}

/* 新会话入口：先完整复位上一次会话（含 session_id、响应缓存和 CPU 升频），再重新生成身份。
 * 设备身份不可用时无法向云端证明归属，直接拒绝本次会话而不是发一个无主的 session_sync。
 * session_id 由 16 字节随机数逐字节转十六进制，只用于归属比对，不能当作授权凭据。 */
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

/* 由 WSS owner 每轮调用，负责握手重发和状态快照的变更检测。s_sends 超限说明云端始终没有
 * 确认状态，会话不能带着不确定的状态继续跑，因此在这里自行结束；多设备模式下额外退避 30 秒，
 * 避免多台设备对同一个不可用云端同时重连。 */
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

/* 处理云端的 require_wake 请求。request_id 命中响应缓存时原样重发上一次的响应，保证同一次
 * 请求重复到达不会产生两次唤醒动作；未命中才真正尝试切换状态。
 * reason 的取值来自两处判断：stale_state 表示云端依据的 revision 已不是当前快照（含尝试后
 * 又被其它事件推进的情况），interaction_active 表示设备正处在 S2/S4 的交互中，
 * state_unavailable 则是运行时不接受该请求。 */
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

/* 校验顺序固定为 type →（多设备下）device_id/request_id → control_protocol → session_id：
 * 前面的字段决定这条报文是不是本模块、本设备、本协议的，最后才确认会话归属。session_id 不符
 * 时静默忽略但仍返回 true——报文已被本模块认领，不能让别的命令处理器再解释一遍。 */
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
            /* 握手期间可能已经提交过新状态：丢掉缓存的 revision，让 poll 立即补发当前
             * device_state，而不是等下一次状态变化才同步。 */
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
    if (!s_waiting_ack && s_sync_cpu_boost) {
        julia_power_boost_end();
        s_sync_cpu_boost = false;
    }
    cJSON_Delete(root);
    return true;
}
#else
/* 关闭状态同步时的占位实现：调用方无需条件编译。is_ready 恒为 true 表示不设门槛，
 * session_id 为空串使捕获/判决类消息一律无法匹配会话，handle_text 返回 false 把文本交给
 * 后续处理器。 */
const char *voice_state_sync_session_id(void) { return ""; }
void voice_state_sync_start(void) {}
void voice_state_sync_end(void) {}
void voice_state_sync_poll(void) {}
bool voice_state_sync_is_ready(void) { return true; }
bool voice_state_sync_handle_text(const uint8_t *text, size_t len)
{ (void)text; (void)len; return false; }
#endif
