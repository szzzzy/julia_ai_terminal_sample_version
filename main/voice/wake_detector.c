/*
 * wake_detector.c - 本地唤醒词检测（WakeNet "你好小智"，wn9_nihaoxiaozhi_tts）
 *
 * 来源：fused 工程 julia_voice.c 的 AFE/WakeNet 初始化与 feed/detect 任务，
 * 裁剪为纯"唤醒检测"形态：
 *   - 只做本地唤醒（不接 ASR/LLM/TTS/FSM——那些在服务器侧或后续接入）；
 *   - 检测到唤醒词 → 调 voice_service_mic_start()（开麦推 PCM1 流），
 *     服务器收到后负责 ASR/LLM/TTS，回推 PCM 由 voice_service 播报。
 *
 * 与最小包/board_audio 的衔接：mic_task 的 afe_sink fanout（路径 1）在
 * board_audio_set_afe_sink() 后每 20 ms 回调一次，本模块把它喂给 AFE。
 *
 * 数据流：
 *   mic_task(20ms/320样本) --wake_afe_feed--> 内部拼接缓冲 --s_afe->feed-->
 *   AFE --s_afe->fetch--> wake_detect_task 判唤醒词
 *   --> voice_service_mic_start()（把后续 MIC 流交给服务器）。
 *
 * 线程模型：
 * - wake_afe_feed 运行在 board_audio 的 mic_task 上下文（必须轻量、非阻塞）；
 * - wake_detect_task 常驻 core1/p5（高优先级 5），从 AFE fetch 结果；
 * - s_ready / s_wake_cooldown_until_us 跨任务共享：s_ready 只在 init 写、
 *   其余只读；冷却时间戳仅由 detect 任务读写。
 * - 本模块不持有锁，靠"单生产者（mic_task 喂数）+ 单消费者（detect 拉结果）"
 *   的 AFE 内部队列做解耦。
 */

#include "wake_detector.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_iface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"

#include "board_audio.h"
#include "julia_idle_display.h"
#include "voice_service.h"

#define TAG "WAKE"

/* 唤醒词模型：与 fused wake_word_config.h 一致（量产模型 wn9_nihaoxiaozhi_tts）。 */
#define WAKE_WORD_MODEL_NAME "wn9_nihaoxiaozhi_tts"
#define WAKE_WORD_DISPLAY_TEXT "你好小智"
#define MODEL_PARTITION "model"
/* 两次唤醒之间的最小间隔：屏蔽自己播放/回声导致的二次触发。 */
#define WAKE_COOLDOWN_US 2000000LL

static const esp_afe_sr_iface_t *s_afe;       /* AFE 接口句柄（ESP-SR 静态表）。 */
static esp_afe_sr_data_t *s_afe_data;         /* AFE 运行时实例（含 WakeNet/VAD）。 */
/** AFE 要求的单通道 feed 块大小（由运行时接口查询，不能假定为 20 ms）。 */
static size_t s_feed_chunk_samples;
/** 将板级 20 ms（320 sample）帧拼成 AFE 所要求大小的缓冲区。 */
static size_t s_feed_samples;
static int16_t *s_feed_buffer;                /* 拼接缓冲（内部 SRAM，8bit）。 */
static volatile bool s_ready;                 /* init 完成后置 true，供查询。 */
static int64_t s_wake_cooldown_until_us;      /* 下一次允许唤醒的时间戳（esp_timer us）。 */

/**
 * @brief board_audio 的 AFE sink 回调：凑满一个 AFE feed 块后送入，否则暂存。
 *
 * 运行在 board_audio mic_task 上下文（每 20ms 一帧）。板级帧是 320 样本/20ms，
 * 而 AFE 要求的 feed 块（get_feed_chunksize）往往不是 320 的整数倍/相同值，
 * 因此用 s_feed_buffer 做拼接：每次尽可能填满整块，填满即喂给 AFE 并清零。
 *
 * @param[in] pcm    20ms mono PCM16。
 * @param[in] samples 样本数（通常 320）。
 * @param[in] ctx    未使用。
 *
 * @note 必须在 mic_task 上下文，要求快速返回（纯 memcpy + 偶发 AFE feed）。
 *       任何一次性参数异常（实例/缓冲未建好、空指针、0 样本）都直接丢弃，
 *       不在回调里做重试或记录（避免给 mic_task 引入额外开销）。
 */
static void wake_afe_feed(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    /* 初始化未完成或参数异常时静默丢弃本帧，不阻塞 mic_task。 */
    if (s_afe_data == NULL || s_feed_buffer == NULL || pcm == NULL || samples == 0) {
        return;
    }
    while (samples > 0U) {
        size_t copy = s_feed_chunk_samples - s_feed_samples;
        if (copy > samples) {
            copy = samples;
        }
        memcpy(s_feed_buffer + s_feed_samples, pcm, copy * sizeof(*pcm));
        s_feed_samples += copy;
        pcm += copy;
        samples -= copy;
        /* 拼满一块就交给 AFE，并清空拼接计数，等待下一块。 */
        if (s_feed_samples == s_feed_chunk_samples) {
            (void)s_afe->feed(s_afe_data, s_feed_buffer);
            s_feed_samples = 0;
        }
    }
}

/**
 * @brief 唤醒检测任务：从 AFE fetch 结果中找 WakeNet 事件并触发开麦。
 *
 * 常驻 core1/p5。每轮 fetch 一次；命中唤醒词且已过冷却窗口则：
 *   1) 记录下次允许唤醒时间（防回声/自身音频二次触发）；
 *   2) 记一次显示活动、置 busy（保持屏亮）；
 *   3) 调用 voice_service_mic_start() 等效于服务器下发 MIC_START，把麦克风
 *      流推给服务器（之后的 ASR/LLM/TTS 由服务器完成）；
 *   4) 清 AFE 识别窗口，避免残余结果重复触发。
 * mic_start 失败时回退 busy 状态并记日志（但检测器仍可继续工作）。
 *
 * @param[in] arg 未使用。
 *
 * @note 本任务只读 AFE、写 busy/冷却时间戳；不改 s_ready（init 后固定 true）。
 */
static void wake_detect_task(void *arg)
{
    (void)arg;
    while (true) {
        afe_fetch_result_t *result = s_afe->fetch(s_afe_data);
        if (result == NULL || result->ret_value == ESP_FAIL) {
            continue;
        }
        int64_t now_us = esp_timer_get_time();
        /* 冷却判定：避免唤醒词-播放-回声形成无限自触发循环。 */
        if (result->wakeup_state == WAKENET_DETECTED &&
            result->vad_state == AFE_VAD_SPEECH &&
            now_us >= s_wake_cooldown_until_us) {
            s_wake_cooldown_until_us = now_us + WAKE_COOLDOWN_US;
            ESP_LOGI(TAG, "Wake word detected [%s] vad=speech volume=%.1fdB",
                     WAKE_WORD_DISPLAY_TEXT, (double)result->data_volume);
            julia_idle_display_note_activity();
            julia_idle_display_set_busy(true);
            /* 形态 1：本地唤醒 → 自动开麦推流，服务器负责 ASR/LLM/TTS。 */
            esp_err_t err = voice_service_mic_start();
            if (err != ESP_OK) {
                julia_idle_display_set_busy(false);
                ESP_LOGW(TAG, "MIC start after wake failed: %s", esp_err_to_name(err));
            }
            /* 清除识别窗口，防止同一次唤醒的残留结果再次触发。 */
            (void)s_afe->reset_buffer(s_afe_data);
        }
    }
}

/**
 * @brief 初始化本地唤醒词检测（app 装配阶段调用一次）。
 *
 * 顺序：初始化 AFE/WakeNet -> 校验输入布局(1 声道/正块长) -> 分配 feed 拼接
 * 缓冲 -> 挂板级 AFE sink -> 创建 wake_detect_task -> 置 s_ready。任何失败
 * 都会按序释放已创建的资源（if (s_afe_data) s_afe->destroy）后返回错误码。
 *
 * @return ESP_OK 就绪；ESP_ERR_INVALID_STATE CONFIG_USE_WAKENET 未启用；
 *         ESP_ERR_NOT_FOUND "model" 分区或 WakeNet 模型无；
 *         ESP_ERR_NO_MEM AFE/缓冲/任务创建失败；
 *         ESP_ERR_INVALID_STATE AFE 输入布局异常。
 *
 * @note 须在 board_audio_init() 与 voice_service_init() 之后调用（依赖二者
 *       提供的 AFE sink 与 mic_start 通道）。AFE 实例依赖 PSRAM，见下方注释。
 */
esp_err_t wake_detector_init(void)
{
    ESP_RETURN_ON_FALSE(CONFIG_USE_WAKENET, ESP_ERR_INVALID_STATE, TAG,
                        "CONFIG_USE_WAKENET is not enabled");
    srmodel_list_t *models = esp_srmodel_init(MODEL_PARTITION);
    ESP_RETURN_ON_FALSE(models, ESP_ERR_NOT_FOUND, TAG,
                        "speech model partition unavailable (flash 'model' partition?)");
    char *wake_model = esp_srmodel_filter(models, ESP_WN_PREFIX, WAKE_WORD_MODEL_NAME);
    ESP_RETURN_ON_FALSE(wake_model, ESP_ERR_NOT_FOUND, TAG, "WakeNet model unavailable");

    /* AFE 配置：单麦、无回声(AEC)/无分离(SE)，只保留 WakeNet + VAD。
     * 实机上 VAD mode 2/3 会漏掉正常唤醒词，因此 VAD 保持最宽松的 mode 0；
     * WakeNet 仍使用 normal DET_MODE_90，并在检测任务中要求二者同时命中，
     * 比旧的 VAD_MODE_0 + aggressive DET_MODE_95 更能抑制环境噪声误唤醒。 */
    afe_config_t config = AFE_CONFIG_DEFAULT();
    config.aec_init = false; config.se_init = false;
    config.vad_init = true; config.wakenet_init = true;
    config.vad_mode = VAD_MODE_0;
    config.wakenet_model_name = wake_model;
    config.afe_ringbuf_size = 50;
    config.wakenet_mode = DET_MODE_90;
    config.pcm_config.total_ch_num = 1;
    config.pcm_config.mic_num = 1;
    config.pcm_config.ref_num = 0;
    s_afe = &ESP_AFE_SR_HANDLE;

    /* AFE 创建依赖 PSRAM（WakeNet9l ~3.9 MiB 模型，内部 RAM 放不下）。
     * sdkconfig 必须启用 CONFIG_SPIRAM（Octal 8 MiB / 80 MHz）且 CPU=240MHz；
     * memory_alloc_mode 保持 AFE 默认（PSRAM 分配），不再回退到内部 RAM。
     * 若 PSRAM 未启用，AFE 会因 MALLOC_CAP_SPIRAM 不可用而失败（启动早期崩溃）。 */
    s_afe_data = s_afe->create_from_config(&config);
    ESP_RETURN_ON_FALSE(s_afe_data, ESP_ERR_NO_MEM, TAG, "create AFE failed");

    int channels = s_afe->get_total_channel_num(s_afe_data);
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    if (channels != 1 || chunk <= 0) {
        ESP_LOGE(TAG, "Unexpected AFE input layout: channels=%d chunk=%d", channels, chunk);
        s_afe->destroy(s_afe_data);
        s_afe_data = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_feed_chunk_samples = (size_t)chunk;
    s_feed_buffer = heap_caps_malloc(s_feed_chunk_samples * sizeof(*s_feed_buffer),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_feed_buffer == NULL) {
        s_afe->destroy(s_afe_data);
        s_afe_data = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 挂板级麦克风 sink：mic_task 每帧回调 wake_afe_feed。
     * 注意：AFE feed 在 mic_task 侧执行（CPU 0），而检测任务放到 core1，
     * 让"喂数"与"判定"分核，避免挤占同一核心。 */
    ESP_RETURN_ON_ERROR(board_audio_set_afe_sink(wake_afe_feed, NULL), TAG, "set AFE sink");

    /* 创建唤醒检测任务（core1/p5，与 fused 一致）。失败则返回，不置 s_ready。 */
    if (xTaskCreatePinnedToCore(wake_detect_task, "wake_detect", 6144, NULL, 5,
                                NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "ready model=%s wake_word=%s afe_chunk=%u",
             wake_model, WAKE_WORD_DISPLAY_TEXT, (unsigned)s_feed_chunk_samples);
    return ESP_OK;
}

bool wake_detector_is_ready(void) { return s_ready; }
