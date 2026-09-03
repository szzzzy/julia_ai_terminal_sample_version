/**
 * @file    voice_service.c
 * @brief   语音业务服务实现：命令语法、WSS 文件推送协议与入口装配。
 *
 * 模块关系：
 * - 通过 mqtt_comm_register_topic() 注册语音命令 topic；原有纯文本控制与 WSS
 *   语义一致，S4/S2 的 intent_result JSON 只由 MQTT 控制面解析；
 * - 传输由纯传输层 wss_transport 完成（连接、帧、握手、保活、重连）；
 * - FILE_SEND 的 URI 映射由 voice_uri 完成；
 * - 所有对外接口只做有界入队，实际发送在 WSS 会话任务上下文中执行。
 *
 * 并发/上下文：
 * - MQTT 事件任务：voice_service_on_mqtt_command 解析命令 -> voice_service_* 入队；
 * - mic_task：board_audio 的 PCM1 帧 -> voice_service_on_board_audio_frame ->
 *   voice_service_send_chunk() 入队；
 * - WSS 会话任务处理控制、收发和文件块；voice_playback 独立消费 PCM，
 *   完成事件由 on_poll 收回，FSM 对话阶段仍只由 WSS 会话推进。
 * - 对话阶段只由 WSS 会话任务推进；陪伴上传超时由 esp_timer 回调关闭，
 *   因而上传/对话标志由 s_mic_state_lock 保护。
 */

#include "voice_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"

#include "board_audio.h"
#include "julia_avatar.h"
#include "julia_idle_display.h"
#include "julia_fsm_runtime.h"
#include "mqtt_comm.h"
#include "voice_uri.h"
#include "voice_playback.h"
#include "wss_transport.h"
#include "sdkconfig.h"

/* 可选的共享 SD 所有权钩子；默认弱实现不加锁。 */
__attribute__((weak)) bool julia_wireless_sd_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}
__attribute__((weak)) void julia_wireless_sd_unlock(void) {}

/** 本模块统一使用的日志标签。 */
static const char *TAG = "voice_service";

static void post_fsm_event(fsm_event_t event)
{
    esp_err_t err = julia_fsm_runtime_post(event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FSM event %s rejected: %s", julia_fsm_event_name(event),
                 esp_err_to_name(err));
    }
}

/**
 * WSS 作业队列深度。MIC 每 20 ms 产生一帧，8 槽可吸收约 160 ms 的
 * 短时网络/调度抖动；队列仍保持有界，避免弱网时无限占用内部 RAM。
 */
#define VOICE_QUEUE_DEPTH 8
#define VOICE_INTERACTION_ID_MAX_LEN 64

/**
 * @brief 命令队列中的一条作业：文件 URI（含 NUL）或一块 MIC 数据。
 *
 * 作业对传输层是纯不透明条目；本模块在 on_queue_item 回调中解释它。
 */
typedef enum {
    VOICE_JOB_SEND_FILE = 0, /**< FILE_SEND：推送一个音频文件。 */
    VOICE_JOB_MIC_START,     /**< MIC_START：确认用户开始一轮说话。 */
    VOICE_JOB_MIC_STOP,      /**< MIC_STOP：确认本轮用户说话结束。 */
    VOICE_JOB_SEND_CHUNK,    /**< 发送一个 MIC 音频块。 */
    VOICE_JOB_SEND_TEXT,     /**< FSM 已提交状态后的 WSS 文本通知。 */
    VOICE_JOB_INTENT_GOODNIGHT, /**< MQTT 晚安语义，转交 WSS 任务串行收尾。 */
    VOICE_JOB_INTENT_DISMISS,   /**< MQTT 结束沟通语义，转交 WSS 任务串行收尾。 */
} voice_job_type_t;

typedef struct {
    voice_job_type_t type;                  /**< 作业类型。 */
    size_t len;                             /**< data 的有效字节数。 */
    uint8_t data[WSS_TRANSPORT_MAX_PAYLOAD]; /**< URI 或音频块内容。 */
} voice_job_t;

/** 上传与语义监听解耦：陪伴期可 streaming=true、listening=false。 */
static portMUX_TYPE s_mic_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_mic_streaming;
static bool s_dialog_listening;
static char s_interaction_id[VOICE_INTERACTION_ID_MAX_LEN];
static julia_main_state_t s_interaction_origin = JULIA_MAIN_STATE_S1_COMPANION;
static bool s_s4_ready_pending;
static bool s_wake_reply_expected;

typedef enum {
    VOICE_PLAYBACK_ROLE_NONE = 0,
    VOICE_PLAYBACK_ROLE_WAKE_REPLY,
    VOICE_PLAYBACK_ROLE_DIALOG_REPLY,
    VOICE_PLAYBACK_ROLE_SELF_TEST,
} voice_playback_role_t;

/* These fields are owned exclusively by the WSS session task. */
static uint32_t s_playback_generation;
static voice_playback_role_t s_playback_role;
static FILE *s_file;
static uint64_t s_file_size;
static uint64_t s_file_sent;
static uint32_t s_uplink_dropped;
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
static bool s_companion_timer_armed;
static esp_timer_handle_t s_companion_timer;
#endif

/** FILE_SEND 允许的最大文件大小（8 MiB，融合方案 §9.6 二次限制）。 */
#define VOICE_SEND_MAX_FILE_BYTES (8 * 1024 * 1024)

static esp_err_t voice_service_enqueue(voice_job_type_t type,
                                       const uint8_t *data, size_t len);
static void voice_service_apply_terminal_intent(fsm_event_t event,
                                                const char *intent);

static bool voice_service_mic_is_streaming(void)
{
    bool streaming;
    portENTER_CRITICAL(&s_mic_state_lock);
    streaming = s_mic_streaming;
    portEXIT_CRITICAL(&s_mic_state_lock);
    return streaming;
}

#if CONFIG_JULIA_SERVER_WAKE_ENABLE
static void voice_service_disarm_companion_timer(void) {}
#else
static void voice_service_disarm_companion_timer(void)
{
    portENTER_CRITICAL(&s_mic_state_lock);
    s_companion_timer_armed = false;
    portEXIT_CRITICAL(&s_mic_state_lock);

    if (s_companion_timer != NULL) {
        esp_err_t err = esp_timer_stop(s_companion_timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Companion timer stop failed: %s", esp_err_to_name(err));
        }
    }
}

/** 本地唤醒模式下，十分钟无交互会关闭陪伴 PCM 上传；显示策略同时进入 S3。 */
static void voice_service_companion_timeout(void *arg)
{
    (void)arg;
    bool stopped = false;

    portENTER_CRITICAL(&s_mic_state_lock);
    if (s_companion_timer_armed) {
        s_companion_timer_armed = false;
        s_mic_streaming = false;
        s_dialog_listening = false;
        board_audio_enable_wss_mic(false);
        stopped = true;
    }
    portEXIT_CRITICAL(&s_mic_state_lock);

    if (stopped) {
        ESP_LOGI(TAG, "Companion MIC upload stopped after %d idle seconds",
                 CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS);
    }
}

static void voice_service_arm_companion_timer(void)
{
    if (s_companion_timer == NULL) {
        ESP_LOGE(TAG, "Companion timer unavailable; stopping MIC upload");
        portENTER_CRITICAL(&s_mic_state_lock);
        s_mic_streaming = false;
        s_dialog_listening = false;
        board_audio_enable_wss_mic(false);
        portEXIT_CRITICAL(&s_mic_state_lock);
        return;
    }

    voice_service_disarm_companion_timer();
    esp_err_t err = esp_timer_start_once(
        s_companion_timer,
        (uint64_t)CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Companion timer start failed: %s; stopping MIC upload",
                 esp_err_to_name(err));
        portENTER_CRITICAL(&s_mic_state_lock);
        s_mic_streaming = false;
        s_dialog_listening = false;
        board_audio_enable_wss_mic(false);
        portEXIT_CRITICAL(&s_mic_state_lock);
        return;
    }

    portENTER_CRITICAL(&s_mic_state_lock);
    s_companion_timer_armed = true;
    portEXIT_CRITICAL(&s_mic_state_lock);
    ESP_LOGI(TAG, "Companion MIC upload armed for %d seconds",
             CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS);
}
#endif

/**
 * @brief board_audio 的 WSS sink 适配器：完整 PCM1 帧（16 B 头 + PCM）
 * 作为一个二进制 WSS 消息入队。WSS 未启动时静默丢弃，不阻塞 mic_task。
 */
static void voice_service_on_board_audio_frame(const uint8_t *frame, size_t bytes, void *ctx)
{
    (void)ctx;
    if (voice_service_send_chunk(frame, bytes) != ESP_OK) {
        /* 只有该 MIC 回调写计数器；按累计值节流日志，避免每帧刷屏。 */
        if ((++s_uplink_dropped & 255U) == 1U) {
            ESP_LOGW(TAG, "MIC enqueue drops=%lu", (unsigned long)s_uplink_dropped);
        }
    }
}

/** 是否为允许外发的文件扩展名（当前仅 .wav）。 */
static bool voice_service_path_is_allowed(const char *path)
{
    size_t len = strlen(path);
    if (len < 4) {
        return false;
    }
    return strcasecmp(path + len - 4, ".wav") == 0;
}

/**
 * @brief 向服务端回一条 ERROR 文本帧；写出失败会由传输层标记会话故障。
 *
 * @param[in] text NUL 结尾的错误文本，不允许为 NULL。
 */
static esp_err_t voice_service_send_error(const char *text)
{
    return wss_transport_send_now(0x1, (const uint8_t *)text, strlen(text));
}

/**
 * @brief 推送一个音频文件（仅在 WSS 会话任务上下文中调用）。
 *
 * 打开失败向服务端回复 "ERROR file_open_failed"；成功后按协议发送
 * "BEGIN FILE <size> <name>"、若干 1200 B 二进制帧和 "END <bytes>"。
 * 此函数只打开文件并发送 BEGIN；on_poll 每轮推进一个块，完整结束才发送 END。
 *
 * @param[in] uri NUL 结尾的文件 URI，长度受 VOICE_SERVICE_URI_MAX_LEN 约束。
 * @return ESP_OK 传输成功或命令被安全拒绝（会话仍健康）；
 * @return ESP_FAIL 传输中途失败，会话必须关闭重连。
 */
static esp_err_t voice_service_push_file(const char *uri)
{
    portENTER_CRITICAL(&s_mic_state_lock);
    bool listening = s_dialog_listening;
    portEXIT_CRITICAL(&s_mic_state_lock);
    if (s_file != NULL || voice_playback_is_active() || listening) {
        (void)voice_service_send_error("ERROR file_busy");
        return ESP_OK;
    }
    char path[VOICE_SERVICE_URI_MAX_LEN + 16];
    if (!voice_uri_to_path(uri, path, sizeof(path))) {
        (void)voice_service_send_error("ERROR bad_uri");
        ESP_LOGW(TAG, "Unsupported FILE_SEND uri: %s", uri);
        return ESP_OK;
    }
    if (!voice_service_path_is_allowed(path)) {
        (void)voice_service_send_error("ERROR bad_extension");
        ESP_LOGW(TAG, "FILE_SEND extension rejected: %s", path);
        return ESP_OK;
    }

    /* 文件访问必须持有 SD 锁（融合方案 §9.6）。 */
    if (!julia_wireless_sd_lock(0)) {
        (void)voice_service_send_error("ERROR sd_busy");
        ESP_LOGW(TAG, "FILE_SEND rejected: SD lock busy");
        return ESP_OK;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        julia_wireless_sd_unlock();
        (void)voice_service_send_error("ERROR file_open_failed");
        ESP_LOGW(TAG, "Cannot open %s", path);
        return ESP_OK;
    }

    /* 先取文件大小并回到开头：fseek/ftell 任一失败都不能启动传输。 */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        julia_wireless_sd_unlock();
        (void)voice_service_send_error("ERROR file_size_failed");
        ESP_LOGW(TAG, "Cannot seek to end of %s", path);
        return ESP_OK;
    }
    long size = ftell(f);
    if (size < 0 || size > VOICE_SEND_MAX_FILE_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        julia_wireless_sd_unlock();
        (void)voice_service_send_error("ERROR file_size_failed");
        ESP_LOGW(TAG, "Cannot determine size of %s (or exceeds limit)", path);
        return ESP_OK;
    }
    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;

    char begin[VOICE_SERVICE_URI_MAX_LEN + 64];
    int n = snprintf(begin, sizeof(begin), "BEGIN FILE %ld %s", size, base);
    if (n <= 0 || (size_t)n >= sizeof(begin)) {
        fclose(f);
        julia_wireless_sd_unlock();
        (void)voice_service_send_error("ERROR file_name_too_long");
        ESP_LOGW(TAG, "BEGIN frame too long for %s", path);
        return ESP_OK;
    }
    if (wss_transport_send_now(0x1, (const uint8_t *)begin, (size_t)n) != ESP_OK) {
        ESP_LOGE(TAG, "BEGIN frame send failed for %s", path);
        fclose(f);
        julia_wireless_sd_unlock();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Pushing %s (%ld bytes)", path, size);

    s_file = f;
    s_file_size = (uint64_t)size;
    s_file_sent = 0;
    return ESP_OK;
}

static void voice_service_close_file(void)
{
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
        julia_wireless_sd_unlock();
    }
}

static void voice_service_cancel_file(void)
{
    if (s_file != NULL) {
        voice_service_close_file();
        (void)voice_service_send_error("ERROR file_cancelled");
    }
}

/* 每轮会话最多推进一个有界文件块，确保控制、接收和 PING 都有执行机会。
 * 文件 END（或 ERROR）之前暂停 MIC 二进制帧，避免同一连接上的数据语义混淆。 */
static void voice_service_file_poll(void)
{
    if (s_file == NULL) return;
    uint8_t buf[WSS_TRANSPORT_MAX_PAYLOAD];
    size_t got = fread(buf, 1, sizeof(buf), s_file);
    if (got > 0) {
        if (s_file_sent + got > s_file_size ||
            wss_transport_send_now(0x2, buf, got) != ESP_OK) {
            voice_service_close_file();
            wss_transport_fail_session();
            return;
        }
        s_file_sent += got;
        return;
    }
    bool ok = !ferror(s_file) && s_file_sent == s_file_size;
    voice_service_close_file();
    if (!ok) {
        ESP_LOGW(TAG, "File read incomplete: %" PRIu64 "/%" PRIu64, s_file_sent, s_file_size);
        wss_transport_fail_session();
        return;
    }
    char end[48];
    int n = snprintf(end, sizeof(end), "END %" PRIu64, s_file_sent);
    (void)wss_transport_send_now(0x1, (const uint8_t *)end, (size_t)n);
}

static void voice_service_speaker_done(void)
{
    voice_playback_role_t completed_role = s_playback_role;
    s_playback_generation = 0;
    s_playback_role = VOICE_PLAYBACK_ROLE_NONE;
    julia_avatar_talking_stop();

    if (completed_role == VOICE_PLAYBACK_ROLE_WAKE_REPLY) {
        /* 唤醒回应属于 S4 交互建立，不是 S2 正常回答。播放结束后保持 S4，
         * 等服务器确认实际话语开始时再接收 MIC_START。 */
        s_wake_reply_expected = false;
        julia_idle_display_note_activity();
        julia_idle_display_set_busy(false);
        ESP_LOGI(TAG, "Wake reply completed; remaining in S4");
        return;
    }
    if (completed_role == VOICE_PLAYBACK_ROLE_SELF_TEST) {
        julia_idle_display_note_activity();
        julia_idle_display_set_busy(false);
        return;
    }

    portENTER_CRITICAL(&s_mic_state_lock);
    s_dialog_listening = false;
    if (s_mic_streaming) board_audio_mic_wake();
    portEXIT_CRITICAL(&s_mic_state_lock);
    if (completed_role == VOICE_PLAYBACK_ROLE_DIALOG_REPLY) {
        post_fsm_event(EVT_SILENCE_TIMEOUT);
    }
    julia_idle_display_note_activity();
    julia_idle_display_set_busy(false);
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
    if (voice_service_mic_is_streaming()) voice_service_arm_companion_timer();
#endif
}

static void voice_service_poll(void)
{
    uint32_t generation;
    esp_err_t result;
    if (voice_playback_take_completion(&generation, &result) &&
        generation == s_playback_generation) {
        voice_service_speaker_done();
        if (result != ESP_OK) {
            (void)voice_service_send_error(result == ESP_ERR_NO_MEM ? "ERROR playback_overflow" :
                                          result == ESP_ERR_TIMEOUT ? "ERROR playback_timeout" :
                                                                     "ERROR playback_failed");
        }
    }
    voice_service_file_poll();
}

static void voice_service_played_pcm(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    julia_avatar_feed_pcm(pcm, samples);
}

/** MIC_START 表示一轮用户发言开始，并在需要时同时开启 PCM 上传。 */
static void voice_service_apply_mic_start(void)
{
    voice_service_disarm_companion_timer();
    julia_idle_display_note_activity();

    julia_main_state_t state = julia_fsm_runtime_get_state();
    julia_s2_sub_state_t s2_sub_state = julia_fsm_runtime_get_s2_sub_state();
    voice_service_cancel_file();
    bool interrupted_speaker = voice_playback_is_active();
    voice_playback_stop();
    s_playback_generation = 0;
    s_playback_role = VOICE_PLAYBACK_ROLE_NONE;
    julia_avatar_talking_stop();

    bool already_listening;
    bool started_streaming = false;
    portENTER_CRITICAL(&s_mic_state_lock);
    already_listening = s_dialog_listening;
    s_dialog_listening = true;
    if (!s_mic_streaming) {
        s_mic_streaming = true;
        board_audio_mic_wake();
        board_audio_enable_wss_mic(true);
        started_streaming = true;
    }
    portEXIT_CRITICAL(&s_mic_state_lock);

    julia_idle_display_set_busy(true);
    if (state == JULIA_MAIN_STATE_S3_STANDBY ||
        state == JULIA_MAIN_STATE_S5_SILENT ||
        state == JULIA_MAIN_STATE_S6_SLEEP) {
        /* 兼容旧服务端/本地 WakeNet：没有 wake_detected 时，MIC_START 仍可可靠地
         * 先把低活动状态推进到 S4；扬声器或残留 listening 不得改投 INTERRUPT。 */
        portENTER_CRITICAL(&s_mic_state_lock);
        s_interaction_origin = state;
        s_wake_reply_expected = true;
        portEXIT_CRITICAL(&s_mic_state_lock);
        post_fsm_event(EVT_WAKEUP);
    } else if (state == JULIA_MAIN_STATE_S1_COMPANION) {
        if (!already_listening) post_fsm_event(EVT_USER_CALL);
    } else if (state == JULIA_MAIN_STATE_S4_INTERACTION) {
        /* S4 已完成交互建立：MIC_START 只标记实际话语开始。若用户在唤醒回应
         * 播放时插话，上方已取消回应，后续正常回答由 MIC_STOP/SPKS 推进。 */
        if (interrupted_speaker) s_wake_reply_expected = false;
    } else if (state == JULIA_MAIN_STATE_S2_DIALOG &&
               s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING) {
        /* 正常回答期间插话，复用 S2.3 -> S2.1 的中断事件。 */
        post_fsm_event(EVT_INTERRUPT);
    }
    ESP_LOGI(TAG, "MIC_START: state=%s/%s listening%s%s",
             julia_fsm_main_state_name(state),
             julia_fsm_s2_sub_state_name(s2_sub_state),
             started_streaming ? ", PCM upload started" : ", PCM upload already active",
             interrupted_speaker ? ", speaker interrupted" : "");
}

/** MIC_STOP 表示本轮用户发言结束，但在陪伴窗口内继续保留 PCM 上传能力。 */
static void voice_service_apply_mic_stop(void)
{
    julia_idle_display_note_activity();

    bool was_listening;
    portENTER_CRITICAL(&s_mic_state_lock);
    was_listening = s_dialog_listening;
    s_dialog_listening = false;
    portEXIT_CRITICAL(&s_mic_state_lock);

    if (!was_listening) {
        ESP_LOGD(TAG, "MIC_STOP ignored: no active utterance");
        return;
    }

    /* “想”阶段继续保持忙碌；传输连接保留到后续 SPKE -> IDLE 陪伴窗口，
     * 同时供服务端动态噪声 VAD 继续使用。 */
    julia_idle_display_set_busy(true);
    if (was_listening) {
        post_fsm_event(EVT_START_DIALOG);
    }
    ESP_LOGI(TAG, "MIC_STOP: utterance ended; PCM upload retained");
}

static bool interaction_id_is_valid(const char *value)
{
    if (value == NULL || value[0] == '\0' ||
        strlen(value) >= VOICE_INTERACTION_ID_MAX_LEN) return false;
    for (const char *p = value; *p != '\0'; ++p) {
        bool allowed = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
                       *p == '.' || *p == ':';
        if (!allowed) return false;
    }
    return true;
}

/** 解析服务器唤醒握手；返回 true 表示该 JSON 已处理，不再按旧文本命令解析。 */
static bool voice_service_handle_wake_json(const uint8_t *text, size_t len)
{
    if (len == 0U || text[0] != '{') return false;
    cJSON *root = cJSON_ParseWithLength((const char *)text, len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "wake_detected") != 0) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "interaction_id");
    if (!cJSON_IsString(id) || !interaction_id_is_valid(id->valuestring)) {
        ESP_LOGW(TAG, "Ignoring wake_detected with invalid interaction_id");
        cJSON_Delete(root);
        return true;
    }

    julia_main_state_t state = julia_fsm_runtime_get_state();
    if (state == JULIA_MAIN_STATE_S4_INTERACTION) {
        ESP_LOGW(TAG, "Ignoring wake_detected in S4; actual speech must use MIC_START");
        cJSON_Delete(root);
        return true;
    }
    if (state != JULIA_MAIN_STATE_S3_STANDBY &&
        state != JULIA_MAIN_STATE_S5_SILENT &&
        state != JULIA_MAIN_STATE_S6_SLEEP) {
        ESP_LOGW(TAG, "Ignoring wake_detected in state=%s",
                 julia_fsm_main_state_name(state));
        cJSON_Delete(root);
        return true;
    }

    portENTER_CRITICAL(&s_mic_state_lock);
    strncpy(s_interaction_id, id->valuestring, sizeof(s_interaction_id) - 1U);
    s_interaction_id[sizeof(s_interaction_id) - 1U] = '\0';
    s_interaction_origin = state;
    s_s4_ready_pending = true;
    s_wake_reply_expected = true;
    s_dialog_listening = false;
    portEXIT_CRITICAL(&s_mic_state_lock);

    julia_idle_display_note_activity();
    julia_idle_display_set_busy(true);
    post_fsm_event(EVT_WAKEUP);
    ESP_LOGI(TAG, "wake_detected id=%s origin=%s; waiting for committed S4",
             id->valuestring, julia_fsm_main_state_name(state));
    cJSON_Delete(root);
    return true;
}

/**
 * @brief 处理服务端下发的完整文本帧命令（wss_transport on_text 回调）。
 *
 * 在会话任务上下文中执行：FILE_SEND 启动分块推送；MIC_START/MIC_STOP 直接调用
 * 与 MQTT 队列作业共用的内部状态应用函数，避免被 PCM1 队列挤占。
 *
 * @param[in] text 文本载荷，不要求以 NUL 结尾。
 * @param[in] len  文本长度。
 */
static void voice_service_on_server_text(const uint8_t *text, size_t len)
{
    ESP_LOGI(TAG, "Server cmd: %.*s", (int)len, (const char *)text);
    if (voice_service_handle_wake_json(text, len)) return;
    if (len > strlen("FILE_SEND ") && strncmp((const char *)text, "FILE_SEND ", 10) == 0) {
        char uri[VOICE_SERVICE_URI_MAX_LEN];
        size_t uri_len = len - 10;
        if (uri_len >= sizeof(uri)) {
            ESP_LOGW(TAG, "Server FILE_SEND uri too long");
            return;
        }
        memcpy(uri, text + 10, uri_len);
        uri[uri_len] = '\0';
        (void)voice_service_push_file(uri);
        return;
    }

    if (len == strlen("MIC_START") && memcmp(text, "MIC_START", len) == 0) {
        voice_service_apply_mic_start();
        return;
    }
    if (len == strlen("MIC_STOP") && memcmp(text, "MIC_STOP", len) == 0) {
        voice_service_apply_mic_stop();
        return;
    }

    /* ---- 板级音频下行控制命令（融合方案 §9.4，与最小包 USB 命令同语义） ----
     * 这是服务器经 WSS 下行文本帧下发的控制面：SPKS 开播 / SPKV 音量 /
     * SPKE 停播 / SPKT 自检 / MICS 休眠触发 / MICW 恢复上传，全部只在
     * WSS 会话任务上下文执行（on_text 回调）。与上行（MIC 推流）无竞态，
     * 但与在下行 PCM（on_binary）之间隐含着顺序约束：必须先 SPKS 成功
     * 才允许写 PCM，否则 PCM 被 on_binary 丢弃（见 voice_service_on_binary）。 */
    if (len == 4 && memcmp(text, "SPKE", 4) == 0) {
        /* END 排在所有已接收 PCM 之后；播放任务负责报告完成。
         * 已被打断或不存在的播放代次无需执行结束动作。 */
        if (s_playback_generation != 0) voice_playback_finish();
        ESP_LOGI(TAG, "SPKE: draining accepted playback");
    } else if (len == 4 && memcmp(text, "SPKT", 4) == 0) {
        voice_service_disarm_companion_timer();
        if (voice_playback_start(24000, true, &s_playback_generation) == ESP_OK) {
            s_playback_role = VOICE_PLAYBACK_ROLE_SELF_TEST;
            voice_service_cancel_file();
            julia_idle_display_set_busy(true);
        }
        ESP_LOGI(TAG, "SPKT: asynchronous local tone test");
    } else if (len > 5 && memcmp(text, "SPKS ", 5) == 0) {
        char buf[16];
        size_t n = len - 5;
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, text + 5, n);
        buf[n] = '\0';
        char *end = NULL;
        long rate = strtol(buf, &end, 10);
        julia_main_state_t state = julia_fsm_runtime_get_state();
        julia_s2_sub_state_t sub_state = julia_fsm_runtime_get_s2_sub_state();
        voice_playback_role_t role = VOICE_PLAYBACK_ROLE_NONE;
        if (state == JULIA_MAIN_STATE_S4_INTERACTION && s_wake_reply_expected) {
            role = VOICE_PLAYBACK_ROLE_WAKE_REPLY;
        } else if (state == JULIA_MAIN_STATE_S2_DIALOG &&
                   sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING) {
            role = VOICE_PLAYBACK_ROLE_DIALOG_REPLY;
        }
        if (end != buf && *end == '\0' && role != VOICE_PLAYBACK_ROLE_NONE &&
            voice_playback_start((uint32_t)rate, false, &s_playback_generation) == ESP_OK) {
            s_playback_role = role;
            if (role == VOICE_PLAYBACK_ROLE_WAKE_REPLY) s_wake_reply_expected = false;
            voice_service_cancel_file();
            voice_service_disarm_companion_timer();
            julia_avatar_talking_start();
            /* 唤醒回应属于 S4，绝不推进 S2；只有正常回答从 S2.2 进入 S2.3。 */
            if (role == VOICE_PLAYBACK_ROLE_DIALOG_REPLY) {
                post_fsm_event(EVT_MULTI_TURN_DETECTED);
            }
            julia_idle_display_note_activity();
            julia_idle_display_set_busy(true);
            ESP_LOGI(TAG, "SPKS: speaker start rate=%ld role=%s", rate,
                     role == VOICE_PLAYBACK_ROLE_WAKE_REPLY ? "wake_reply" : "dialog_reply");
        } else if (end != buf && *end == '\0' && role == VOICE_PLAYBACK_ROLE_NONE) {
            ESP_LOGW(TAG, "SPKS rejected in state=%s/%s: no playback role",
                     julia_fsm_main_state_name(state),
                     julia_fsm_s2_sub_state_name(sub_state));
            (void)voice_service_send_error("ERROR playback_state");
        } else {
            ESP_LOGW(TAG, "Invalid SPKS rate: %.*s", (int)n, buf);
        }
    } else if (len > 5 && memcmp(text, "SPKV ", 5) == 0) {
        char buf[16];
        size_t n = len - 5;
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, text + 5, n);
        buf[n] = '\0';
        char *end = NULL;
        long volume = strtol(buf, &end, 10);
        if (end != buf && volume >= 0 && volume <= 100) {
            board_audio_speaker_set_volume((uint8_t)volume);
            ESP_LOGI(TAG, "SPKV: volume=%ld", volume);
        } else {
            ESP_LOGW(TAG, "Invalid SPKV volume: %.*s", (int)n, buf);
        }
    } else if (len > 5 && memcmp(text, "MICS ", 5) == 0) {
#if CONFIG_JULIA_SERVER_WAKE_ENABLE
        ESP_LOGW(TAG, "MICS ignored: server wake mode requires continuous PCM upload");
#else
        char buf[16];
        size_t n = len - 5;
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, text + 5, n);
        buf[n] = '\0';
        char *end = NULL;
        long bg = strtol(buf, &end, 10);
        if (end != buf && bg >= -10000 && bg <= 0) {
            board_audio_mic_sleep((int16_t)bg);
            ESP_LOGI(TAG, "MICS: sleep trigger bg=%ld", bg);
        } else {
            ESP_LOGW(TAG, "Invalid MICS bg: %.*s", (int)n, buf);
        }
#endif
    } else if (len == 4 && memcmp(text, "MICW", 4) == 0) {
        board_audio_mic_wake();
        ESP_LOGI(TAG, "MICW: wake, continuous upload");
    } else {
        ESP_LOGW(TAG, "Ignoring unknown server text command");
    }
}

/**
 * @brief WSS 下行 PCM 复制到独立播放任务的有界缓冲，不在此写 I2S。
 *
 * 前置约束：必须已由 SPKS 开始播放（board_audio_speaker_is_playing()），
 * 否则整帧丢弃（§9.6 保护）——服务器推流顺序要求"先 SPKS，再 PCM 帧"。
 * 长度必须非 0 且为偶数：PCM 是 16-bit mono，偶数长度才能被安全地当作
 * int16 数组喂给 `julia_avatar_feed_pcm()`（否则越界/错位）。
 */
static void voice_service_on_binary(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U || (len & 1U) != 0U) {
        return;
    }
    if (!voice_playback_is_active()) {
        ESP_LOGW(TAG, "Dropping %u-byte downlink PCM: speaker not started", (unsigned)len);
        return;
    }
    esp_err_t err = voice_playback_write(data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Downlink PCM write failed: %s", esp_err_to_name(err));
    }
}

/**
 * @brief 处理命令队列中的一条作业（wss_transport on_queue_item 回调）。
 *
 * 在会话任务上下文中执行；FILE_SEND 启动分块传输，MIC 块仅在流式状态
 * 开启且会话有效时发送。
 *
 * @param[in] item      队列条目，不允许为 NULL。
 * @param[in] item_size 条目大小，必须等于 sizeof(voice_job_t)。
 */
static void voice_service_on_queue_item(void *item, size_t item_size)
{
    if (item == NULL || item_size != sizeof(voice_job_t)) {
        ESP_LOGW(TAG, "Malformed queued voice job");
        return;
    }
    voice_job_t *job = (voice_job_t *)item;
    switch (job->type) {
    case VOICE_JOB_SEND_FILE:
        job->data[VOICE_SERVICE_URI_MAX_LEN - 1] = '\0';
        (void)voice_service_push_file((const char *)job->data);
        break;
    case VOICE_JOB_MIC_START:
        voice_service_apply_mic_start();
        break;
    case VOICE_JOB_MIC_STOP:
        voice_service_apply_mic_stop();
        break;
    case VOICE_JOB_SEND_TEXT:
        if (wss_transport_send_now(0x1, job->data, job->len) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send %u-byte control text", (unsigned)job->len);
        }
        break;
    case VOICE_JOB_INTENT_GOODNIGHT:
        voice_service_apply_terminal_intent(EVT_INTENT_GOODNIGHT, "goodnight");
        break;
    case VOICE_JOB_INTENT_DISMISS:
        voice_service_apply_terminal_intent(EVT_INTENT_DISMISS, "dismiss");
        break;
    case VOICE_JOB_SEND_CHUNK:
        if (s_file != NULL) {
            /* BEGIN FILE..END is a file-only binary interval. */
            break;
        } else if (!voice_service_mic_is_streaming()) {
            ESP_LOGW(TAG, "Dropping %u-byte MIC chunk: streaming is not active",
                     (unsigned)job->len);
        } else if (wss_transport_send_now(0x2, job->data, job->len) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send %u-byte MIC chunk", (unsigned)job->len);
        }
        break;
    default:
        ESP_LOGW(TAG, "Unknown queued voice job type %d", (int)job->type);
        break;
    }
}

#if CONFIG_JULIA_SERVER_WAKE_ENABLE
/** Authenticated WSS means the server becomes the sole wake-word detector. */
static void voice_service_on_session_start(void)
{
    portENTER_CRITICAL(&s_mic_state_lock);
    s_mic_streaming = true;
    s_dialog_listening = false;
    s_s4_ready_pending = false;
    s_interaction_id[0] = '\0';
    board_audio_mic_wake();
    board_audio_enable_wss_mic(true);
    portEXIT_CRITICAL(&s_mic_state_lock);

    /* 后台唤醒监听只属于传输层；服务端发送 wake_detected 前，
     * 保持当前主状态与呈现不变。 */
    ESP_LOGI(TAG, "WSS session ready: IDLE PCM upload enabled for server wake detection");
}
#endif

/**
 * @brief WSS 会话结束回调：链路关闭后复位会话级 MIC 流式状态。
 */
static void voice_service_on_session_end(void)
{
    voice_service_disarm_companion_timer();
    portENTER_CRITICAL(&s_mic_state_lock);
    s_mic_streaming = false;
    s_dialog_listening = false;
    s_s4_ready_pending = false;
    s_wake_reply_expected = false;
    s_interaction_id[0] = '\0';
    board_audio_enable_wss_mic(false);
    portEXIT_CRITICAL(&s_mic_state_lock);
    voice_service_close_file();
    voice_playback_stop();
    s_playback_generation = 0;
    s_playback_role = VOICE_PLAYBACK_ROLE_NONE;
    julia_avatar_talking_stop();
    post_fsm_event(EVT_SILENCE_TIMEOUT);
    julia_idle_display_note_activity();
    julia_idle_display_set_busy(false);
}

/**
 * @brief 把一条作业复制到有界命令队列。
 *
 * @param[in] type 作业类型。
 * @param[in] data 作业数据首地址，len 为 0 时可为 NULL。
 * @param[in] len  数据长度，不超过队列块数据容量。
 * @return ESP_OK 已入队；ESP_ERR_NO_MEM 队列已满；ESP_ERR_INVALID_STATE 尚未启动。
 */
static esp_err_t voice_service_enqueue(voice_job_type_t type, const uint8_t *data, size_t len)
{
    if (len > WSS_TRANSPORT_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    voice_job_t job;
    memset(&job, 0, sizeof(job));
    job.type = type;
    job.len = len;
    if (data != NULL && len > 0U) {
        memcpy(job.data, data, len);
    }
    return type == VOICE_JOB_SEND_CHUNK ? wss_transport_enqueue(&job, sizeof(job))
                                       : wss_transport_enqueue_control(&job, sizeof(job));
}

/** FSM 提交 S4 后才排队 state_ready，保证服务器不会在状态尚未生效时开始播回应。 */
static void voice_service_on_fsm_state(julia_main_state_t main_state,
                                       julia_s2_sub_state_t s2_sub_state,
                                       fsm_event_t event, void *ctx)
{
    (void)s2_sub_state;
    (void)ctx;
    if (main_state != JULIA_MAIN_STATE_S4_INTERACTION || event != EVT_WAKEUP) return;

    char interaction_id[VOICE_INTERACTION_ID_MAX_LEN];
    bool pending;
    julia_main_state_t origin;
    portENTER_CRITICAL(&s_mic_state_lock);
    pending = s_s4_ready_pending;
    origin = s_interaction_origin;
    strncpy(interaction_id, s_interaction_id, sizeof(interaction_id) - 1U);
    interaction_id[sizeof(interaction_id) - 1U] = '\0';
    s_s4_ready_pending = false;
    portEXIT_CRITICAL(&s_mic_state_lock);
    if (!pending || interaction_id[0] == '\0') return;

    char payload[144];
    int len = snprintf(payload, sizeof(payload),
                       "{\"type\":\"state_ready\",\"interaction_id\":\"%s\","
                       "\"state\":\"S4\"}", interaction_id);
    if (len <= 0 || (size_t)len >= sizeof(payload)) {
        ESP_LOGW(TAG, "state_ready payload overflow");
        return;
    }
    esp_err_t err = voice_service_enqueue(VOICE_JOB_SEND_TEXT,
                                          (const uint8_t *)payload, (size_t)len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "state_ready enqueue failed id=%s: %s",
                 interaction_id, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "S4 committed; state_ready queued id=%s origin=%s",
                 interaction_id, julia_fsm_main_state_name(origin));
    }
}

/**
 * @brief 结束特殊语义对应的当前话语，避免迟到 MIC_STOP 再推进状态。
 */
static void voice_service_apply_terminal_intent(fsm_event_t event, const char *intent)
{
    julia_main_state_t state = julia_fsm_runtime_get_state();
    if (state != JULIA_MAIN_STATE_S4_INTERACTION &&
        state != JULIA_MAIN_STATE_S2_DIALOG) {
        ESP_LOGW(TAG, "Ignoring terminal intent=%s in state=%s", intent,
                 julia_fsm_main_state_name(state));
        return;
    }

    /* 终止语义是纯显示迁移，不允许残留唤醒回应或正常回答继续出声。该函数只在
     * WSS 会话任务中执行，因此可同时收回播放代次和业务角色。 */
    voice_playback_stop();
    s_playback_generation = 0;
    s_playback_role = VOICE_PLAYBACK_ROLE_NONE;
    s_wake_reply_expected = false;
    julia_avatar_talking_stop();

    bool was_listening;
    portENTER_CRITICAL(&s_mic_state_lock);
    was_listening = s_dialog_listening;
    s_dialog_listening = false;
    portEXIT_CRITICAL(&s_mic_state_lock);

    julia_idle_display_set_busy(false);
    post_fsm_event(event);
    ESP_LOGI(TAG, "Terminal intent applied: %s%s", intent,
             was_listening ? ", utterance closed" : "");
}

/**
 * @brief 解析服务端经 MQTT 下发的 S4/S2 语义判定结果。
 *
 * 固定格式：{"type":"intent_result","intent":"normal|goodnight|dismiss"}。
 * 返回 true 表示载荷是 JSON 并已完成处理或拒绝；false 表示继续按旧文本命令解析。
 */
static bool voice_service_handle_intent_json(const char *cmd, size_t cmd_len)
{
    if (cmd_len == 0U || cmd[0] != '{') return false;
    cJSON *root = cJSON_ParseWithLength(cmd, cmd_len);
    if (root == NULL || !cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Ignoring malformed intent JSON");
        cJSON_Delete(root);
        return true;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *intent = cJSON_GetObjectItemCaseSensitive(root, "intent");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "intent_result") != 0 ||
        !cJSON_IsString(intent)) {
        ESP_LOGW(TAG, "Ignoring JSON without type=intent_result and string intent");
        cJSON_Delete(root);
        return true;
    }

    if (strcmp(intent->valuestring, "normal") == 0) {
        /* normal 只确认没有特殊语义；正常状态推进继续由现有语音事件负责。 */
        ESP_LOGD(TAG, "Normal intent accepted without FSM transition");
    } else if (strcmp(intent->valuestring, "goodnight") == 0) {
        esp_err_t err = voice_service_enqueue(VOICE_JOB_INTENT_GOODNIGHT, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "goodnight intent enqueue failed: %s", esp_err_to_name(err));
        }
    } else if (strcmp(intent->valuestring, "dismiss") == 0) {
        esp_err_t err = voice_service_enqueue(VOICE_JOB_INTENT_DISMISS, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "dismiss intent enqueue failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Ignoring unknown intent result: %s", intent->valuestring);
    }
    cJSON_Delete(root);
    return true;
}

/**
 * @brief 处理一条完整重组的 MQTT 语音命令（通信层注册表回调）。
 *
 * 原有命令是纯文本行：FILE_SEND <uri>、MIC_START、MIC_STOP；语义结果使用
 * JSON：{"type":"intent_result","intent":"normal|goodnight|dismiss"}。
 *
 * @param[in] cmd     NUL 结尾的命令文本，不允许为 NULL。
 * @param[in] cmd_len 命令有效长度，范围为 1～VOICE_SERVICE_CMD_MAX_LEN。
 */
static void voice_service_on_mqtt_command(const char *cmd, size_t cmd_len)
{
    if (cmd == NULL || cmd_len == 0U || cmd_len > VOICE_SERVICE_CMD_MAX_LEN) {
        ESP_LOGW(TAG, "Ignoring oversized or empty voice command");
        return;
    }
    /* 去掉发布端可能附加的换行与空白；通信层尾部 NUL 保证 cmd 可读。 */
    while (cmd_len > 0U && (cmd[cmd_len - 1] == '\r' || cmd[cmd_len - 1] == '\n' ||
                            cmd[cmd_len - 1] == ' ' || cmd[cmd_len - 1] == '\t')) {
        cmd_len--;
    }
    if (cmd_len == 0U) {
        ESP_LOGW(TAG, "Ignoring blank voice command");
        return;
    }
    ESP_LOGI(TAG, "Voice command: %.*s", (int)cmd_len, cmd);

    if (voice_service_handle_intent_json(cmd, cmd_len)) return;

    if (cmd_len > strlen("FILE_SEND ") && strncmp(cmd, "FILE_SEND ", 10) == 0) {
        /* 复制到独立缓冲区保证 NUL 结尾，供 WSS 会话任务异步使用。 */
        char uri[VOICE_SERVICE_CMD_MAX_LEN];
        size_t uri_len = cmd_len - 10;
        if (uri_len >= sizeof(uri)) {
            ESP_LOGW(TAG, "Voice FILE_SEND uri too long");
            return;
        }
        memcpy(uri, cmd + 10, uri_len);
        uri[uri_len] = '\0';
        esp_err_t err = voice_service_send_file(uri);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Voice FILE_SEND rejected: %s", esp_err_to_name(err));
        }
    } else if (cmd_len == strlen("MIC_START") && memcmp(cmd, "MIC_START", cmd_len) == 0) {
        esp_err_t err = voice_service_mic_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Voice MIC_START rejected: %s", esp_err_to_name(err));
        }
    } else if (cmd_len == strlen("MIC_STOP") && memcmp(cmd, "MIC_STOP", cmd_len) == 0) {
        esp_err_t err = voice_service_mic_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Voice MIC_STOP rejected: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Ignoring unknown voice command: %.*s", (int)cmd_len, cmd);
    }
}

/* ------------------------------------------------------------------ */
/* 公共接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t voice_service_init(void)
{
    julia_fsm_runtime_set_state_observer(voice_service_on_fsm_state, NULL);
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
    if (s_companion_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = voice_service_companion_timeout,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "voice_companion",
            .skip_unhandled_events = true,
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_companion_timer),
                            TAG, "create companion timer");
    }
#endif
    /* 语音 topic 非 critical：语音订阅失败不影响 OTA 连接就绪判定。 */
    return mqtt_comm_register_topic(CONFIG_COMM_MQTT_VOICE_CMD_TOPIC,
                                    VOICE_SERVICE_CMD_MAX_LEN, false,
                                    voice_service_on_mqtt_command);
}

esp_err_t voice_service_init_board_audio(void)
{
    ESP_RETURN_ON_ERROR(voice_playback_init(voice_service_played_pcm, NULL),
                        TAG, "init playback worker");
    /* 板级MIC把PCM1帧路由到WSS。服务器唤醒模式在WSS认证完成时立即打开
     * 此上行；MIC_START只改变对话语义和UI。 */
    ESP_RETURN_ON_ERROR(board_audio_set_wss_sink(voice_service_on_board_audio_frame, NULL),
                        TAG, "set WSS sink");
    ESP_LOGI(TAG, "board audio wired: PCM1 uplink -> WSS, SPKS/SPKE downlink -> speaker");
    return ESP_OK;
}

esp_err_t voice_service_ip_ready(void *arg)
{
    (void)arg;
    static const wss_transport_config_t transport_cfg = {
        .on_text = voice_service_on_server_text,
        .on_binary = voice_service_on_binary,
        .on_queue_item = voice_service_on_queue_item,
#if CONFIG_JULIA_SERVER_WAKE_ENABLE
        .on_session_start = voice_service_on_session_start,
#endif
        .on_session_end = voice_service_on_session_end,
        .on_poll = voice_service_poll,
        .queue_item_size = sizeof(voice_job_t),
        .queue_depth = VOICE_QUEUE_DEPTH,
    };
    return wss_transport_start(&transport_cfg);
}

esp_err_t voice_service_send_file(const char *uri)
{
    if (uri == NULL || uri[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(uri);
    if (len >= VOICE_SERVICE_URI_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* 连同末尾 NUL 一起入队，会话任务侧可直接当作字符串使用。 */
    return voice_service_enqueue(VOICE_JOB_SEND_FILE, (const uint8_t *)uri, len + 1U);
}

esp_err_t voice_service_send_chunk(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > WSS_TRANSPORT_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    return voice_service_enqueue(VOICE_JOB_SEND_CHUNK, buf, len);
}

esp_err_t voice_service_mic_start(void)
{
    return voice_service_enqueue(VOICE_JOB_MIC_START, NULL, 0);
}

esp_err_t voice_service_mic_stop(void)
{
    return voice_service_enqueue(VOICE_JOB_MIC_STOP, NULL, 0);
}
