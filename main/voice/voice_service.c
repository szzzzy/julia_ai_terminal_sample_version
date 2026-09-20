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
#include "voice_timing.h"
#include "esp_random.h"

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
#include "voice_local_capture.h"
#include "julia_quiet_power.h"
#include "julia_night_schedule.h"
#include "julia_motion.h"
#include "voice_playback.h"
#include "voice_state_sync.h"
#include "voice_control_guard.h"
#include "ota_control_plane.h"
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
static int64_t s_rx_backpressure_since_us, s_last_backpressure_log_us;
static uint32_t s_rx_backpressure_generation;

/* 读下一帧之前先预留一条最大报文的空间：TCP 流控负责吸收服务器突发，
 * 这样既不必复制或丢弃部分 PCM，也不必在 on_binary 内等待。
 * 控制帧同样可能被拖后，因此不能无限期等待停住的播放 worker：
 * 一秒内腾不出空间就结束本次会话。 */
static bool voice_service_can_receive(void)
{
    voice_playback_buffer_status_t status;
    if (!voice_playback_get_buffer_status(&status) ||
        status.capacity - status.queued >= WSS_TRANSPORT_MAX_PAYLOAD) {
        s_rx_backpressure_since_us = 0;
        return true;
    }
    int64_t now = esp_timer_get_time();
    if (!s_rx_backpressure_since_us || s_rx_backpressure_generation != status.generation) {
        s_rx_backpressure_since_us = now;
        s_rx_backpressure_generation = status.generation;
    }
    bool stalled = now - s_rx_backpressure_since_us >= 1000000LL;
    if (stalled || !s_last_backpressure_log_us || now - s_last_backpressure_log_us >= 60000000LL) {
        s_last_backpressure_log_us = now;
        ESP_LOGW(TAG, "playback backpressure stalled=%d generation=%lu queued=%u capacity=%u rate=%lu "
                      "rx=%llu accepted=%llu dequeued=%llu i2s_bytes=%llu configured_rate=%lu "
                      "first_rx_us=%lld last_rx_us=%lld i2s_request_us=%lld i2s_started_us=%lld "
                      "first_output_us=%lld output_age_ms=%lld",
                 stalled, (unsigned long)status.generation, (unsigned)status.queued,
                 (unsigned)status.capacity, (unsigned long)status.rate,
                 (unsigned long long)status.received_bytes, (unsigned long long)status.accepted_bytes,
                 (unsigned long long)status.dequeued_bytes, (unsigned long long)status.output_bytes,
                 (unsigned long)status.configured_rate, (long long)status.first_input_us,
                 (long long)status.last_input_us, (long long)status.i2s_request_us,
                 (long long)status.i2s_started_us,
                 (long long)status.first_output_us,
                 (long long)(status.last_output_us ? (now-status.last_output_us)/1000 : -1));
    }
    if (stalled) {
        wss_transport_fail_session();
    }
    return false;
}

static void post_fsm_event(fsm_event_t event)
{
    /* WSS 按序接收的下一条命令只能观察到本条事件已经提交后的状态。 */
    esp_err_t err = julia_fsm_runtime_post_sync(event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FSM event %s rejected: %s", julia_fsm_event_name(event),
                 esp_err_to_name(err));
        /* 调用方都在 WSS owner 上下文；已消费的 MIC_STOP 或播放完成结果不能停在
         * “事件已被取走但状态未提交”的中间态，否则本轮对话再也不会收尾。 */
        if (event != EVT_WSS_DISCONNECTED) wss_transport_fail_session();
    }
}

/* PSRAM MIC 上行 ring：格数 256，单帧 656 字节。正常每轮只发送 1 帧；积压达到
 * 2 帧才启动追赶，每轮最多 8 帧，并在帧与帧之间检查 8ms（VOICE_UPLINK_RUN_BUDGET_US
 * 微秒）的时间预算（见 docs/PROTOCOL.md §2）。预算只是帧间检查：已进入的 TLS 写调用
 * 无法被中断，且每轮至少推进 1 帧，因此单轮实际耗时可能超过 8ms，它不是硬上界。
 * 扩大这两个上限会直接压缩控制队列和接收数据的执行机会。 */
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
    VOICE_JOB_SEND_TEXT,     /**< 向 WSS 直接发一条文本控制帧；当前全仓无生产者，未接通。 */
    VOICE_JOB_INTENT_GOODNIGHT, /**< MQTT 晚安语义，转交 WSS 任务串行收尾。 */
    VOICE_JOB_INTENT_DISMISS,   /**< MQTT 结束沟通语义，转交 WSS 任务串行收尾。 */
    VOICE_JOB_SCOPED_CONTROL,  /**< 原始封装留到 WSS owner 校验，避免排队期间换轮。 */
} voice_job_type_t;

typedef struct {
    voice_job_type_t type;
    size_t len;
    uint8_t data[WSS_TRANSPORT_MAX_PAYLOAD]; /**< URI 或控制消息内容。 */
} voice_job_t;

/** 分别记录“是否向服务器发送声音”和“是否正在等待用户完成本轮话语”。 */
/* s_mic_state_lock 是关中断的 portMUX：进入临界区后只剩本核高优先级中断，任何
 * 阻塞调用、日志或等待其他锁的调用都会拖住整个核。因此锁内只允许两件事：读写
 * 上述标志与时限，以及调用 board_audio_enable_wss_mic()/mic_wake() 这类只写
 * volatile 标志、不会阻塞的函数（见 board_audio.c）。 */
static portMUX_TYPE s_mic_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_mic_streaming;
static bool s_dialog_listening;
static char s_interaction_id[VOICE_INTERACTION_ID_MAX_LEN];
static julia_main_state_t s_interaction_origin = JULIA_MAIN_STATE_S1_COMPANION;
static bool s_s4_ready_pending;
static bool s_s4_ready_committed;
static bool s_wake_reply_expected;
/* 期限由 FSM 观察者写入，过期判定与清除只在 WSS owner 中完成。 */
static int64_t s_reply_deadline_us;
static int64_t s_listen_deadline_us;

typedef enum {
    VOICE_PLAYBACK_ROLE_NONE = 0,
    VOICE_PLAYBACK_ROLE_WAKE_REPLY,
    VOICE_PLAYBACK_ROLE_DIALOG_REPLY,
    VOICE_PLAYBACK_ROLE_SELF_TEST,
    VOICE_PLAYBACK_ROLE_DISMISS_REPLY,
    VOICE_PLAYBACK_ROLE_GOODNIGHT_REPLY,
} voice_playback_role_t;

static bool playback_role_is_terminal(voice_playback_role_t role)
{
    return role == VOICE_PLAYBACK_ROLE_DISMISS_REPLY ||
           role == VOICE_PLAYBACK_ROLE_GOODNIGHT_REPLY;
}

extern const uint8_t bye_no_bother_wav_start[]
    asm("_binary_bye_no_bother_16k_mono_16bit_wav_start");
extern const uint8_t bye_no_bother_wav_end[]
    asm("_binary_bye_no_bother_16k_mono_16bit_wav_end");
extern const uint8_t goodnight_wav_start[]
    asm("_binary_goodnight_16k_mono_16bit_wav_start");
extern const uint8_t goodnight_wav_end[]
    asm("_binary_goodnight_16k_mono_16bit_wav_end");
extern const uint8_t wake_prompt_wav_start[]
    asm("_binary_wake_prompt_16k_mono_16bit_wav_start");
extern const uint8_t wake_prompt_wav_end[]
    asm("_binary_wake_prompt_16k_mono_16bit_wav_end");

/* 下列播放、文件和上传进度只由负责语音连接的任务修改，避免跨任务互相覆盖。 */
static uint32_t s_playback_generation;
static voice_playback_role_t s_playback_role;
static bool s_session_activated;
#if CONFIG_JULIA_VOICE_TIMING
/* 仅 WSS owner 使用；与上行 generation 分开统计，后者可能在会话中途变化。 */
static uint32_t s_timing_epoch;
static uint32_t s_timing_tx_id, s_timing_pcm_frames;
static int64_t s_timing_last_pcm, s_timing_max_send;
#endif
/* 该时间戳由 MIC producer 在临界区内存入、会话开始时清零；诊断日志统一由 WSS owner
 * 输出，采音任务和 I2S 任务都不做 telemetry 打印。 */
static int64_t s_last_capture_us;
static struct {
    uint32_t generation;
    char session[33];
    char interaction[VOICE_INTERACTION_ID_MAX_LEN];
    int64_t spks_us, first_pcm_us;
    bool output_reported;
} s_audio_timing;

static void voice_service_timing_poll(bool completed)
{
    if (s_audio_timing.generation != s_playback_generation) return;
    voice_playback_timing_t timing;
    if (!voice_playback_get_timing(s_audio_timing.generation, &timing)) return;
    if (!completed && (s_audio_timing.output_reported || !timing.first_output_us)) return;
    portENTER_CRITICAL(&s_mic_state_lock);
    int64_t last_capture = s_last_capture_us;
    portEXIT_CRITICAL(&s_mic_state_lock);
    ESP_LOGI(TAG, "audio_timing clock=boot_monotonic_us session_id=%s interaction_id=%s"
             " generation=%" PRIu32 " last_capture=%" PRIi64 " spks=%" PRIi64
             " first_pcm=%" PRIi64 " first_i2s_write=%" PRIi64 " completed=%" PRIi64,
             s_audio_timing.session, s_audio_timing.interaction[0] ? s_audio_timing.interaction : "unknown",
             timing.generation, last_capture, s_audio_timing.spks_us, s_audio_timing.first_pcm_us,
             timing.first_output_us, timing.completed_us);
    /* SPKS 不带 utterance ID：只上报播放 generation，不猜测轮次；
     * collector 只关联已完成且判决为 speech 的无歧义片段。 */
#if CONFIG_JULIA_VOICE_TIMING
    if (!s_audio_timing.output_reported && timing.first_output_us)
        voice_timing_record(VT_FIRST_I2S,s_timing_epoch,0,timing.generation,
                            timing.first_output_us,0,0);
#endif
    s_audio_timing.output_reported = true;
    if (completed) s_audio_timing.generation = 0;
}
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
#define VOICE_SCOPED_CONTROL_MAX_LEN 768U
static voice_control_guard_t s_control_guard;
static char s_voice_device_id[NATIVE_OTA_DEVICE_ID_SIZE];
static char s_round_ack[512], s_round_request[64];
static bool s_round_pending, s_round_speech;
static int64_t s_busy_until_us;
static void voice_service_apply_scoped_control(const uint8_t *data, size_t len);
static bool voice_service_handle_control_json(const uint8_t *data, size_t len);
#endif
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

/* ring 和 pump 把 0 当作“无代次／无条件停止”，因此 generation 从 1 起发放，
 * 递增后若回绕到 0 就跳过。 */
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

/** 本地唤醒模式下，达到配置的无交互期限后关闭陪伴 PCM 上传，同时进入 S3。 */
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
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    /* 本轮先验证半双工边界：板级没有 AEC，不能由扬声器回声直接触发本地停播。 */
    static int64_t playback_guard_until_us;
    int64_t now_us = esp_timer_get_time();
    if (voice_playback_is_active()) playback_guard_until_us = now_us + 600000;
    julia_fsm_snapshot_t capture_snapshot;
    julia_fsm_runtime_get_snapshot(&capture_snapshot);
    julia_main_state_t state = capture_snapshot.main_state;
    bool listen_window = state == JULIA_MAIN_STATE_S4_INTERACTION;
    lc_mode_t mode = LC_OFF;
    if (state == JULIA_MAIN_STATE_S3_STANDBY || state == JULIA_MAIN_STATE_S5_SILENT ||
        state == JULIA_MAIN_STATE_S6_SLEEP) mode = LC_WAKE;
    else if (state == JULIA_MAIN_STATE_S1_COMPANION || state == JULIA_MAIN_STATE_S4_INTERACTION ||
             (state == JULIA_MAIN_STATE_S2_DIALOG &&
              capture_snapshot.s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING)) mode = LC_DIALOG;
    /* S4 提交与播放任务启动之间也不能开麦；回执发出前保留门控，避免首句先于云端握手。 */
    portENTER_CRITICAL(&s_mic_state_lock);
    bool wake_pending = s_wake_reply_expected || s_s4_ready_pending;
    portEXIT_CRITICAL(&s_mic_state_lock);
    if (now_us < playback_guard_until_us ||
        (state == JULIA_MAIN_STATE_S4_INTERACTION && wake_pending)) mode = LC_OFF;
    voice_local_capture_listen_window(listen_window ? capture_snapshot.revision : 0);
    voice_local_capture_mode(mode);
    voice_local_capture_frame(frame, bytes);
    return;
#endif
    esp_err_t err = voice_service_send_chunk(frame, bytes);
    if (err == ESP_ERR_NO_MEM) {
        /* 只有缓冲确实装满才记录容量故障；发送文件或连接切换造成的主动暂停不算丢帧。 */
        if ((++s_uplink_dropped & 255U) == 1U) {
            ESP_LOGW(TAG, "MIC uplink ring full drops=%lu",
                     (unsigned long)s_uplink_dropped);
        }
    }
}

/* 扩展名用 strcasecmp 比较，因此 ".WAV" 也放行；这与 voice_uri.c 中区分大小写的
 * URI 前缀匹配不同（"sd:/x.wav" 会先被 voice_uri_to_path() 拒绝）。 */
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

    /* 文件访问必须在 SD 锁内完成。本文件只提供默认弱实现，它不加锁、恒返回 true；
     * 产品必须覆盖这两个符号接入真实互斥，否则这里等同无保护访问 SD。 */
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

    /* s_file 只在 BEGIN 帧发送成功后才赋值；由 weak julia_wireless_sd_lock()
     * 取得的 SD 锁随它一起持有，唯一释放点是 voice_service_close_file()。
     * 因此 s_file != NULL 与“持有 SD 锁”互为充要条件，其他路径不得单独解锁。
     * s_file 本身不在临界区内读写，靠“打开、推进、关闭都在 WSS 会话任务”串行保护。 */
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

    if (playback_role_is_terminal(completed_role)) {
        /* 播放与 MIC_START 由同一任务收尾；被打断后旧完成结果不能再触发退出。 */
        if (julia_fsm_runtime_get_state() == JULIA_MAIN_STATE_S4_INTERACTION) {
            post_fsm_event(completed_role == VOICE_PLAYBACK_ROLE_GOODNIGHT_REPLY
                               ? EVT_INTENT_GOODNIGHT : EVT_INTENT_DISMISS);
        }
        julia_idle_display_set_busy(false);
        return;
    }

    if (completed_role == VOICE_PLAYBACK_ROLE_WAKE_REPLY) {
        /* 本地收音在播放结束、防回声间隔及 S4 回执发送完成后放行；不等待旧 MIC_START。 */
        portENTER_CRITICAL(&s_mic_state_lock);
        s_wake_reply_expected = false;
        portEXIT_CRITICAL(&s_mic_state_lock);
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
#if CONFIG_JULIA_VOICE_TIMING
    int64_t tx_begin = esp_timer_get_time();
#endif
    esp_err_t sent = wss_transport_send_now(data[0] == '{' ? 0x1 : 0x2, data, len);
#if CONFIG_JULIA_VOICE_TIMING
    int64_t tx_end = esp_timer_get_time();
    uint32_t uid = 0;
    if (len == 656 && !memcmp(data,"PCM2",4)) {
        uid = (uint32_t)data[4] | ((uint32_t)data[5]<<8) |
              ((uint32_t)data[6]<<16) | ((uint32_t)data[7]<<24);
        if (sent == ESP_OK && uid == s_timing_tx_id) {
            if (!s_timing_pcm_frames)
                voice_timing_record(VT_FIRST_PCM_TX,s_timing_epoch,uid,0,tx_end,tx_begin,0);
            ++s_timing_pcm_frames;
            s_timing_last_pcm=tx_end;
            if (tx_end-tx_begin>s_timing_max_send) s_timing_max_send=tx_end-tx_begin;
        }
    } else if (len && data[0]=='{') {
        cJSON *o=cJSON_ParseWithLength((const char *)data,len);
        const cJSON *type=cJSON_GetObjectItemCaseSensitive(o,"type");
        if (voice_control_uint(o,"utterance_id",&uid) && cJSON_IsString(type)) {
            voice_timing_kind_t k = !strcmp(type->valuestring,"capture_start") ? VT_START_TX :
                !strcmp(type->valuestring,"capture_end") ? VT_END_TX : VT_ABORT_TX;
            if (sent == ESP_OK) {
                voice_timing_record(k,s_timing_epoch,uid,0,tx_end,tx_begin,0);
                if (k==VT_START_TX) {
                    s_timing_tx_id=uid;s_timing_pcm_frames=0;s_timing_last_pcm=s_timing_max_send=0;
                } else if (uid==s_timing_tx_id) {
                    if (s_timing_last_pcm) voice_timing_record(VT_LAST_PCM_TX,s_timing_epoch,uid,0,s_timing_last_pcm,0,0);
                    voice_timing_record(VT_UPLINK_STATS,s_timing_epoch,uid,0,tx_end,s_timing_max_send,s_timing_pcm_frames);
                }
            }
        }
        cJSON_Delete(o);
    }
    if (sent != ESP_OK) voice_timing_record(VT_TX_FAILED,s_timing_epoch,uid,0,tx_end,tx_begin,sent);
#endif
    if (sent != ESP_OK) {
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
    char payload[384];
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    int length = snprintf(payload, sizeof(payload),
        "{\"type\":\"state_ready\",\"device_id\":\"%s\",\"session_id\":\"%s\","
        "\"interaction_id\":\"%s\",\"interaction_seq\":%" PRIu32 ",\"request_id\":\"ready-%" PRIu32 "\",\"state\":\"S4\"}",
        s_voice_device_id, voice_state_sync_session_id(), id, s_control_guard.interaction_seq, s_uplink_generation);
#else
    int length = snprintf(payload, sizeof(payload),
        "{\"type\":\"state_ready\",\"interaction_id\":\"%s\",\"state\":\"S4\"}", id);
#endif
    if (length > 0 && (size_t)length < sizeof(payload) &&
        wss_transport_send_now(0x1, (const uint8_t *)payload, (size_t)length) == ESP_OK) {
        portENTER_CRITICAL(&s_mic_state_lock);
        s_s4_ready_pending = false;
        s_s4_ready_committed = false;
        portEXIT_CRITICAL(&s_mic_state_lock);
    }
}

static bool voice_service_reply_timeout_poll(void)
{
    portENTER_CRITICAL(&s_mic_state_lock);
    int64_t deadline = s_reply_deadline_us;
    bool expired = deadline != 0 && esp_timer_get_time() >= deadline;
    bool listen_expired = s_listen_deadline_us != 0 &&
                          esp_timer_get_time() >= s_listen_deadline_us;
    if (expired) s_reply_deadline_us = 0;
    if (listen_expired) s_listen_deadline_us = 0;
    portEXIT_CRITICAL(&s_mic_state_lock);
    if (!expired && !listen_expired) return false;

    ESP_LOGW(TAG, "%s timeout after %d s; ending stale voice session",
             expired ? "S2.2 reply" : "S4/S2.1 listening",
             expired ? CONFIG_JULIA_DIALOG_REPLY_TIMEOUT_SECONDS
                     : CONFIG_JULIA_DIALOG_LISTEN_TIMEOUT_SECONDS);
    /* 协议没有 reply/turn ID：与其让本轮迟到的 SPKS 被当成下一轮回答，不如重建会话。
     * 正常 teardown 会清 busy/listening/播放状态并退出 S2。 */
    wss_transport_fail_session();
    return true;
}

static bool voice_service_busy_wait(void)
{
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    /* 服务器 busy(voice_capacity) 是资源拒绝，不是断链：冷却期内新命令一律丢弃，
     * 不排队、不补发，也不伪造设备状态变化。到期后换新代次并丢弃旧连接残留帧，
     * 从空 ring 恢复实时上传。 */
    if (s_busy_until_us) {
        if (esp_timer_get_time()<s_busy_until_us) return true;
        s_busy_until_us=0;
        voice_service_resume_uplink_after_file();
    }
#endif
    return false;
}

static void voice_service_poll(void)
{
    if (!CONFIG_JULIA_LOCAL_CAPTURE_ENABLE && julia_fsm_is_quiet(julia_fsm_runtime_get_state())) {
        voice_service_pause_uplink_for_file();
        return;
    }
    voice_state_sync_poll();
    if (!voice_state_sync_is_ready()) return;
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    if (!voice_local_capture_poll()) return;
#endif
    if (voice_service_busy_wait()) return;
    if (!s_session_activated) {
        s_session_activated = true;
#if CONFIG_JULIA_SERVER_WAKE_ENABLE
        portENTER_CRITICAL(&s_mic_state_lock);
        s_mic_streaming = true;
        board_audio_mic_wake();
        board_audio_enable_wss_mic(true);
        portEXIT_CRITICAL(&s_mic_state_lock);
#endif
        ESP_LOGI(TAG, "cloud session synchronized; voice traffic enabled");
    }
    if (voice_service_reply_timeout_poll()) return;
    /* 关键确认由 WSS owner 直接发送，不与可丢弃的四槽控制作业竞争。 */
    voice_service_state_ready_poll();
    if ((playback_role_is_terminal(s_playback_role) ||
         (CONFIG_JULIA_LOCAL_CAPTURE_ENABLE && s_playback_role == VOICE_PLAYBACK_ROLE_WAKE_REPLY)) &&
        julia_fsm_runtime_get_state() != JULIA_MAIN_STATE_S4_INTERACTION) {
        bool stopped = voice_playback_stop_generation(s_playback_generation);
        s_playback_generation = 0;
        s_playback_role = VOICE_PLAYBACK_ROLE_NONE;
        if (stopped || !voice_playback_is_active()) julia_avatar_talking_stop();
        julia_idle_display_set_busy(false);
    }
    uint32_t generation;
    esp_err_t result;
    /* 完成槽每轮最多取走一个结果。槽里可能是被取消或已被替换的旧代次，此时只丢弃，
     * 不补播、也不拿它收尾当前这一轮，避免旧回答的结束动作打断新回答。 */
    if (voice_playback_take_completion(&generation, &result) &&
        generation == s_playback_generation) {
#if CONFIG_JULIA_VOICE_TIMING
        voice_playback_timing_t trace_done;
        if (voice_playback_get_timing(generation,&trace_done))
            voice_timing_record(VT_PLAY_DONE,s_timing_epoch,0,generation,
                                trace_done.completed_us,result,s_playback_role);
#endif
        voice_service_timing_poll(true);
        bool local_terminal = playback_role_is_terminal(s_playback_role);
        bool local_wake = CONFIG_JULIA_LOCAL_CAPTURE_ENABLE &&
                          s_playback_role == VOICE_PLAYBACK_ROLE_WAKE_REPLY;
        if (result == ESP_ERR_NO_MEM) {
            /* 溢出属于本轮话语失败，不得当作正常播完上报 speaker_done。 */
            (void)voice_service_send_error("ERROR playback_overflow");
            wss_transport_fail_session();
            return;
        }
        if (result != ESP_OK && local_wake) {
            /* 应答失败不能被当成正常播完而开放首句；保留门控直至会话清场。 */
            julia_avatar_talking_stop();
            ESP_LOGW(TAG, "Local wake prompt playback failed: %s", esp_err_to_name(result));
            wss_transport_fail_session();
            return;
        }
        voice_service_speaker_done();
        if (result != ESP_OK && local_terminal) {
            ESP_LOGW(TAG, "Terminal prompt playback failed: %s", esp_err_to_name(result));
        } else if (result != ESP_OK) {
            (void)voice_service_send_error(result == ESP_ERR_NO_MEM ? "ERROR playback_overflow" :
                                          result == ESP_ERR_TIMEOUT ? "ERROR playback_timeout" :
                                                                     "ERROR playback_failed");
        }
    }
    voice_service_timing_poll(false);
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
    if (julia_fsm_runtime_get_state() == JULIA_MAIN_STATE_S0_BOOT ||
        julia_fsm_runtime_get_service_state() != JULIA_SERVICE_ONLINE ||
        !mqtt_comm_is_ready() || !wss_transport_is_ready() || !voice_state_sync_is_ready()) return;
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    /* 本地收音版本不接受云端起音，包括能力协商尚未完成时。 */
    return;
#else
    /* 终止语义已提交：本地提示播放期间，VAD 回声或已入队的 MIC_START 都不能
     * 取消待执行的 S5/S6 迁移。 */
    if (playback_role_is_terminal(s_playback_role)) {
        ESP_LOGI(TAG, "MIC_START ignored: terminal reply must finish before another interaction");
        return;
    }
    julia_main_state_t state = julia_fsm_runtime_get_state();
    if (state == JULIA_MAIN_STATE_S0_BOOT || state == JULIA_MAIN_STATE_S7_FAULT ||
        state == JULIA_MAIN_STATE_S8_OTA) {
        ESP_LOGW(TAG, "MIC_START ignored in non-interactive state=%s", julia_fsm_main_state_name(state));
        return;
    }
#if CONFIG_JULIA_SERVER_WAKE_ENABLE && CONFIG_JULIA_CLOUD_STATE_SYNC_ENABLE
    if (state == JULIA_MAIN_STATE_S3_STANDBY || state == JULIA_MAIN_STATE_S5_SILENT ||
        state == JULIA_MAIN_STATE_S6_SLEEP) {
        ESP_LOGI(TAG, "MIC_START ignored in %s: waiting for wake_detected",
                 julia_fsm_main_state_name(state));
        return;
    }
#endif
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
    if (event != EVT_NONE && julia_fsm_runtime_post_sync(event) != ESP_OK) {
        wss_transport_fail_session();
        return;
    }
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
#endif
}

/** “结束说话”只结束本轮听音；免唤醒陪伴期间仍可继续上传环境声音。 */
static void voice_service_apply_mic_stop(void)
{
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    /* 结束由本地采音事件驱动，旧停麦命令不改变状态。 */
    return;
#else
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
#endif
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

/** 本地唤醒应答与现有 state_ready 回执并行，不等待网络音频。 */
static void voice_service_start_wake_prompt(void)
{
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    /* WSS owner 在同步提交 S4 后启动；FSM 观察者不操作播放器或网络。 */
    if (julia_fsm_runtime_get_state() != JULIA_MAIN_STATE_S4_INTERACTION) return;
    julia_avatar_talking_start();
    esp_err_t err = voice_playback_start_local_wav(wake_prompt_wav_start,
        (size_t)(wake_prompt_wav_end - wake_prompt_wav_start), false, &s_playback_generation);
    if (err != ESP_OK) {
        julia_avatar_talking_stop();
        ESP_LOGW(TAG, "Local wake prompt start failed: %s", esp_err_to_name(err));
        /* 不在应答缺失时悄悄开放收音；由连接恢复清理本轮门控。 */
        wss_transport_fail_session();
        return;
    }
    s_playback_role = VOICE_PLAYBACK_ROLE_WAKE_REPLY;
    memset(&s_audio_timing, 0, sizeof(s_audio_timing));
    s_audio_timing.generation = s_playback_generation;
    s_audio_timing.spks_us = esp_timer_get_time();
    snprintf(s_audio_timing.session, sizeof(s_audio_timing.session), "%s", voice_state_sync_session_id());
    snprintf(s_audio_timing.interaction, sizeof(s_audio_timing.interaction), "%s", s_interaction_id);
    ESP_LOGI(TAG, "Local wake prompt started generation=%" PRIu32, s_playback_generation);
#endif
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
    /* 拒绝时不改变 round、busy 或播放状态，恢复后也不重放该次唤醒。 */
    if (julia_fsm_runtime_get_state() == JULIA_MAIN_STATE_S0_BOOT ||
        julia_fsm_runtime_get_service_state() != JULIA_SERVICE_ONLINE ||
        !mqtt_comm_is_ready() || !wss_transport_is_ready() || !voice_state_sync_is_ready()) {
        cJSON_Delete(root);
        return true;
    }
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    if (!voice_local_capture_ready()) {
        cJSON_Delete(root);
        return true;
    }
#endif
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "interaction_id");
    if (!cJSON_IsString(id) || !interaction_id_is_valid(id->valuestring)) {
        ESP_LOGW(TAG, "Ignoring wake_detected with invalid interaction_id");
        cJSON_Delete(root);
        return true;
    }
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    const char *rejection = voice_control_guard_check(&s_control_guard, root,
        s_voice_device_id, voice_state_sync_session_id(), id->valuestring);
    uint32_t round;
    if (!rejection && (!s_round_pending || s_round_speech || !s_control_guard.active ||
        !voice_control_uint(root,"interaction_seq",&round) || round!=s_control_guard.interaction_seq ||
        strcmp(id->valuestring,s_control_guard.interaction_id))) rejection="round_sync_required";
    if (rejection) {
        ESP_LOGW(TAG, "wake_detected ignored: %s", rejection);
        cJSON_Delete(root);
        return true;
    }
#endif

    julia_main_state_t state = julia_fsm_runtime_get_state();
    if (state == JULIA_MAIN_STATE_S4_INTERACTION) {
        ESP_LOGW(TAG, "Ignoring wake_detected in S4; actual speech must use MIC_START");
        cJSON_Delete(root);
        return true;
    }
    if (state != JULIA_MAIN_STATE_S1_COMPANION &&
        state != JULIA_MAIN_STATE_S3_STANDBY &&
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
    if (julia_fsm_runtime_post_sync(EVT_WAKEUP) != ESP_OK ||
        julia_fsm_runtime_get_state() != JULIA_MAIN_STATE_S4_INTERACTION) {
        wss_transport_fail_session();
        cJSON_Delete(root);
        return true;
    }
    voice_service_start_wake_prompt();
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    s_round_pending=false;
#endif
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
    if (!CONFIG_JULIA_LOCAL_CAPTURE_ENABLE && julia_fsm_is_quiet(julia_fsm_runtime_get_state())) return;
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    if(len && text[0]=='{') {
        /* 严格的 WSS 封装必须在任何旧 JSON 解析器之前自检，否则超出长度上限、
         * 嵌套过深、含内嵌 NUL 或重复字段的载荷会被 cJSON 直接接受并绕过检查。 */
        cJSON *envelope=voice_control_parse((const char *)text,len);
        bool valid=cJSON_IsObject(envelope);cJSON_Delete(envelope);
        if(!valid){ESP_LOGW(TAG,"Ignoring malformed strict WSS envelope");return;}
    }
#endif
    if (voice_state_sync_handle_text(text, len)) return;
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    if (voice_local_capture_text(text, len)) return;
#endif
    if (!voice_state_sync_is_ready()) {
        ESP_LOGW(TAG, "Ignoring business command before session_sync_ack");
        return;
    }
    /* S0 允许握手和能力协商，但不能让播放/业务命令覆盖连接等待画面。 */
    if (julia_fsm_runtime_get_state() == JULIA_MAIN_STATE_S0_BOOT) return;
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    if (voice_service_handle_control_json(text,len)) return;
    if (s_busy_until_us) return;
#if !CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    bool mic_start=len==9 && !memcmp(text,"MIC_START",9);
    if (mic_start && (!s_round_pending || !s_round_speech)) {
        ESP_LOGW(TAG,"MIC_START ignored: round_sync_required");return;
    }
    if (s_round_pending && !mic_start && (len==0 || text[0]!='{')) return;
#else
    /* capture-v1 的语音轮次在 interaction_sync 时就绪；唤醒仍等待 wake_detected。 */
    if (s_round_pending && (len==0 || text[0]!='{')) return;
#endif
#endif
    ESP_LOGD(TAG, "Server command received (%u bytes)", (unsigned)len);
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
#if !CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
        voice_service_apply_mic_start();
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
        s_round_pending=false;
#endif
#endif
        return;
    }
    if (len == strlen("MIC_STOP") && memcmp(text, "MIC_STOP", len) == 0) {
#if !CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
        voice_service_apply_mic_stop();
#endif
        return;
    }

    /* 回答声音必须先用 SPKS 声明采样率和用途，再发送二进制声音，最后用 SPKE
     * 表示服务器已经发完。顺序错误的声音会被拒绝，避免未知数据进入扬声器。 */
    if (len == 4 && memcmp(text, "SPKE", 4) == 0) {
#if CONFIG_JULIA_VOICE_TIMING
        if (s_playback_generation && s_playback_role==VOICE_PLAYBACK_ROLE_DIALOG_REPLY)
            voice_timing_record(VT_SPKE,s_timing_epoch,0,s_playback_generation,esp_timer_get_time(),0,0);
#endif
        /* END 排在所有已接收 PCM 之后；播放任务负责报告完成。
         * 已被打断或不存在的播放代次无需执行结束动作。 */
        if (s_playback_generation != 0 &&
            !(CONFIG_JULIA_LOCAL_CAPTURE_ENABLE && s_playback_role == VOICE_PLAYBACK_ROLE_WAKE_REPLY))
            voice_playback_finish();
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
        int64_t received_us = esp_timer_get_time();
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
        if (!CONFIG_JULIA_LOCAL_CAPTURE_ENABLE &&
            state == JULIA_MAIN_STATE_S4_INTERACTION && s_wake_reply_expected) {
            role = VOICE_PLAYBACK_ROLE_WAKE_REPLY;
        } else if (state == JULIA_MAIN_STATE_S2_DIALOG &&
                   sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING) {
            role = VOICE_PLAYBACK_ROLE_DIALOG_REPLY;
        }
        if (end != buf && *end == '\0' && role != VOICE_PLAYBACK_ROLE_NONE &&
            voice_playback_start((uint32_t)rate, false, &s_playback_generation) == ESP_OK) {
            memset(&s_audio_timing, 0, sizeof(s_audio_timing));
            s_audio_timing.generation = s_playback_generation;
            s_audio_timing.spks_us = received_us;
            snprintf(s_audio_timing.session, sizeof(s_audio_timing.session), "%s",
                     voice_state_sync_session_id());
            snprintf(s_audio_timing.interaction, sizeof(s_audio_timing.interaction), "%s", s_interaction_id);
            s_playback_role = role;
#if CONFIG_JULIA_VOICE_TIMING
            voice_timing_record(VT_SPKS,s_timing_epoch,0,s_playback_generation,received_us,role,rate);
#endif
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
        ESP_LOGI(TAG, "SPKV ignored: fixed speaker volume=%d",
                 CONFIG_JULIA_SPEAKER_VOLUME_PERCENT);
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
    if (!CONFIG_JULIA_LOCAL_CAPTURE_ENABLE && julia_fsm_is_quiet(julia_fsm_runtime_get_state())) return;
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    if (s_round_pending || !s_control_guard.active || s_busy_until_us) return;
#endif
    int64_t received_us = esp_timer_get_time();
    if (!voice_state_sync_is_ready()) return;
    if (playback_role_is_terminal(s_playback_role) ||
        (CONFIG_JULIA_LOCAL_CAPTURE_ENABLE && s_playback_role == VOICE_PLAYBACK_ROLE_WAKE_REPLY)) return;
    if (data == NULL || len == 0U || (len & 1U) != 0U) {
        return;
    }
    if (!voice_playback_is_active()) {
        ESP_LOGW(TAG, "Dropping %u-byte downlink PCM: speaker not started", (unsigned)len);
        return;
    }
    esp_err_t err = voice_playback_write(data, len);
    if (err == ESP_OK && s_audio_timing.generation == s_playback_generation &&
        s_audio_timing.first_pcm_us == 0) {
        s_audio_timing.first_pcm_us = received_us;
#if CONFIG_JULIA_VOICE_TIMING
        voice_timing_record(VT_FIRST_PCM_RX,s_timing_epoch,0,s_playback_generation,received_us,len,0);
#endif
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Downlink PCM write failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_NO_MEM) {
            (void)voice_service_send_error("ERROR playback_overflow");
            wss_transport_fail_session();
        }
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
    if (!CONFIG_JULIA_LOCAL_CAPTURE_ENABLE && julia_fsm_is_quiet(julia_fsm_runtime_get_state())) return;
    if (!voice_state_sync_is_ready()) return;
    if (item == NULL || item_size != sizeof(voice_job_t)) {
        ESP_LOGW(TAG, "Malformed queued voice job");
        return;
    }
    voice_job_t *job = (voice_job_t *)item;
    switch (job->type) {
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    case VOICE_JOB_SCOPED_CONTROL:
        voice_service_apply_scoped_control(job->data, job->len);
        break;
#endif
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
    s_rx_backpressure_since_us = 0;
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    voice_local_capture_connection(0);
#endif
    memset(&s_audio_timing, 0, sizeof(s_audio_timing));
    portENTER_CRITICAL(&s_mic_state_lock);
    s_last_capture_us = 0;
    portEXIT_CRITICAL(&s_mic_state_lock);
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    voice_control_guard_reset(&s_control_guard);
    s_control_guard.active=true;
    s_round_pending=false;s_round_ack[0]=0;s_round_request[0]=0;s_busy_until_us=0;
#endif
    s_session_activated = false;
    /* 新 socket 一律先回到服务器唤醒模式，与 MQTT 是否可用无关；必须在该状态
     * 提交成功后才允许接受新的语音命令。 */
    if (julia_fsm_runtime_post_sync(EVT_VOICE_SESSION_RESET) != ESP_OK) {
        wss_transport_fail_session();
        return;
    }
    uint32_t generation = voice_service_next_uplink_generation();
#if CONFIG_JULIA_VOICE_TIMING
    s_timing_epoch=generation;s_timing_tx_id=s_timing_pcm_frames=0;
    voice_timing_record(VT_SESSION_START,generation,0,0,esp_timer_get_time(),0,0);
#endif
    if (!s_uplink_ring_ready ||
        !voice_uplink_ring_start_generation(&s_uplink_ring, generation)) {
        ESP_LOGE(TAG, "Cannot start MIC uplink generation=%" PRIu32, generation);
        wss_transport_fail_session();
        return;
    }
    voice_uplink_pump_start_generation(&s_uplink_pump, generation);
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    voice_local_capture_connection(generation);
#endif
#if CONFIG_JULIA_SERVER_WAKE_ENABLE
    #if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    voice_local_capture_connection(generation);
    #endif
    portENTER_CRITICAL(&s_mic_state_lock);
    s_mic_streaming = false;
    s_dialog_listening = false;
    s_s4_ready_pending = false;
    s_s4_ready_committed = false;
    s_interaction_id[0] = '\0';
    board_audio_enable_wss_mic(false);
    portEXIT_CRITICAL(&s_mic_state_lock);

    /* 后台唤醒监听只属于传输层；服务端发送 wake_detected 前，
     * 保持当前主状态与呈现不变。 */
    ESP_LOGI(TAG, "WSS session ready: generation=%" PRIu32
                  " waiting for cloud state synchronization",
             generation);
#else
    ESP_LOGI(TAG, "WSS session ready: generation=%" PRIu32, generation);
#endif
    voice_state_sync_start();
#if !CONFIG_JULIA_CLOUD_STATE_SYNC_ENABLE
    post_fsm_event(EVT_WSS_CONNECTED);
#endif
}

/**
 * 一条 WSS generation 同时拥有 MIC ring、播放角色和文件区间。结束通知必须先清空
 * 这些会话资源再投递 FSM 事件；否则重连可能把旧话语或旧完成结果带入新会话。
 */
static void voice_service_on_session_end(wss_transport_end_reason_t reason)
{
    s_rx_backpressure_since_us = 0;
#if CONFIG_JULIA_VOICE_TIMING
    voice_timing_record(VT_SESSION_END,s_timing_epoch,0,0,esp_timer_get_time(),reason,0);
#endif
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    voice_local_capture_connection(0);
#endif
    voice_state_sync_end();
    s_session_activated = false;
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
    s_reply_deadline_us = 0;
    s_listen_deadline_us = 0;
    s_interaction_id[0] = '\0';
    board_audio_enable_wss_mic(false);
    portEXIT_CRITICAL(&s_mic_state_lock);
    voice_service_close_file();
    /* 传输层断链不取消 FSM 自己持有的 OTA 提示，因此这里只按代次停当前播放。 */
    bool stopped = voice_playback_stop_generation(s_playback_generation);
    s_playback_generation = 0;
    s_playback_role = VOICE_PLAYBACK_ROLE_NONE;
    if (stopped || !voice_playback_is_active()) julia_avatar_talking_stop();
    ESP_LOGI(TAG, "WSS uplink generation=%" PRIu32
                  " ended reason=%s; discarded MIC ring frames=%u",
             s_uplink_generation, wss_transport_end_reason_name(reason),
             (unsigned)discarded_frames);
    julia_fsm_runtime_wss_disconnected(wss_transport_generation());
    /* 断线不是用户主动交流，只结束“正在处理”的标记，不能因此点亮睡眠中的屏幕。 */
    julia_idle_display_set_busy(false);
}

/* data 在返回前完成复制，len 为 0 时允许为 NULL。成功只表示控制队列已接收，
 * 不表示 WSS 已发送或业务状态已经生效。 */
static esp_err_t voice_service_enqueue(voice_job_type_t type, const uint8_t *data, size_t len)
{
    if (julia_fsm_is_quiet(julia_fsm_runtime_get_state())) return ESP_ERR_INVALID_STATE;
    if (!voice_state_sync_is_ready()) return ESP_ERR_INVALID_STATE;
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
    (void)ctx;
    julia_quiet_power_notify();
    julia_night_schedule_notify();
    julia_motion_notify();
    julia_avatar_set_suspended(main_state == JULIA_MAIN_STATE_S6_SLEEP);
    voice_playback_set_interaction(main_state == JULIA_MAIN_STATE_S4_INTERACTION ||
                                   main_state == JULIA_MAIN_STATE_S2_DIALOG);
    if (!CONFIG_JULIA_LOCAL_CAPTURE_ENABLE && julia_fsm_is_quiet(main_state)) {
        board_audio_mic_set_enabled(false);
        board_audio_enable_wss_mic(false);
    }
    portENTER_CRITICAL(&s_mic_state_lock);
    if (main_state != JULIA_MAIN_STATE_S4_INTERACTION) {
        s_s4_ready_pending = false;
        s_s4_ready_committed = false;
        s_wake_reply_expected = false;
    }
    s_reply_deadline_us =
        main_state == JULIA_MAIN_STATE_S2_DIALOG &&
        s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING
            ? esp_timer_get_time() +
                (int64_t)CONFIG_JULIA_DIALOG_REPLY_TIMEOUT_SECONDS * 1000000LL
            : 0;
    s_listen_deadline_us =
        main_state == JULIA_MAIN_STATE_S4_INTERACTION ||
        (main_state == JULIA_MAIN_STATE_S2_DIALOG &&
         s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING)
            ? esp_timer_get_time() +
                (int64_t)CONFIG_JULIA_DIALOG_LISTEN_TIMEOUT_SECONDS * 1000000LL
            : 0;
    portEXIT_CRITICAL(&s_mic_state_lock);
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
    if (playback_role_is_terminal(s_playback_role)) return;
    ESP_LOGI(TAG, "Terminal intent=%s received in %s/%s", intent,
             julia_fsm_main_state_name(state),
             julia_fsm_s2_sub_state_name(julia_fsm_runtime_get_s2_sub_state()));

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
    if (state == JULIA_MAIN_STATE_S2_DIALOG) {
        /* MQTT 语义可能晚于 WSS MIC_STOP；等待 S4 呈现提交后再启动声音和嘴型。 */
        esp_err_t err = julia_fsm_runtime_post_sync(EVT_PREPARE_TERMINAL_REPLY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Terminal prompt=%s could not enter S4: %s", intent,
                     esp_err_to_name(err));
            post_fsm_event(event);
            return;
        }
        state = julia_fsm_runtime_get_state();
    }
    if (state == JULIA_MAIN_STATE_S4_INTERACTION) {
        bool goodnight = event == EVT_INTENT_GOODNIGHT;
        const uint8_t *wav = goodnight ? goodnight_wav_start : bye_no_bother_wav_start;
        const uint8_t *wav_end = goodnight ? goodnight_wav_end : bye_no_bother_wav_end;
        julia_avatar_talking_start();
        esp_err_t err = voice_playback_start_local_wav(
            wav, (size_t)(wav_end - wav), false, &s_playback_generation);
        if (err == ESP_OK) {
            s_playback_role = goodnight ? VOICE_PLAYBACK_ROLE_GOODNIGHT_REPLY
                                        : VOICE_PLAYBACK_ROLE_DISMISS_REPLY;
            julia_idle_display_set_busy(true);
            ESP_LOGI(TAG, "Terminal prompt=%s playing in S4 generation=%" PRIu32,
                     intent, s_playback_generation);
            return;
        }
        julia_avatar_talking_stop();
        ESP_LOGW(TAG, "Terminal prompt=%s start failed: %s", intent, esp_err_to_name(err));
    }
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
#if !CONFIG_JULIA_MULTI_DEVICE_ENABLE
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
#endif

#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
static bool voice_service_handle_control_json(const uint8_t *data, size_t len)
{
    if (!len || data[0]!='{') return false;
    cJSON *root=voice_control_parse((const char *)data,len);
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(root,"type");
    bool sync=cJSON_IsString(type) && !strcmp(type->valuestring,"interaction_sync");
    bool busy=cJSON_IsString(type) && !strcmp(type->valuestring,"busy");
    if (!sync && !busy) {cJSON_Delete(root);return false;}
    const char *error=voice_control_guard_check(&s_control_guard,root,s_voice_device_id,
                                               voice_state_sync_session_id(),NULL);
    if(error){ESP_LOGW(TAG,"WSS control ignored: %s",error);cJSON_Delete(root);return true;}
    if(busy) {
        if(playback_role_is_terminal(s_playback_role)) {cJSON_Delete(root);return true;}
        uint32_t busy_round;
        const cJSON *busy_id=cJSON_GetObjectItemCaseSensitive(root,"interaction_id");
        if(!voice_control_uint(root,"interaction_seq",&busy_round) || busy_round!=s_control_guard.interaction_seq ||
           strcmp(busy_id->valuestring,s_control_guard.interaction_id)) {
            ESP_LOGW(TAG,"Ignoring stale busy notification");cJSON_Delete(root);return true;
        }
        const cJSON *code=cJSON_GetObjectItemCaseSensitive(root,"code");
        uint32_t delay;
        if(cJSON_IsString(code) && !strcmp(code->valuestring,"voice_capacity") &&
           voice_control_uint(root,"retry_after_ms",&delay) && delay>=1000 && delay<=30000) {
            /* 资源拒绝不等于断链：不伪造轮次结果、不重放旧命令，只结束本轮实际对话，
             * 冷却到期后由 voice_service_busy_wait() 换新代次重开采集。 */
            s_busy_until_us=esp_timer_get_time()+(int64_t)delay*1000;
            s_control_guard.active=false;s_round_pending=false;
            voice_playback_stop_generation(s_playback_generation);s_playback_generation=0;
            s_playback_role=VOICE_PLAYBACK_ROLE_NONE;s_audio_timing.generation=0;
            voice_service_pause_uplink_for_file();
            portENTER_CRITICAL(&s_mic_state_lock);
            s_dialog_listening=false;s_reply_deadline_us=0;s_listen_deadline_us=0;
            portEXIT_CRITICAL(&s_mic_state_lock);
            julia_main_state_t state=julia_fsm_runtime_get_state();
            if(state==JULIA_MAIN_STATE_S1_COMPANION || state==JULIA_MAIN_STATE_S2_DIALOG ||
               state==JULIA_MAIN_STATE_S4_INTERACTION) post_fsm_event(EVT_VOICE_BUSY);
            julia_avatar_talking_stop();julia_idle_display_set_busy(false);
            ESP_LOGW(TAG,"Voice resource busy; fresh capture resumes after %lu ms",(unsigned long)delay);
        }
        cJSON_Delete(root);return true;
    }
    const char *id=cJSON_GetObjectItemCaseSensitive(root,"interaction_id")->valuestring;
    const char *request=cJSON_GetObjectItemCaseSensitive(root,"request_id")->valuestring;
    const cJSON *purpose=cJSON_GetObjectItemCaseSensitive(root,"purpose");
    uint32_t seq=0;
    bool speech=cJSON_IsString(purpose) && !strcmp(purpose->valuestring,"speech");
    bool wake=cJSON_IsString(purpose) && !strcmp(purpose->valuestring,"wake");
    if(!voice_control_uint(root,"interaction_seq",&seq) || (!speech && !wake)) error="invalid_envelope";
    else if(seq==s_control_guard.interaction_seq && !strcmp(id,s_control_guard.interaction_id) &&
            !strcmp(request,s_round_request) && s_control_guard.active && s_round_speech==speech) {
        (void)wss_transport_send_now(1,(const uint8_t *)s_round_ack,strlen(s_round_ack));
        cJSON_Delete(root);return true;
    } else if(playback_role_is_terminal(s_playback_role)) error="terminal_reply";
    else if(s_busy_until_us) error="busy";
    else {
        julia_main_state_t state=julia_fsm_runtime_get_state();
        julia_s2_sub_state_t sub=julia_fsm_runtime_get_s2_sub_state();
        bool allowed=wake ? (state==JULIA_MAIN_STATE_S1_COMPANION || state==JULIA_MAIN_STATE_S3_STANDBY ||
                            state==JULIA_MAIN_STATE_S5_SILENT || state==JULIA_MAIN_STATE_S6_SLEEP) :
                            (state==JULIA_MAIN_STATE_S1_COMPANION || state==JULIA_MAIN_STATE_S4_INTERACTION ||
                             (state==JULIA_MAIN_STATE_S2_DIALOG &&
                              (sub==JULIA_S2_SUB_STATE_S2_1_LISTENING || sub==JULIA_S2_SUB_STATE_S2_3_SPEAKING ||
                               (CONFIG_JULIA_LOCAL_CAPTURE_ENABLE && voice_local_capture_ready() &&
                                sub==JULIA_S2_SUB_STATE_S2_2_THINKING))));
        error=allowed ? voice_control_begin(&s_control_guard,seq,id) : "state_unavailable";
        if(error && !strcmp(error,"duplicate_round")) error="request_id_conflict";
    }
    char ack[512];
    int n=snprintf(ack,sizeof(ack),"{\"type\":\"interaction_sync_ack\",\"device_id\":\"%s\","
        "\"session_id\":\"%s\",\"interaction_id\":\"%s\",\"request_id\":\"%s\","
        "\"interaction_seq\":%lu,\"accepted\":%s,\"code\":\"%s\",\"device_time_ms\":%" PRIi64 "}",
        s_voice_device_id,voice_state_sync_session_id(),id,request,(unsigned long)seq,
        error?"false":"true",error?error:"applied",esp_timer_get_time()/1000LL);
    if(!error) {
        /* 本地收音不再等待旧 MIC_START；wake 仍由 wake_detected 完成状态迁移。 */
        s_round_pending=!(CONFIG_JULIA_LOCAL_CAPTURE_ENABLE && speech);
        s_round_speech=speech;
        portENTER_CRITICAL(&s_mic_state_lock);
        snprintf(s_interaction_id,sizeof(s_interaction_id),"%s",id);
        portEXIT_CRITICAL(&s_mic_state_lock);
        snprintf(s_round_request,sizeof(s_round_request),"%s",request);
        if(n>0 && (size_t)n<sizeof(ack)) strcpy(s_round_ack,ack);
    }
    if(n>0 && (size_t)n<sizeof(ack) &&
       wss_transport_send_now(1,(const uint8_t *)ack,(size_t)n)!=ESP_OK) wss_transport_fail_session();
    cJSON_Delete(root);return true;
}

static void voice_service_apply_scoped_control(const uint8_t *data, size_t len)
{
    cJSON *root=voice_control_parse((const char *)data,len);
    const char *basic=voice_control_guard_check(&s_control_guard,root,s_voice_device_id,
                                               voice_state_sync_session_id(),NULL);
    if(basic){ESP_LOGW(TAG,"Scoped control ignored: %s",basic);cJSON_Delete(root);return;}
    const char *cached=NULL;
    const char *error=voice_control_evaluate(&s_control_guard,root,s_voice_device_id,
        voice_state_sync_session_id(),esp_timer_get_time()/1000LL,&cached);
    if(cached){(void)mqtt_comm_publish_voice_status(s_voice_device_id,cached,strlen(cached));cJSON_Delete(root);return;}
    bool record=!error || !strcmp(error,"expired") || !strcmp(error,"invalid_deadline");
    bool accepted=false;
    if(!error) {
        const char *type=cJSON_GetObjectItemCaseSensitive(root,"type")->valuestring;
        julia_main_state_t state=julia_fsm_runtime_get_state();
        bool terminal=playback_role_is_terminal(s_playback_role);
        if(!strcmp(type,"intent_result")) {
            const char *intent=cJSON_GetObjectItemCaseSensitive(root,"intent")->valuestring;
            if(!strcmp(intent,"normal")){accepted=true;}
            else if(strcmp(intent,"goodnight") && strcmp(intent,"dismiss")) error="unsupported_operation";
            else if(terminal) error="terminal_reply";
            else if(state!=JULIA_MAIN_STATE_S4_INTERACTION && state!=JULIA_MAIN_STATE_S2_DIALOG) error="state_unavailable";
            else {
                bool goodnight=!strcmp(intent,"goodnight");
                voice_service_apply_terminal_intent(goodnight?EVT_INTENT_GOODNIGHT:EVT_INTENT_DISMISS,intent);
                julia_main_state_t after=julia_fsm_runtime_get_state();
                accepted=playback_role_is_terminal(s_playback_role) ||
                    after==(goodnight?JULIA_MAIN_STATE_S6_SLEEP:JULIA_MAIN_STATE_S5_SILENT);
            }
        } else if(!strcmp(type,"command")) {
            const char *command=cJSON_GetObjectItemCaseSensitive(root,"command")->valuestring;
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
            if(!strcmp(command,"MIC_START") || !strcmp(command,"MIC_STOP")) {
                error="unsupported_operation";
            } else
#else
            if(!strcmp(command,"MIC_START")) {
                if(terminal)error="terminal_reply";
                else if(!s_round_pending || !s_round_speech)error="round_sync_required";
                else {voice_service_apply_mic_start();s_round_pending=false;
                      accepted=s_dialog_listening;}
            } else if(!strcmp(command,"MIC_STOP")) {
                bool listening=s_dialog_listening;
                voice_service_apply_mic_stop();
                accepted=!listening || (julia_fsm_runtime_get_state()==JULIA_MAIN_STATE_S2_DIALOG &&
                    julia_fsm_runtime_get_s2_sub_state()==JULIA_S2_SUB_STATE_S2_2_THINKING);
            }
            else
#endif
            if(!strncmp(command,"FILE_SEND ",10)) {
                if(voice_playback_is_active() || s_dialog_listening || s_file)error="state_unavailable";
                else {esp_err_t err=voice_service_push_file(command+10,true);accepted=err==ESP_OK && s_file!=NULL;}
            } else error="unsupported_operation";
        } else error="unsupported_operation";
    }
    uint32_t seq,round;
    if(!voice_control_uint(root,"control_seq",&seq) || !voice_control_uint(root,"interaction_seq",&round)) {
        ESP_LOGW(TAG,"Scoped control ignored: invalid sequence");cJSON_Delete(root);return;
    }
    julia_fsm_snapshot_t state;julia_fsm_runtime_get_snapshot(&state);
    char ack[512];
    int n=snprintf(ack,sizeof(ack),"{\"type\":\"control_ack\",\"device_id\":\"%s\","
        "\"session_id\":\"%s\",\"interaction_id\":\"%s\",\"request_id\":\"%s\","
        "\"interaction_seq\":%lu,\"control_seq\":%lu,\"accepted\":%s,\"code\":\"%s\",\"state_revision\":%lu}",
        s_voice_device_id,voice_state_sync_session_id(),cJSON_GetObjectItemCaseSensitive(root,"interaction_id")->valuestring,
        cJSON_GetObjectItemCaseSensitive(root,"request_id")->valuestring,(unsigned long)round,(unsigned long)seq,
        accepted?"true":"false",error?error:accepted?"applied":"state_unavailable",(unsigned long)state.revision);
    if(n>0 && (size_t)n<sizeof(ack)) {
        /* record 返回 false 表示结果无法保存。此时既不能补发未记录的 ACK，也不能让
         * 序号水位前进，只能结束会话，让服务器在重连后按新的会话重新发起控制。 */
        if(record && !voice_control_record(&s_control_guard,root,ack)) wss_transport_fail_session();
        (void)mqtt_comm_publish_voice_status(s_voice_device_id,ack,(size_t)n);
    }
    cJSON_Delete(root);
}
#endif

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
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    if (!cmd || !cmd_len || cmd_len > VOICE_SCOPED_CONTROL_MAX_LEN) return;
    esp_err_t err = voice_service_enqueue(VOICE_JOB_SCOPED_CONTROL,
                                          (const uint8_t *)cmd, cmd_len);
    if (err != ESP_OK) ESP_LOGW(TAG, "Scoped control not queued: %s", esp_err_to_name(err));
#else
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
    ESP_LOGD(TAG, "Voice command received (%u bytes)", (unsigned)cmd_len);

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
        ESP_LOGW(TAG, "Ignoring unknown voice command");
    }
#endif
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
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    ESP_RETURN_ON_ERROR(native_ota_get_device_id(s_voice_device_id, sizeof(s_voice_device_id)),
                        TAG, "stable voice identity unavailable");
    char topic[96];
    int n = snprintf(topic, sizeof(topic), "voice/%s/vcmd", s_voice_device_id);
    if (n <= 0 || (size_t)n >= sizeof(topic)) return ESP_ERR_INVALID_SIZE;
    return mqtt_comm_register_topic(topic, VOICE_SCOPED_CONTROL_MAX_LEN, true,
                                    voice_service_on_mqtt_command);
#else
    /* 语音 topic 非 critical：语音订阅失败不影响 OTA 连接就绪判定。 */
    return mqtt_comm_register_topic(CONFIG_COMM_MQTT_VOICE_CMD_TOPIC,
                                    VOICE_SERVICE_CMD_MAX_LEN, false,
                                    voice_service_on_mqtt_command);
#endif
}

static esp_err_t voice_service_send_capture_record(const uint8_t *, size_t, uint32_t);

static void voice_service_local_capture_event(lc_event_t event, lc_mode_t mode,
                                              uint32_t listen_revision)
{
    /* 本函数由采音任务在产出 LC_START/LC_END 记录后回调，运行在采音任务上下文；
     * LC_START 等价于云端 MIC_START：先停掉正在播放的声音，再把本轮标记为“正在处理”。 */
    if (mode != LC_DIALOG) return;
    if (event == LC_IDLE_TIMEOUT) {
        if (julia_fsm_runtime_listen_idle_timeout(listen_revision) != ESP_OK)
            wss_transport_fail_session();
        return;
    }
    if (event == LC_START) voice_playback_stop();
    julia_idle_display_note_activity();
    julia_idle_display_set_busy(true);
    portENTER_CRITICAL(&s_mic_state_lock);
    s_dialog_listening = event == LC_START;
    if (event == LC_START) s_wake_reply_expected = false;
    portEXIT_CRITICAL(&s_mic_state_lock);
    if (julia_fsm_runtime_post(event == LC_START ? EVT_LOCAL_SPEECH_START :
                                EVT_START_DIALOG) != ESP_OK) wss_transport_fail_session();
}

esp_err_t voice_service_init_board_audio(void)
{
#if CONFIG_JULIA_VOICE_TIMING
    static bool trace_initialized;
    if (!trace_initialized) { voice_timing_init(esp_random()); trace_initialized=true; }
#endif
    ESP_RETURN_ON_ERROR(voice_service_uplink_ring_init(), TAG,
                        "init MIC uplink ring");
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    ESP_RETURN_ON_ERROR(voice_local_capture_init(voice_service_send_capture_record,
                        voice_service_local_capture_event), TAG, "init local capture");
#endif
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
    wss_transport_set_paused(false);
    static const wss_transport_config_t transport_cfg = {
        .on_text = voice_service_on_server_text,
        .on_binary = voice_service_on_binary,
        .on_queue_item = voice_service_on_queue_item,
        .on_session_start = voice_service_on_session_start,
        .on_session_end = voice_service_on_session_end,
        .on_poll = voice_service_poll,
        .can_receive = voice_service_can_receive,
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
    return voice_service_send_capture_record(buf, len, 0);
}

/* generation 必须由采集侧在采集当时取得并原样携带；ring 收到不匹配的值会按
 * INACTIVE 拒绝，禁止把旧预录标成刚重连的新连接数据。0 是保留值，表示“不校验代次”，
 * 只用于 voice_service_send_chunk() 这条兼容路径。 */
static esp_err_t voice_service_send_capture_record(const uint8_t *buf, size_t len, uint32_t generation)
{
    if (!voice_state_sync_is_ready()) return ESP_ERR_INVALID_STATE;
    if (buf == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > VOICE_UPLINK_FRAME_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_uplink_ring_ready) return ESP_ERR_INVALID_STATE;
    voice_uplink_push_result_t result = voice_uplink_ring_push_generation(
        &s_uplink_ring, buf, len, generation);
    switch (result) {
    case VOICE_UPLINK_PUSH_OK:
        portENTER_CRITICAL(&s_mic_state_lock);
        s_last_capture_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_mic_state_lock);
        return ESP_OK;
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
    if (julia_fsm_runtime_get_state() == JULIA_MAIN_STATE_S0_BOOT ||
        julia_fsm_runtime_get_service_state() != JULIA_SERVICE_ONLINE ||
        !mqtt_comm_is_ready() || !wss_transport_is_ready() || !voice_state_sync_is_ready())
        return ESP_ERR_INVALID_STATE;
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
