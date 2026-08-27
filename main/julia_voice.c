#include "julia_voice.h"

#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_wn_iface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "julia_ai_client.h"
#include "julia_audio.h"
#include "wake_word_config.h"
#include "wake_reply.h"
#include "julia_fsm.h"
#include "julia_home.h"
#include "julia_network.h"
#include "julia_memory.h"
#include "julia_routine.h"
#include "julia_speech_cloud.h"
#include "julia_local_tts.h"
#include "julia_lipsync.h"
#include "julia_system.h"
#include "julia_ui.h"
#include "avatar_micro_action.h"
#include "model_path.h"
#include "sdkconfig.h"

#define TAG "JULIA_VOICE"
#define MAX_RECORD_SAMPLES (JULIA_AUDIO_SAMPLE_RATE * 6)
#define NO_SPEECH_TIMEOUT_MS 5000
#define END_SILENCE_MS 750
#define MIN_CAPTURE_MS 1200
#define SPEECH_PEAK_THRESHOLD 650
#define SPEECH_RMS_THRESHOLD  260
#define SPEECH_CONFIRM_FRAMES  3
#define NETWORK_PROMPT_PATH "/sdcard/julia/netmsg.pcm"
#define FOLLOWUP_WINDOW_MS 0
#define FOLLOWUP_COOLDOWN_MS 700
#define VOICE_SESSION_MAX_MS 60000
#define MAX_DIALOG_ROUNDS 5

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static srmodel_list_t *s_models;
static julia_fsm_t s_fsm;
static volatile bool s_session_busy;
static volatile bool s_interrupt_requested;
static int16_t *s_recording;
static size_t s_recorded_samples;
static esp_pm_lock_handle_t s_cpu_lock;
static SemaphoreHandle_t s_fsm_lock;
static volatile int64_t s_last_audio_activity_ms;
static volatile int64_t s_followup_until_ms;
static volatile uint8_t s_dialog_rounds;
static volatile int64_t s_followup_ready_ms;
static volatile int64_t s_wake_resume_ms;

typedef enum {
    VOICE_FAILURE_NONE = 0,
    VOICE_FAILURE_ASR = 1,
    VOICE_FAILURE_AI = 2,
    VOICE_FAILURE_TTS = 3,
    VOICE_FAILURE_PLAYBACK = 4,
} voice_failure_t;

static void play_failure_code(voice_failure_t failure)
{
    ESP_LOGW(TAG, "offline voice feedback: failure=%d", failure);
    julia_audio_play_tone(220, 70, 18);
}

static bool handle_local_command(const char *text)
{
    if (!text) return false;
    if (strstr(text, "status") || strstr(text, "状态")) {
        ESP_LOGI(TAG, "Local command: status");
        if (julia_local_tts_speak("系统运行正常") != ESP_OK) {
            julia_audio_play_tone(660, 90, 45); julia_audio_play_tone(880, 90, 45);
        }
        return true;
    }
    if (strstr(text, "time") || strstr(text, "时间")) {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        ESP_LOGI(TAG, "Local command: time %02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        if (julia_local_tts_speak("时间已同步") != ESP_OK) julia_audio_play_tone(880, 120, 45);
        return true;
    }
    if (strstr(text, "standby") || strstr(text, "待机")) {
        ESP_LOGI(TAG, "Local command: standby");
        if (julia_local_tts_speak("进入待机模式") != ESP_OK) julia_audio_play_tone(440, 180, 40);
        return true;
    }
    return false;
}

static const char *detect_emotion(const char *text, uint8_t *label)
{
    static const char *sad[] = {"难过", "伤心", "不开心", "想哭", "孤独", "焦虑", "压力"};
    static const char *tired[] = {"累了", "好累", "疲惫", "困了", "没精神"};
    static const char *happy[] = {"开心", "高兴", "太好了", "喜欢", "兴奋"};
    for (size_t i = 0; i < sizeof(sad) / sizeof(sad[0]); ++i)
        if (strstr(text, sad[i])) { *label = JULIA_MEMORY_EMOTION_SAD; return "用户情绪低落，请先共情，再简短回应。"; }
    for (size_t i = 0; i < sizeof(tired) / sizeof(tired[0]); ++i)
        if (strstr(text, tired[i])) { *label = JULIA_MEMORY_EMOTION_TIRED; return "用户感到疲惫，请温和关心并避免冗长回答。"; }
    for (size_t i = 0; i < sizeof(happy) / sizeof(happy[0]); ++i)
        if (strstr(text, happy[i])) { *label = JULIA_MEMORY_EMOTION_HAPPY; return "用户情绪积极，请自然分享这份愉快。"; }
    return NULL;
}

bool julia_voice_handle_event(fsm_event_t event)
{
    if (!s_fsm_lock || xSemaphoreTake(s_fsm_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    bool changed = julia_fsm_handle_event(&s_fsm, event, NULL);
    xSemaphoreGive(s_fsm_lock);
    return changed;
}

julia_sub_state_t julia_voice_get_state(void)
{
    if (!s_fsm_lock || xSemaphoreTake(s_fsm_lock, pdMS_TO_TICKS(100)) != pdTRUE) return s_fsm.sub_state;
    julia_sub_state_t state = s_fsm.sub_state;
    xSemaphoreGive(s_fsm_lock);
    return state;
}

bool julia_voice_is_busy(void) { return s_session_busy; }

void julia_voice_interrupt(void)
{
    s_interrupt_requested = true;
    ESP_LOGI(TAG, "Voice interruption requested");
}
int64_t julia_voice_last_audio_activity_ms(void) { return s_last_audio_activity_ms; }

esp_err_t julia_voice_inject_event(julia_voice_injected_event_t event, const char *text)
{
    switch (event) {
    case JULIA_VOICE_INJECT_WAKE:
        julia_routine_on_activity(JULIA_ACTIVITY_WAKE);
        julia_voice_handle_event(EVT_USER_CALL);
        julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_LISTENING);
        break;
    case JULIA_VOICE_INJECT_ASR_DONE:
        ESP_LOGI(TAG, "Injected ASR: %s", text ? text : "");
        julia_voice_handle_event(EVT_START_DIALOG);
        julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_THINKING);
        break;
    case JULIA_VOICE_INJECT_LLM_RESPONSE:
        ESP_LOGI(TAG, "Injected LLM: %s", text ? text : "");
        julia_voice_handle_event(EVT_MULTI_TURN_DETECTED);
        break;
    case JULIA_VOICE_INJECT_TTS_READY:
        julia_voice_handle_event(EVT_MULTI_TURN_DETECTED);
        julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_SPEAKING);
        break;
    case JULIA_VOICE_INJECT_TTS_DONE:
        julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_IDLE);
        julia_voice_handle_event(EVT_SILENCE_TIMEOUT);
        if (julia_voice_get_state() != JULIA_SUB_STATE_S1_1_NEAR_STANDBY)
            julia_voice_handle_event(EVT_WAKEUP);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void voice_on_enter(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t event)
{
    (void)fsm; (void)event;
    julia_ui_set_state(state);
    if (state == JULIA_SUB_STATE_S3_3_USER_CALL) julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_LISTENING);
    else if (state == JULIA_SUB_STATE_S1_1_NEAR_STANDBY) julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_IDLE);
}

static void return_to_standby(void)
{
    julia_voice_handle_event(EVT_SILENCE_TIMEOUT);
    if (s_fsm.sub_state != JULIA_SUB_STATE_S1_1_NEAR_STANDBY)
        julia_voice_handle_event(EVT_WAKEUP);
}

static size_t sentence_segment_bytes(const char *text, bool final)
{
    size_t length = strlen(text);
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '.' || text[i] == '!' || text[i] == '?' || text[i] == ';')
            return i + 1;
        if (i + 2 < length && (memcmp(text + i, "。", 3) == 0 ||
                               memcmp(text + i, "！", 3) == 0 ||
                               memcmp(text + i, "？", 3) == 0 ||
                               memcmp(text + i, "；", 3) == 0))
            return i + 3;
    }
    /* Long first sentences may stream for several seconds before a full stop.
     * A comma after roughly 24 Chinese characters is a natural TTS boundary. */
    if (length >= 72) {
        for (size_t i = 0; i < length; ++i) {
            if (text[i] == ',') return i + 1;
            if (i + 2 < length && memcmp(text + i, "，", 3) == 0) return i + 3;
        }
    }
    if (length >= 900) {
        size_t cut = 900;
        while (cut > 0 && ((uint8_t)text[cut] & 0xc0U) == 0x80U) --cut;
        return cut;
    }
    return final ? length : 0;
}

typedef struct {
    int16_t *pcm;
    size_t samples;
    bool end;
} stream_pcm_item_t;

typedef struct {
    QueueHandle_t queue;
    SemaphoreHandle_t done;
    esp_err_t result;
    bool played_any;
    int64_t session_started_ms;
} stream_playback_t;

static void stream_playback_task(void *arg)
{
    stream_playback_t *stream = arg;
    bool started = false;
    stream->result = ESP_OK;
    while (true) {
        stream_pcm_item_t item = {0};
        if (xQueueReceive(stream->queue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (item.end) break;
        if (stream->result == ESP_OK && item.pcm && item.samples) {
            if (!started) {
                julia_voice_handle_event(EVT_MULTI_TURN_DETECTED);
                s_afe->enable_wakenet(s_afe_data);
                s_interrupt_requested = false;
                julia_lipsync_begin();
                started = true;
                stream->played_any = true;
                ESP_LOGI(TAG, "First audio latency=%lldms",
                         esp_timer_get_time() / 1000 - stream->session_started_ms);
            }
            size_t remaining = item.samples * sizeof(int16_t);
            uint8_t *cursor = (uint8_t *)item.pcm;
            while (remaining && !s_interrupt_requested) {
                size_t chunk = remaining > 4096 ? 4096 : remaining;
                stream->result = julia_lipsync_play((const int16_t *)cursor,
                                                    chunk / sizeof(int16_t));
                if (stream->result != ESP_OK) break;
                cursor += chunk;
                remaining -= chunk;
            }
            if (s_interrupt_requested) stream->result = ESP_ERR_INVALID_STATE;
        }
        free(item.pcm);
    }
    if (started) {
        s_afe->disable_wakenet(s_afe_data);
        esp_err_t stop_err = julia_lipsync_end();
        if (stream->result == ESP_OK) stream->result = stop_err;
    }
    xSemaphoreGive(stream->done);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t synthesize_and_queue_segment(const char *text, stream_playback_t *stream,
                                               bool *task_started)
{
    int16_t *pcm = NULL;
    size_t samples = 0;
    ESP_LOGI(TAG, "Streaming TTS segment: %u bytes", (unsigned)strlen(text));
    esp_err_t err = julia_speech_tts(text, &pcm, &samples);
    if (err != ESP_OK) { free(pcm); return err; }
    if (!*task_started) {
        if (xTaskCreateWithCaps(stream_playback_task, "voice_stream_play", 8192, stream, 6,
                                NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            free(pcm);
            return ESP_ERR_NO_MEM;
        }
        *task_started = true;
    }
    stream_pcm_item_t item = {.pcm = pcm, .samples = samples};
    if (xQueueSend(stream->queue, &item, pdMS_TO_TICKS(15000)) != pdTRUE) {
        free(pcm);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t speak_local_chunked(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    const uint8_t *cursor = (const uint8_t *)text;
    size_t remaining = strlen(text);
    while (remaining) {
        size_t bytes = remaining > 700 ? 700 : remaining;
        /* 不在 UTF-8 多字节字符中间截断。 */
        while (bytes && bytes < remaining && (cursor[bytes] & 0xc0) == 0x80) --bytes;
        if (!bytes) return ESP_ERR_INVALID_ARG;
        char segment[704];
        memcpy(segment, cursor, bytes);
        segment[bytes] = '\0';
        esp_err_t err = julia_local_tts_speak(segment);
        if (err != ESP_OK) return err;
        cursor += bytes;
        remaining -= bytes;
    }
    return ESP_OK;
}

static void session_task(void *arg)
{
    (void)arg;
    int64_t session_started_ms = esp_timer_get_time() / 1000;
#define SESSION_TIMED_OUT() ((esp_timer_get_time() / 1000 - session_started_ms) > VOICE_SESSION_MAX_MS)
    char user_text[1024] = {0};
    char answer[3072] = {0};
    int16_t *speech = s_recording;
    size_t samples = s_recorded_samples;
    bool response_played = false;
    voice_failure_t failure = VOICE_FAILURE_NONE;
    s_recording = NULL; s_recorded_samples = 0;

    ESP_LOGI(TAG, "ASR upload: %.2f seconds", (double)samples / JULIA_AUDIO_SAMPLE_RATE);
    // Move the avatar from listening to understanding while the request is decoded.
    julia_voice_handle_event(EVT_START_DIALOG);
    julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_THINKING);
    esp_err_t err = julia_speech_asr(speech, samples, user_text, sizeof(user_text));
    ESP_LOGI(TAG, "ASR elapsed=%lldms", esp_timer_get_time() / 1000 - session_started_ms);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "ASR first attempt failed: %s; retrying", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
        err = julia_speech_asr(speech, samples, user_text, sizeof(user_text));
    }
    free(speech);
    if (SESSION_TIMED_OUT()) { failure = VOICE_FAILURE_ASR; goto done; }
    if (err != ESP_OK) {
        failure = VOICE_FAILURE_ASR;
        ESP_LOGE(TAG, "ASR failed after retry: %s", esp_err_to_name(err));
        goto done;
    }
    ESP_LOGI(TAG, "User: %s", user_text);
    if (handle_local_command(user_text)) {
        response_played = true;
        goto done;
    }
    uint8_t memory_emotion = JULIA_MEMORY_EMOTION_NONE;
    const char *emotion_prompt = detect_emotion(user_text, &memory_emotion);
    bool streamed_reply = false;
    bool turn_recorded = false;
    if (emotion_prompt) {
        ESP_LOGI(TAG, "Emotion cue detected");
        julia_memory_append(1, memory_emotion, user_text);
        julia_voice_handle_event(EVT_EMOTION_DETECTED);
    }
    julia_voice_handle_event(EVT_START_DIALOG);
    if (!julia_memory_handle_forget(user_text, answer, sizeof(answer))) {
        if (!julia_wifi_is_connected()) {
            failure = VOICE_FAILURE_AI; ESP_LOGW(TAG, "WiFi unavailable"); goto done;
        }
        char system_prompt[4096];
#if 0
        const char *base_prompt =
            "你是 Julia，一个温柔、简洁的中文陪伴助手。回答适合语音朗读，"
            "通常不超过两句话。不要使用 Markdown。";
 #endif
        const char *base_prompt = "你是 Julia，一个温柔、简洁的中文陪伴助手。回答适合语音朗读，通常不超过两句话，不要使用 Markdown。";
        char configured_prompt[1024] = {0};
        if (julia_system_config_get("system_prompt", configured_prompt,
                                    sizeof(configured_prompt)) == ESP_OK &&
            configured_prompt[0]) {
            base_prompt = configured_prompt;
        }
        if (julia_memory_build_prompt(base_prompt, system_prompt, sizeof(system_prompt)) != ESP_OK)
            strlcpy(system_prompt, base_prompt, sizeof(system_prompt));
        if (emotion_prompt) {
            strlcat(system_prompt, "\n", sizeof(system_prompt));
            strlcat(system_prompt, emotion_prompt, sizeof(system_prompt));
        }
        err = julia_ai_send_message(user_text, system_prompt);
        if (err != ESP_OK) {
            failure = VOICE_FAILURE_AI;
            ESP_LOGE(TAG, "AI request failed: %s", esp_err_to_name(err)); goto done;
        }
        char pending[1024] = {0};
        stream_playback_t stream = {
            .queue = xQueueCreate(2, sizeof(stream_pcm_item_t)),
            .done = xSemaphoreCreateBinary(),
            .session_started_ms = session_started_ms,
        };
        bool playback_task_started = false;
        esp_err_t playback_err = ESP_OK;
        if (!stream.queue || !stream.done) {
            if (stream.queue) vQueueDelete(stream.queue);
            if (stream.done) vSemaphoreDelete(stream.done);
            failure = VOICE_FAILURE_TTS;
            err = ESP_ERR_NO_MEM;
            goto done;
        }
        while (strlen(answer) < sizeof(answer) - 1) {
            char chunk[256];
            err = julia_ai_receive_chunk(chunk, sizeof(chunk));
            if (err != ESP_OK) break;
            strlcat(answer, chunk, sizeof(answer));
            strlcat(pending, chunk, sizeof(pending));
            size_t segment_bytes;
            while ((segment_bytes = sentence_segment_bytes(pending, false)) > 0) {
                char segment[1024];
                memcpy(segment, pending, segment_bytes);
                segment[segment_bytes] = '\0';
                memmove(pending, pending + segment_bytes, strlen(pending + segment_bytes) + 1);
                playback_err = synthesize_and_queue_segment(segment, &stream,
                                                            &playback_task_started);
                if (playback_err != ESP_OK) break;
            }
            if (playback_err != ESP_OK) break;
        }
        if (err == ESP_ERR_TIMEOUT) {
            esp_err_t cancel_err = julia_ai_cancel_request();
            if (cancel_err != ESP_OK && cancel_err != ESP_ERR_INVALID_STATE)
                ESP_LOGW(TAG, "AI request cancellation failed: %s", esp_err_to_name(cancel_err));
        }
        if (playback_err == ESP_OK && pending[0])
            playback_err = synthesize_and_queue_segment(pending, &stream,
                                                        &playback_task_started);
        ESP_LOGI(TAG, "AI elapsed=%lldms", esp_timer_get_time() / 1000 - session_started_ms);
        if (playback_task_started) {
            stream_pcm_item_t end = {.end = true};
            xQueueSend(stream.queue, &end, portMAX_DELAY);
            xSemaphoreTake(stream.done, portMAX_DELAY);
            if (playback_err == ESP_OK) playback_err = stream.result;
            response_played = stream.played_any && playback_err == ESP_OK;
            streamed_reply = true;
            if (playback_err != ESP_OK) {
                failure = stream.played_any ? VOICE_FAILURE_PLAYBACK : VOICE_FAILURE_TTS;
                ESP_LOGE(TAG, "Streaming playback failed: %s", esp_err_to_name(playback_err));
            }
        }
        vQueueDelete(stream.queue);
        vSemaphoreDelete(stream.done);
    }
    if (!answer[0]) {
        failure = VOICE_FAILURE_AI; ESP_LOGE(TAG, "AI returned no text"); goto done;
    }
    if (SESSION_TIMED_OUT()) { failure = VOICE_FAILURE_AI; goto done; }
    ESP_LOGI(TAG, "Julia: %s", answer);
    if (streamed_reply) {
        julia_memory_record_turn_with_emotion(user_text, answer, memory_emotion);
        turn_recorded = true;
        if (response_played) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            s_followup_ready_ms = now_ms + FOLLOWUP_COOLDOWN_MS;
            s_followup_until_ms = now_ms + FOLLOWUP_WINDOW_MS;
        }
        goto done;
    }
    // Keep the dialog animation active while cloud or offline speech is rendered.
    julia_voice_handle_event(EVT_MULTI_TURN_DETECTED);
    if (!turn_recorded) julia_memory_record_turn_with_emotion(user_text, answer, memory_emotion);
    int16_t *reply_pcm = NULL; size_t reply_samples = 0;
    ESP_LOGI(TAG, "Voice stack before TTS: %u bytes free",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    err = julia_speech_tts(answer, &reply_pcm, &reply_samples);
    ESP_LOGI(TAG, "TTS elapsed=%lldms", esp_timer_get_time() / 1000 - session_started_ms);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "TTS first attempt failed: %s; retrying", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
        err = julia_speech_tts(answer, &reply_pcm, &reply_samples);
    }
    if (SESSION_TIMED_OUT()) { free(reply_pcm); failure = VOICE_FAILURE_TTS; goto done; }
    if (err != ESP_OK) {
        failure = VOICE_FAILURE_TTS;
        ESP_LOGE(TAG, "TTS failed after retry: %s", esp_err_to_name(err)); goto done;
    }
    ESP_LOGI(TAG, "Playing %.2f seconds", (double)reply_samples / JULIA_AUDIO_SAMPLE_RATE);
    // Keep WakeNet active only during playback so a new wake word can interrupt TTS.
    s_afe->enable_wakenet(s_afe_data);
    s_interrupt_requested = false;
                julia_lipsync_begin();
                julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_SPEAKING);
    size_t remaining = reply_samples * sizeof(int16_t);
    uint8_t *cursor = (uint8_t *)reply_pcm;
    while (remaining && !s_interrupt_requested) {
        size_t chunk = remaining > 4096 ? 4096 : remaining;
        err = julia_lipsync_play((const int16_t *)cursor, chunk / sizeof(int16_t));
        if (err != ESP_OK) break;
        cursor += chunk;
        remaining -= chunk;
    }
    if (s_interrupt_requested) err = ESP_ERR_INVALID_STATE;
    s_afe->disable_wakenet(s_afe_data);
    esp_err_t stop_err = julia_lipsync_end();
    if (err == ESP_OK) err = stop_err;
    free(reply_pcm);
    if (err != ESP_OK) {
        failure = VOICE_FAILURE_PLAYBACK;
        ESP_LOGE(TAG, "Playback failed: %s", esp_err_to_name(err));
    }
    else {
        response_played = true;
        int64_t now_ms = esp_timer_get_time() / 1000;
        s_followup_ready_ms = now_ms + FOLLOWUP_COOLDOWN_MS;
        s_followup_until_ms = now_ms + FOLLOWUP_WINDOW_MS;
        ESP_LOGI(TAG, "Follow-up window open for %dms", FOLLOWUP_WINDOW_MS);
    }

done:
    /* 三级输出链：云端 PCM 已在上方尝试；未播放任何内容时再尝试本地
     * TTS。只有扬声器播放本身失败时才跳过本地，避免重复走同一故障。 */
    if (!response_played && failure != VOICE_FAILURE_PLAYBACK) {
        const char *local_text = answer[0] ? answer : "网络好像不太好，请稍后再试。";
        julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_SPEAKING);
        esp_err_t local_err = speak_local_chunked(local_text);
        if (local_err == ESP_OK) {
            response_played = true;
            ESP_LOGI(TAG, "Voice fallback selected: local TTS");
        } else {
            ESP_LOGW(TAG, "Local TTS fallback unavailable: %s", esp_err_to_name(local_err));
        }
    }
    if (!response_played) {
        const char *offline_text = "网络暂时不可用";
        if (failure == VOICE_FAILURE_ASR) offline_text = "没有听清楚，请再说一次";
        else if (failure == VOICE_FAILURE_TTS) offline_text = "语音生成失败，请稍后再试";
        else if (failure == VOICE_FAILURE_PLAYBACK) offline_text = "扬声器播放失败";
        bool network_failure = !julia_wifi_is_connected() || err == ESP_ERR_HTTP_CONNECT;
        esp_err_t prompt_err = ESP_FAIL;
        if (network_failure) {
            /* This cached sentence explicitly says the network is unstable;
             * never use it for ASR, TTS or speaker failures. */
    julia_lipsync_begin();
    julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_SPEAKING);
            prompt_err = julia_lipsync_play_file(NETWORK_PROMPT_PATH);
            esp_err_t stop_err = julia_lipsync_end();
            if (prompt_err == ESP_OK) prompt_err = stop_err;
        } else {
            /* 本地 TTS 不可用时仍播放固化 PCM，不能静音卡死。 */
            ESP_LOGW(TAG, "Non-network failure feedback: %s", offline_text);
            julia_lipsync_begin();
            julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_SPEAKING);
            prompt_err = julia_lipsync_play_file(NETWORK_PROMPT_PATH);
            esp_err_t stop_err = julia_lipsync_end();
            if (prompt_err == ESP_OK) prompt_err = stop_err;
        }
        if (prompt_err != ESP_OK) play_failure_code(failure ? failure : VOICE_FAILURE_AI);
    }
    return_to_standby();
    ESP_LOGI(TAG, "Voice session complete: total=%lldms failure=%d",
             esp_timer_get_time() / 1000 - session_started_ms, failure);
    ESP_LOGI(TAG, "Voice heap free=%u min=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    s_wake_resume_ms = esp_timer_get_time() / 1000 + 1000;
    s_afe->enable_wakenet(s_afe_data);
    s_session_busy = false;
    ESP_LOGI(TAG, "Ready: say %s", WAKE_WORD_DISPLAY_TEXT);
    vTaskDelete(NULL);
}

static void feed_task(void *arg)
{
    (void)arg;
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int16_t *samples = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!samples) { ESP_LOGE(TAG, "feed buffer allocation failed"); vTaskDelete(NULL); return; }
    unsigned diagnostic_frames = 0;
    while (true) {
        size_t read = 0;
        if (julia_audio_mic_read(samples, chunk, &read) == ESP_OK && read == (size_t)chunk) {
            s_afe->feed(s_afe_data, samples);
            if (++diagnostic_frames >= 100) {
                int peak = 0; uint64_t energy = 0;
                for (int i = 0; i < chunk; ++i) {
                    int value = abs(samples[i]);
                    if (value > peak) peak = value;
                    energy += (uint64_t)value * value;
                }
                ESP_LOGI(TAG, "Mic level: peak=%d rms=%u gain=%dx", peak,
                         (unsigned)sqrt((double)energy / chunk), JULIA_AUDIO_MIC_GAIN);
                diagnostic_frames = 0;
            }
        }
    }
}

static void detect_task(void *arg)
{
    (void)arg;
    int chunk = s_afe->get_fetch_chunksize(s_afe_data);
    int frame_ms = chunk * 1000 / JULIA_AUDIO_SAMPLE_RATE;
    bool capturing = false, heard_speech = false;
    int silence_ms = 0, waiting_ms = 0, followup_vad_frames = 0;
    int capture_log_ms = 0, energy_speech_frames = 0;
    uint32_t noise_rms = 180, noise_peak = 500;
    uint32_t capture_rms_threshold = SPEECH_RMS_THRESHOLD;
    uint32_t capture_peak_threshold = SPEECH_PEAK_THRESHOLD;
    size_t first_speech_sample = SIZE_MAX, last_speech_sample = 0;
    while (true) {
        afe_fetch_result_t *result = s_afe->fetch(s_afe_data);
        if (!result || result->ret_value == ESP_FAIL) continue;
        size_t result_samples = result->data_size / sizeof(int16_t);
        int frame_peak = 0;
        uint64_t frame_energy = 0;
        for (size_t i = 0; i < result_samples; ++i) {
            int value = abs(result->data[i]);
            if (value > frame_peak) frame_peak = value;
            frame_energy += (uint64_t)value * value;
        }
        uint32_t frame_rms = result_samples
                                 ? (uint32_t)sqrt((double)frame_energy / result_samples)
                                 : 0;
        if (!capturing && !s_session_busy && result->vad_state != AFE_VAD_SPEECH &&
            frame_rms < noise_rms * 3U) {
            noise_rms = (noise_rms * 31U + frame_rms) / 32U;
            noise_peak = (noise_peak * 31U + (uint32_t)frame_peak) / 32U;
        }
        if (result->vad_state == AFE_VAD_SPEECH)
            s_last_audio_activity_ms = esp_timer_get_time() / 1000;
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool followup_active = now_ms >= s_followup_ready_ms && now_ms < s_followup_until_ms;
        /* Never accumulate follow-up VAD while a session or TTS playback is
         * still active. Otherwise Julia's own speaker audio preloads the
         * counter and starts a new listening round as soon as the task ends. */
        if (!s_session_busy && followup_active && result->vad_state == AFE_VAD_SPEECH)
            ++followup_vad_frames;
        else followup_vad_frames = 0;
        bool followup_detected = followup_active && followup_vad_frames >= 3;
        bool wake_detected = result->wakeup_state == WAKENET_DETECTED && now_ms >= s_wake_resume_ms;
        if (s_session_busy && wake_detected) {
            // A wake word during playback is treated as an interrupt request.
            julia_voice_interrupt();
            continue;
        }
        if (!s_session_busy && !capturing && (wake_detected ||
            (followup_detected && s_dialog_rounds < MAX_DIALOG_ROUNDS))) {
            ESP_LOGI(TAG, "%s detected", wake_detected ? "Wake word" : "Follow-up speech");
            julia_routine_on_activity(JULIA_ACTIVITY_WAKE);
            s_afe->disable_wakenet(s_afe_data);
            on_user_interaction();
            julia_voice_handle_event(EVT_USER_CALL);
            s_followup_until_ms = 0; followup_vad_frames = 0;
            if (wake_detected) s_dialog_rounds = 0;
            if (wake_detected) {
                esp_err_t response_err = wake_reply_play();
                if (response_err != ESP_OK) {
                    ESP_LOGW(TAG, "Offline wake reply unavailable: %s",
                             esp_err_to_name(response_err));
                    julia_audio_play_tone(740, 70, 25);
                }
                ESP_LOGI(TAG, "Wake reply playback finished result=%s; starting capture",
                         esp_err_to_name(response_err));
            } else {
                julia_audio_play_tone(740, 70, 25);
            }
            s_recording = heap_caps_malloc(MAX_RECORD_SAMPLES * sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_recording) { s_afe->enable_wakenet(s_afe_data); continue; }
            s_recorded_samples = 0; capturing = true; heard_speech = false;
            silence_ms = 0; waiting_ms = 0; capture_log_ms = 0;
            energy_speech_frames = 0;
            first_speech_sample = SIZE_MAX; last_speech_sample = 0;
            capture_rms_threshold = noise_rms * 2U;
            if (capture_rms_threshold < SPEECH_RMS_THRESHOLD)
                capture_rms_threshold = SPEECH_RMS_THRESHOLD;
            capture_peak_threshold = noise_peak * 2U;
            if (capture_peak_threshold < SPEECH_PEAK_THRESHOLD)
                capture_peak_threshold = SPEECH_PEAK_THRESHOLD;
            ESP_LOGI(TAG, "Listening... noise=%u/%u threshold=%u/%u",
                     (unsigned)noise_peak, (unsigned)noise_rms,
                     (unsigned)capture_peak_threshold, (unsigned)capture_rms_threshold);
        }
        if (!capturing) continue;
        size_t frame_samples = result_samples;
        if (frame_samples > MAX_RECORD_SAMPLES - s_recorded_samples)
            frame_samples = MAX_RECORD_SAMPLES - s_recorded_samples;
        memcpy(s_recording + s_recorded_samples, result->data, frame_samples * sizeof(int16_t));
        s_recorded_samples += frame_samples; waiting_ms += frame_ms;
        bool energy_speech = (uint32_t)frame_peak >= capture_peak_threshold ||
                             frame_rms >= capture_rms_threshold;
        if (energy_speech) {
            if (energy_speech_frames < SPEECH_CONFIRM_FRAMES) energy_speech_frames++;
        } else {
            energy_speech_frames = 0;
        }
        bool confirmed_energy_speech = energy_speech_frames >= SPEECH_CONFIRM_FRAMES;
        capture_log_ms += frame_ms;
        if (capture_log_ms >= 500) {
            ESP_LOGI(TAG, "Listening level: peak=%d rms=%u vad=%d heard=%d",
                     frame_peak, (unsigned)frame_rms,
                     result->vad_state == AFE_VAD_SPEECH, heard_speech);
            capture_log_ms = 0;
        }
        bool vad_above_noise = result->vad_state == AFE_VAD_SPEECH &&
                               frame_rms >= noise_rms + noise_rms / 3U;
        bool speech_evidence = confirmed_energy_speech || vad_above_noise;
        if (speech_evidence) {
            if (!heard_speech) ESP_LOGI(TAG, "Speech started: peak=%d rms=%u vad=%d",
                                        frame_peak, (unsigned)frame_rms,
                                        result->vad_state == AFE_VAD_SPEECH);
            if (first_speech_sample == SIZE_MAX)
                first_speech_sample = s_recorded_samples - frame_samples;
            last_speech_sample = s_recorded_samples;
            heard_speech = true; silence_ms = 0;
        }
        else if (heard_speech) silence_ms += frame_ms;
        bool finish = s_recorded_samples >= MAX_RECORD_SAMPLES ||
                      (heard_speech && waiting_ms >= MIN_CAPTURE_MS && silence_ms >= END_SILENCE_MS) ||
                      (!heard_speech && waiting_ms >= NO_SPEECH_TIMEOUT_MS);
        if (!finish) continue;
        capturing = false;
        if (heard_speech && first_speech_sample != SIZE_MAX && last_speech_sample > first_speech_sample) {
            const size_t pre_roll = JULIA_AUDIO_SAMPLE_RATE / 5;
            const size_t post_roll = JULIA_AUDIO_SAMPLE_RATE / 4;
            size_t start = first_speech_sample > pre_roll ? first_speech_sample - pre_roll : 0;
            size_t end = last_speech_sample + post_roll;
            if (end > s_recorded_samples) end = s_recorded_samples;
            size_t original_samples = s_recorded_samples;
            memmove(s_recording, s_recording + start, (end - start) * sizeof(int16_t));
            s_recorded_samples = end - start;
            ESP_LOGI(TAG, "Capture trimmed: %.2fs -> %.2fs",
                     (double)original_samples / JULIA_AUDIO_SAMPLE_RATE,
                     (double)s_recorded_samples / JULIA_AUDIO_SAMPLE_RATE);
        }
        if (!heard_speech) {
            /* This enclosure's AFE VAD can remain silent even when WakeNet
             * and raw microphone levels clearly detect the user. Preserve
             * the fixed capture and let cloud ASR make the final decision. */
            ESP_LOGW(TAG, "VAD did not confirm speech; submitting %dms fallback capture",
                     waiting_ms);
        }
        s_session_busy = true;
        if (++s_dialog_rounds > MAX_DIALOG_ROUNDS) {
            ESP_LOGW(TAG, "dialog round limit reached");
            s_dialog_rounds = 0;
            s_followup_until_ms = 0;
        }
        ESP_LOGI(TAG, "Capture complete: %dms, %u samples", waiting_ms,
                 (unsigned)s_recorded_samples);
        /* This task records conversation metadata in NVS. NVS flash access
         * temporarily disables the external-memory cache, so its stack must
         * live in internal SRAM (an external PSRAM stack asserts in IDF's
         * cache safety check). Core dumps show <12 KB peak usage. */
        if (xTaskCreate(session_task, "voice_session", 24576, NULL, 5, NULL) != pdPASS) {
            free(s_recording); s_recording = NULL; s_recorded_samples = 0; s_session_busy = false;
            return_to_standby(); s_afe->enable_wakenet(s_afe_data);
        }
    }
}

esp_err_t julia_voice_init(void)
{
    ESP_RETURN_ON_FALSE(CONFIG_JULIA_AI_API_KEY[0], ESP_ERR_INVALID_STATE, TAG, "DashScope key is empty");
    ESP_RETURN_ON_ERROR(julia_audio_mic_start(), TAG, "start microphone");
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "voice_sr", &s_cpu_lock),
                        TAG, "create CPU frequency lock");
    ESP_RETURN_ON_ERROR(esp_pm_lock_acquire(s_cpu_lock), TAG, "lock CPU frequency");
    ESP_RETURN_ON_ERROR(julia_ai_init_qwen(CONFIG_JULIA_AI_API_KEY), TAG, "initialize Qwen");
    ESP_RETURN_ON_ERROR(julia_ai_chat_start(), TAG, "start Qwen chat");
    esp_err_t home_err = julia_home_register_ai_functions();
    if (home_err != ESP_OK) ESP_LOGW(TAG, "home tools unavailable: %s", esp_err_to_name(home_err));
    s_models = esp_srmodel_init(ACTIVE_WAKENET_MODEL_PARTITION);
    ESP_RETURN_ON_FALSE(s_models, ESP_ERR_NOT_FOUND, TAG, "speech model partition unavailable");
    char *wake_model = esp_srmodel_filter(s_models, ESP_WN_PREFIX,
                                          ACTIVE_WAKENET_MODEL_NAME);
    ESP_RETURN_ON_FALSE(wake_model, ESP_ERR_NOT_FOUND, TAG, "WakeNet model unavailable");
    afe_config_t config = AFE_CONFIG_DEFAULT();
    config.aec_init = false; config.se_init = false;
    config.vad_init = true; config.wakenet_init = true;
    /* The default mode 3 rejected normal speech on this enclosure. Mode 0 is
     * less restrictive; the consecutive-frame energy gate still suppresses
     * isolated ambient spikes. */
    config.vad_mode = VAD_MODE_0;
    config.wakenet_model_name = wake_model; config.afe_ringbuf_size = 50;
    config.wakenet_mode = DET_MODE_95;
    config.pcm_config.total_ch_num = 1; config.pcm_config.mic_num = 1; config.pcm_config.ref_num = 0;
    s_afe = &ESP_AFE_SR_HANDLE;
    s_afe_data = s_afe->create_from_config(&config);
    ESP_RETURN_ON_FALSE(s_afe_data, ESP_ERR_NO_MEM, TAG, "create AFE failed");
    s_fsm_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_fsm_lock, ESP_ERR_NO_MEM, TAG, "create FSM lock failed");
    julia_fsm_init(&s_fsm); s_fsm.on_enter = voice_on_enter;
    s_last_audio_activity_ms = esp_timer_get_time() / 1000;
    TaskHandle_t feed_handle = NULL;
    if (xTaskCreatePinnedToCore(feed_task, "voice_feed", 4096, NULL, 6,
                                &feed_handle, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(detect_task, "voice_detect", 6144, NULL, 5,
                                NULL, 1) != pdPASS) {
        vTaskDelete(feed_handle);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Voice pipeline ready model=%s wake_word=%s heap=%u psram=%u",
             wake_model, WAKE_WORD_DISPLAY_TEXT,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}
