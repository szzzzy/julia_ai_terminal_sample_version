/**
 * @file    voice_service.c
 * @brief   把服务器命令转换成用户能够感知的听音、回答播放和文件发送行为。
 *
 * MQTT 传递“用户开始/结束说话、结束交流、发送文件”等控制信息，WSS 传递
 * 麦克风声音、服务器回答和文件内容。本模块统一解释这些消息，连接与帧收发则由
 * WSS 连接模块负责。本地文件地址只允许映射到批准的存储目录。
 *
 * 控制消息先进入小型队列，麦克风声音进入独立的大缓冲区；两者不会互相挤占。
 * 负责 WSS 的任务按顺序发送和推进对话，播放任务只负责把回答真正写入扬声器。
 * 因此“已收到回答数据”和“用户已经听到完整回答”是两个不同阶段。
 */

#include "voice_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
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
#include "voice_uplink_pump.h"
#include "voice_uplink_ring.h"
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

static const char *TAG = "voice_service";

static void post_fsm_event(fsm_event_t event)
{
    /* WSS 按序接收的下一条命令只能观察到本条事件已经提交后的状态。 */
    esp_err_t err = julia_fsm_runtime_post_sync(event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FSM event %s rejected: %s", julia_fsm_event_name(event),
                 esp_err_to_name(err));
    }
}

#define VOICE_TRANSPORT_QUEUE_DEPTH 1
#define VOICE_INTERACTION_ID_MAX_LEN 64
#define VOICE_UPLINK_FRAME_CAPACITY 256U
#define VOICE_UPLINK_FRAME_MAX_BYTES 656U
#define VOICE_UPLINK_NORMAL_BATCH 1U
#define VOICE_UPLINK_CATCHUP_BATCH 8U
#define VOICE_UPLINK_CATCHUP_THRESHOLD 2U
#define VOICE_UPLINK_RUN_BUDGET_US 8000LL

/**
 * @brief 一项等待语音连接按顺序处理的请求，例如开始说话、结束说话或发送文件。
 *
 * 连接模块只负责保存和交回这项内容，不解释它的业务含义。
 */
typedef enum {
    VOICE_JOB_SEND_FILE = 0, /**< FILE_SEND：推送一个音频文件。 */
    VOICE_JOB_MIC_START,     /**< MIC_START：确认用户开始一轮说话。 */
    VOICE_JOB_MIC_STOP,      /**< MIC_STOP：确认本轮用户说话结束。 */
    VOICE_JOB_SEND_TEXT,     /**< FSM 已提交状态后的 WSS 文本通知。 */
    VOICE_JOB_INTENT_GOODNIGHT, /**< MQTT 晚安语义，转交 WSS 任务串行收尾。 */
    VOICE_JOB_INTENT_DISMISS,   /**< MQTT 结束沟通语义，转交 WSS 任务串行收尾。 */
} voice_job_type_t;

typedef struct {
    voice_job_type_t type;
    size_t len;
    uint8_t data[WSS_TRANSPORT_MAX_PAYLOAD]; /**< URI 或控制消息内容。 */
} voice_job_t;

/** 分别记录“是否向服务器发送声音”和“是否正在等待用户完成本轮话语”。 */
static portMUX_TYPE s_mic_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_mic_streaming;
static bool s_dialog_listening;
static char s_interaction_id[VOICE_INTERACTION_ID_MAX_LEN];
static julia_main_state_t s_interaction_origin = JULIA_MAIN_STATE_S1_COMPANION;
static bool s_s4_ready_pending;
static bool s_s4_ready_committed;
static bool s_wake_reply_expected;

typedef enum {
    VOICE_PLAYBACK_ROLE_NONE = 0,
    VOICE_PLAYBACK_ROLE_WAKE_REPLY,
    VOICE_PLAYBACK_ROLE_DIALOG_REPLY,
    VOICE_PLAYBACK_ROLE_SELF_TEST,
} voice_playback_role_t;

/* 下列播放、文件和上传进度只由负责语音连接的任务修改，避免跨任务互相覆盖。 */
static uint32_t s_playback_generation;
static voice_playback_role_t s_playback_role;
static FILE *s_file;
static bool s_file_pending;
static bool s_file_active;
static uint64_t s_file_size;
static uint64_t s_file_sent;
static uint32_t s_uplink_dropped;
static voice_uplink_ring_t s_uplink_ring;
static uint8_t *s_uplink_storage;
static uint16_t s_uplink_lengths[VOICE_UPLINK_FRAME_CAPACITY];
static uint32_t s_uplink_generations[VOICE_UPLINK_FRAME_CAPACITY];
static uint32_t s_uplink_generation;
static bool s_uplink_ring_ready;
static voice_uplink_pump_t s_uplink_pump;
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
static bool s_companion_timer_armed;
static esp_timer_handle_t s_companion_timer;
#endif

/** 远程请求允许发送的单个 WAV 文件最大为 8 MiB，防止长时间占用语音连接。 */
#define VOICE_SEND_MAX_FILE_BYTES (8 * 1024 * 1024)

static esp_err_t voice_service_enqueue(voice_job_type_t type,
                                       const uint8_t *data, size_t len);
static void voice_service_apply_terminal_intent(fsm_event_t event,
                                                const char *intent);
static bool voice_service_send_uplink_frame(void *ctx, const uint8_t *data,
                                            size_t len);
static int64_t voice_service_uplink_now_us(void *ctx);

static bool voice_service_mic_is_streaming(void)
{
    bool streaming;
    portENTER_CRITICAL(&s_mic_state_lock);
    streaming = s_mic_streaming;
    portEXIT_CRITICAL(&s_mic_state_lock);
    return streaming;
}

static esp_err_t voice_service_uplink_ring_init(void)
{
    if (s_uplink_ring_ready) return ESP_OK;
    const size_t storage_bytes =
        VOICE_UPLINK_FRAME_CAPACITY * VOICE_UPLINK_FRAME_MAX_BYTES;
    s_uplink_storage = heap_caps_malloc(
        storage_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_uplink_storage == NULL) return ESP_ERR_NO_MEM;
    if (!voice_uplink_ring_init(&s_uplink_ring, s_uplink_storage,
                                s_uplink_lengths, s_uplink_generations,
                                VOICE_UPLINK_FRAME_CAPACITY,
                                VOICE_UPLINK_FRAME_MAX_BYTES)) {
        heap_caps_free(s_uplink_storage);
        s_uplink_storage = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    const voice_uplink_pump_ops_t pump_ops = {
        .ctx = NULL,
        .send = voice_service_send_uplink_frame,
        .now_us = voice_service_uplink_now_us,
    };
    const voice_uplink_pump_config_t pump_config = {
        .normal_batch = VOICE_UPLINK_NORMAL_BATCH,
        .catchup_batch = VOICE_UPLINK_CATCHUP_BATCH,
        .catchup_threshold_frames = VOICE_UPLINK_CATCHUP_THRESHOLD,
        .run_budget_us = VOICE_UPLINK_RUN_BUDGET_US,
    };
    if (!voice_uplink_pump_init(&s_uplink_pump, &s_uplink_ring,
                                &pump_ops, &pump_config)) {
        heap_caps_free(s_uplink_storage);
        s_uplink_storage = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_uplink_ring_ready = true;
    ESP_LOGI(TAG, "MIC uplink ring ready: %u frames, %u bytes in PSRAM",
             (unsigned)VOICE_UPLINK_FRAME_CAPACITY, (unsigned)storage_bytes);
    return ESP_OK;
}

static uint32_t voice_service_next_uplink_generation(void)
{
    s_uplink_generation++;
    if (s_uplink_generation == 0U) {
        s_uplink_generation++;
    }
    return s_uplink_generation;
}

/** 发送文件期间暂停麦克风并丢弃已积压声音，避免文件和声音无法区分。 */
static void voice_service_pause_uplink_for_file(void)
{
    voice_uplink_pump_stop(&s_uplink_pump);
    if (s_uplink_ring_ready) voice_uplink_ring_stop_generation(&s_uplink_ring);
    portENTER_CRITICAL(&s_mic_state_lock);
    board_audio_enable_wss_mic(false);
    portEXIT_CRITICAL(&s_mic_state_lock);
}

/** 文件完整结束后，从空缓冲重新开始发送实时麦克风声音。 */
static void voice_service_resume_uplink_after_file(void)
{
    if (!s_uplink_ring_ready) {
        return;
    }
    uint32_t generation = voice_service_next_uplink_generation();
    if (!voice_uplink_ring_start_generation(&s_uplink_ring, generation)) return;
    voice_uplink_pump_start_generation(&s_uplink_pump, generation);
    portENTER_CRITICAL(&s_mic_state_lock);
    if (s_mic_streaming) board_audio_enable_wss_mic(true);
    portEXIT_CRITICAL(&s_mic_state_lock);
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
 * @brief 接收板级麦克风生成的一块完整声音，并加入语音服务器待发送缓冲区。
 * WSS 未启动或文件区间暂停时不接收；任何情况都不阻塞 mic_task。
 */
static void voice_service_on_board_audio_frame(const uint8_t *frame, size_t bytes, void *ctx)
{
    (void)ctx;
    esp_err_t err = voice_service_send_chunk(frame, bytes);
    if (err == ESP_ERR_NO_MEM) {
        /* 只有缓冲确实装满才记录容量故障；发送文件或连接切换造成的主动暂停不算丢帧。 */
        if ((++s_uplink_dropped & 255U) == 1U) {
            ESP_LOGW(TAG, "MIC uplink ring full drops=%lu",
                     (unsigned long)s_uplink_dropped);
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
static esp_err_t voice_service_push_file(const char *uri, bool queued)
{
    portENTER_CRITICAL(&s_mic_state_lock);
    bool listening = s_dialog_listening;
    bool reserved = s_file_pending && !queued;
    portEXIT_CRITICAL(&s_mic_state_lock);
    if (reserved || s_file != NULL || voice_playback_is_active() || listening) {
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
    portENTER_CRITICAL(&s_mic_state_lock);
    s_file_active = true;
    portEXIT_CRITICAL(&s_mic_state_lock);
    s_file_size = (uint64_t)size;
    s_file_sent = 0;
    voice_service_pause_uplink_for_file();
    return ESP_OK;
}

static void voice_service_close_file(void)
{
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
        portENTER_CRITICAL(&s_mic_state_lock);
        s_file_active = false;
        portEXIT_CRITICAL(&s_mic_state_lock);
        julia_wireless_sd_unlock();
    }
}

static void voice_service_cancel_file(void)
{
    if (s_file != NULL) {
        voice_service_close_file();
        (void)voice_service_send_error("ERROR file_cancelled");
        voice_service_resume_uplink_after_file();
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
    if (wss_transport_send_now(0x1, (const uint8_t *)end, (size_t)n) == ESP_OK) {
        voice_service_resume_uplink_after_file();
    }
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

static bool voice_service_send_uplink_frame(void *ctx, const uint8_t *data,
                                            size_t len)
{
    (void)ctx;
    if (wss_transport_send_now(0x2, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send %u-byte MIC ring frame", (unsigned)len);
        return false;
    }
    return true;
}

static int64_t voice_service_uplink_now_us(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}

/** 正常每轮发送一块麦克风声音；积压时有限追赶，同时给控制和接收留时间。 */
static void voice_service_uplink_poll(void)
{
    if (!s_uplink_ring_ready || s_uplink_generation == 0U ||
        !voice_service_mic_is_streaming()) {
        return;
    }
    voice_uplink_pump_result_t result = voice_uplink_pump_run(&s_uplink_pump);
    if (result.status == VOICE_UPLINK_PUMP_RING_ERROR) {
        ESP_LOGE(TAG, "MIC uplink pump lost ring ownership");
        wss_transport_fail_session();
        return;
    }
    if (result.catchup_completed &&
        (result.catchup_peak_frames >= 5U ||
         result.catchup_drain_us >= 50000LL)) {
        ESP_LOGI(TAG,
                 "MIC uplink backlog recovered generation=%" PRIu32
                 " peak_frames=%u drain_ms=%" PRIi64,
                 s_uplink_generation,
                 (unsigned)result.catchup_peak_frames,
                 result.catchup_drain_us / 1000LL);
    }
}

static void voice_service_state_ready_poll(void)
{
    char id[VOICE_INTERACTION_ID_MAX_LEN];
    portENTER_CRITICAL(&s_mic_state_lock);
    bool ready = s_s4_ready_pending && s_s4_ready_committed;
    memcpy(id, s_interaction_id, sizeof(id));
    portEXIT_CRITICAL(&s_mic_state_lock);
    if (!ready || id[0] == '\0') return;
    if (julia_fsm_runtime_get_state() != JULIA_MAIN_STATE_S4_INTERACTION) {
        portENTER_CRITICAL(&s_mic_state_lock);
        s_s4_ready_pending = false;
        s_s4_ready_committed = false;
        portEXIT_CRITICAL(&s_mic_state_lock);
        return;
    }
    char payload[144];
    int length = snprintf(payload, sizeof(payload),
        "{\"type\":\"state_ready\",\"interaction_id\":\"%s\",\"state\":\"S4\"}", id);
    if (length > 0 && (size_t)length < sizeof(payload) &&
        wss_transport_send_now(0x1, (const uint8_t *)payload, (size_t)length) == ESP_OK) {
        portENTER_CRITICAL(&s_mic_state_lock);
        s_s4_ready_pending = false;
        s_s4_ready_committed = false;
        portEXIT_CRITICAL(&s_mic_state_lock);
    }
}

static void voice_service_poll(void)
{
    /* 关键确认由 WSS owner 直接发送，不与可丢弃的四槽控制作业竞争。 */
    voice_service_state_ready_poll();
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
    if (s_file != NULL) {
        voice_service_file_poll();
        return;
    }
    voice_service_uplink_poll();
}

static void voice_service_played_pcm(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    julia_avatar_feed_pcm(pcm, samples);
}

/** “开始说话”确认用户已经进入本轮表达；必要时同时开始上传麦克风。 */
static void voice_service_apply_mic_start(void)
{
    julia_main_state_t state = julia_fsm_runtime_get_state();
    if (state == JULIA_MAIN_STATE_S0_BOOT || state == JULIA_MAIN_STATE_S7_FAULT ||
        state == JULIA_MAIN_STATE_S8_OTA) {
        ESP_LOGW(TAG, "MIC_START ignored in non-interactive state=%s", julia_fsm_main_state_name(state));
        return;
    }
    julia_s2_sub_state_t s2_sub_state = julia_fsm_runtime_get_s2_sub_state();
    fsm_event_t event = EVT_NONE;
    if (state == JULIA_MAIN_STATE_S3_STANDBY || state == JULIA_MAIN_STATE_S5_SILENT ||
        state == JULIA_MAIN_STATE_S6_SLEEP) event = EVT_WAKEUP;
    else if (state == JULIA_MAIN_STATE_S1_COMPANION) event = EVT_USER_CALL;
    else if (state == JULIA_MAIN_STATE_S2_DIALOG &&
             s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING) event = EVT_INTERRUPT;
    bool interrupted_speaker = voice_playback_is_active();
    /* 打断保留原有立即停播语义，不把面板呈现的等待增加到扬声器停止延迟。 */
    if (interrupted_speaker) voice_playback_stop();
    /* 准入失败不能留下 busy/采音副作用；计时或 OTA 可能已使上述快照过期。 */
    if (event != EVT_NONE && julia_fsm_runtime_post_sync(event) != ESP_OK) return;
    voice_service_disarm_companion_timer();
    julia_idle_display_note_activity();
    voice_service_cancel_file();
    voice_playback_stop();
    s_playback_generation = 0;
    s_playback_role = VOICE_PLAYBACK_ROLE_NONE;
    julia_avatar_talking_stop();

    bool started_streaming = false;
    portENTER_CRITICAL(&s_mic_state_lock);
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
        /* 兼容没有单独发送唤醒确认的旧服务器和本地唤醒：收到“开始说话”时，
         * 仍先建立一轮交流。正在播放的旧声音不会被误判成用户插话。 */
        portENTER_CRITICAL(&s_mic_state_lock);
        s_interaction_origin = state;
        s_wake_reply_expected = true;
        portEXIT_CRITICAL(&s_mic_state_lock);
    } else if (state == JULIA_MAIN_STATE_S4_INTERACTION) {
        /* 已经完成唤醒时，本命令只表示用户现在开始说话；如果用户打断唤醒回应，
         * 先停止回应，再等待用户说完和服务器返回正式回答。 */
        if (interrupted_speaker) s_wake_reply_expected = false;
    }
    ESP_LOGI(TAG, "MIC_START: state=%s/%s listening%s%s",
             julia_fsm_main_state_name(state),
             julia_fsm_s2_sub_state_name(s2_sub_state),
             started_streaming ? ", PCM upload started" : ", PCM upload already active",
             interrupted_speaker ? ", speaker interrupted" : "");
}

/** “结束说话”只结束本轮听音；免唤醒陪伴期间仍可继续上传环境声音。 */
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

    /* 用户说完后仍属于一次未完成的交流，空闲计时不能把设备送回待机；
     * 连接和麦克风继续工作，直到回答真正播放完成。 */
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

/** 识别服务器的唤醒确认消息；识别成功后不再把它当作普通文本命令。 */
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
    s_s4_ready_committed = false;
    s_wake_reply_expected = true;
    s_dialog_listening = false;
    portEXIT_CRITICAL(&s_mic_state_lock);

    julia_idle_display_note_activity();
    julia_idle_display_set_busy(true);
    post_fsm_event(EVT_WAKEUP);
    ESP_LOGI(TAG, "wake_detected id=%s origin=%s; state transition processed",
             id->valuestring, julia_fsm_main_state_name(state));
    cJSON_Delete(root);
    return true;
}

/**
 * @brief 处理语音服务器发来的一条完整文本命令。
 *
 * 文件请求开始分块发送；开始/结束说话与 MQTT 使用相同的处理规则。
 * 文本控制使用独立队列，因此不会被大量麦克风数据挤掉。
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
        (void)voice_service_push_file(uri, false);
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

    /* 回答声音必须先用 SPKS 声明采样率和用途，再发送二进制声音，最后用 SPKE
     * 表示服务器已经发完。顺序错误的声音会被拒绝，避免未知数据进入扬声器。 */
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
 * @brief 把服务器回答声音交给独立播放任务，本函数不直接操作扬声器。
 *
 * 服务器必须先声明开始播放；未声明的二进制数据全部丢弃。声音采用 16 位单声道，
 * 因而数据不能为空且字节数必须为偶数，否则样本会错位。
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
 * 控制请求在负责语音连接的任务中执行；麦克风声音使用独立缓冲区，不占控制容量。
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
        (void)voice_service_push_file((const char *)job->data, true);
        portENTER_CRITICAL(&s_mic_state_lock);
        s_file_pending = false;
        portEXIT_CRITICAL(&s_mic_state_lock);
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
    default:
        ESP_LOGW(TAG, "Unknown queued voice job type %d", (int)job->type);
        break;
    }
}

/** 新语音连接从空的麦克风缓冲开始，断线前未发出的声音绝不在重连后补发。 */
static void voice_service_on_session_start(void)
{
    uint32_t generation = voice_service_next_uplink_generation();
    if (!s_uplink_ring_ready ||
        !voice_uplink_ring_start_generation(&s_uplink_ring, generation)) {
        ESP_LOGE(TAG, "Cannot start MIC uplink generation=%" PRIu32, generation);
        wss_transport_fail_session();
        return;
    }
    voice_uplink_pump_start_generation(&s_uplink_pump, generation);
#if CONFIG_JULIA_SERVER_WAKE_ENABLE
    portENTER_CRITICAL(&s_mic_state_lock);
    s_mic_streaming = true;
    s_dialog_listening = false;
    s_s4_ready_pending = false;
    s_s4_ready_committed = false;
    s_interaction_id[0] = '\0';
    board_audio_mic_wake();
    board_audio_enable_wss_mic(true);
    portEXIT_CRITICAL(&s_mic_state_lock);

    /* 后台唤醒监听只属于传输层；服务端发送 wake_detected 前，
     * 保持当前主状态与呈现不变。 */
    ESP_LOGI(TAG, "WSS session ready: generation=%" PRIu32
                  " IDLE PCM upload enabled for server wake detection",
             generation);
#else
    ESP_LOGI(TAG, "WSS session ready: generation=%" PRIu32, generation);
#endif
    post_fsm_event(EVT_WSS_CONNECTED);
}

/**
 * 一条 WSS generation 同时拥有 MIC ring、播放角色和文件区间。结束通知必须先清空
 * 这些会话资源再投递 FSM 事件；否则重连可能把旧话语或旧完成结果带入新会话。
 */
static void voice_service_on_session_end(wss_transport_end_reason_t reason)
{
    voice_service_disarm_companion_timer();
    size_t discarded_frames = 0;
    if (s_uplink_ring_ready) {
        discarded_frames = voice_uplink_ring_count(&s_uplink_ring);
        voice_uplink_pump_stop(&s_uplink_pump);
        voice_uplink_ring_stop_generation(&s_uplink_ring);
    }
    portENTER_CRITICAL(&s_mic_state_lock);
    s_mic_streaming = false;
    s_dialog_listening = false;
    s_s4_ready_pending = false;
    s_s4_ready_committed = false;
    s_file_pending = false;
    s_wake_reply_expected = false;
    s_interaction_id[0] = '\0';
    board_audio_enable_wss_mic(false);
    portEXIT_CRITICAL(&s_mic_state_lock);
    voice_service_close_file();
    voice_playback_stop();
    s_playback_generation = 0;
    s_playback_role = VOICE_PLAYBACK_ROLE_NONE;
    julia_avatar_talking_stop();
    ESP_LOGI(TAG, "WSS uplink generation=%" PRIu32
                  " ended reason=%s; discarded MIC ring frames=%u",
             s_uplink_generation, wss_transport_end_reason_name(reason),
             (unsigned)discarded_frames);
    post_fsm_event(EVT_WSS_DISCONNECTED);
    /* 断线不是用户主动交流，只结束“正在处理”的标记，不能因此点亮睡眠中的屏幕。 */
    julia_idle_display_set_busy(false);
}

/* data 在返回前完成复制，len 为 0 时允许为 NULL。成功只表示控制队列已接收，
 * 不表示 WSS 已发送或业务状态已经生效。 */
static esp_err_t voice_service_enqueue(voice_job_type_t type, const uint8_t *data, size_t len)
{
    if (len > WSS_TRANSPORT_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    voice_job_t job;
    if (type == VOICE_JOB_SEND_FILE) {
        portENTER_CRITICAL(&s_mic_state_lock);
        bool busy = s_file_pending || s_file_active;
        if (!busy) s_file_pending = true;
        portEXIT_CRITICAL(&s_mic_state_lock);
        if (busy) return ESP_ERR_NO_MEM;
    }
    memset(&job, 0, sizeof(job));
    job.type = type;
    job.len = len;
    if (data != NULL && len > 0U) {
        memcpy(job.data, data, len);
    }
    esp_err_t err = wss_transport_enqueue_control(&job, sizeof(job));
    if (err != ESP_OK && type == VOICE_JOB_SEND_FILE) {
        portENTER_CRITICAL(&s_mic_state_lock);
        s_file_pending = false;
        portEXIT_CRITICAL(&s_mic_state_lock);
    }
    return err;
}

/** 设备真正进入“已唤醒”状态后才回执，防止服务器过早发送唤醒回应。 */
static void voice_service_on_fsm_state(julia_main_state_t main_state,
                                       julia_s2_sub_state_t s2_sub_state,
                                       fsm_event_t event, void *ctx)
{
    (void)s2_sub_state;
    (void)ctx;
    if (main_state != JULIA_MAIN_STATE_S4_INTERACTION || event != EVT_WAKEUP) return;

    portENTER_CRITICAL(&s_mic_state_lock);
    s_s4_ready_committed = s_s4_ready_pending;
    portEXIT_CRITICAL(&s_mic_state_lock);

}

/**
 * @brief 用户说出“晚安”或“结束交流”时，立即结束当前话语和未播完的回答。
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

    /* 设备已经决定结束交流后，旧的唤醒回应或正常回答都不能继续出声。
     * 所有清理在负责语音连接的同一任务中完成，保证停止顺序一致。 */
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
    ESP_RETURN_ON_ERROR(voice_service_uplink_ring_init(), TAG,
                        "init MIC uplink ring");
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
        .on_session_start = voice_service_on_session_start,
        .on_session_end = voice_service_on_session_end,
        .on_poll = voice_service_poll,
        .queue_item_size = sizeof(voice_job_t),
        /* 麦克风声音走独立缓冲；普通队列只保留最小容量以满足连接层通用接口。 */
        .queue_depth = VOICE_TRANSPORT_QUEUE_DEPTH,
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
    if (len > VOICE_UPLINK_FRAME_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_uplink_ring_ready) return ESP_ERR_INVALID_STATE;
    voice_uplink_push_result_t result = voice_uplink_ring_push(
        &s_uplink_ring, buf, len);
    switch (result) {
    case VOICE_UPLINK_PUSH_OK: return ESP_OK;
    case VOICE_UPLINK_PUSH_FULL: {
        /* 采集任务只报告缓冲已满；负责语音连接的任务统一停止上传并关闭连接，
         * 避免两个任务同时清理同一批声音和加密连接。 */
        voice_uplink_ring_close_generation(&s_uplink_ring);
        (void)wss_transport_request_session_end(
            WSS_TRANSPORT_END_AUDIO_OVERFLOW);
        return ESP_ERR_NO_MEM;
    }
    case VOICE_UPLINK_PUSH_INACTIVE: return ESP_ERR_INVALID_STATE;
    case VOICE_UPLINK_PUSH_INVALID:
    default: return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t voice_service_mic_start(void)
{
    return voice_service_enqueue(VOICE_JOB_MIC_START, NULL, 0);
}

esp_err_t voice_service_mic_stop(void)
{
    return voice_service_enqueue(VOICE_JOB_MIC_STOP, NULL, 0);
}

bool voice_service_file_busy(void)
{
    portENTER_CRITICAL(&s_mic_state_lock);
    bool busy = s_file_pending || s_file_active;
    portEXIT_CRITICAL(&s_mic_state_lock);
    return busy;
}
