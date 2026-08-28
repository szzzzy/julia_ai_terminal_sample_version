/**
 * @file    julia_lipsync.c
 * @brief   语音输出与嘴型驱动的耦合实现：把 PCM 帧 RMS 换算为口型开合度。
 *
 * 职责：接收任意外部 PCM 帧流，按 40ms 帧切分，计算 RMS -> 分档（0..3）
 *        -> 平滑后的 q8.8 开合度，调用 julia_ui 的 mouth 接口驱动口型；同时按
 *        运行期模式决定是否把同一帧写入扬声器（julia_audio_play_start/stop）。
 *
 * 边界：本模块只驱动 mouth 的"开合度"与"是否播声"，不负责语音采集、编解码、
 * 也不决定"说什么"。是否开启表情 UI 由 julia_lipsync_mode_t 决定。
 *
 * 调用方：julia_voice 的流式播放任务（stream_playback_task）与 TTS 播放/提示音
 * 路径调用 begin/play/play_file/end；demo 用于无网络时单独观察嘴型节奏。
 *
 * 数据流：PCM 帧 -> EMA 平滑 RMS -> 迟滞分档 -> 快攻慢放得到 q8.8 开合度
 *          -> julia_ui_set_mouth_openness()；同一帧（FUSED/VOICE_ONLY）另经
 *          julia_audio_play_start() 写入扬声器。
 *
 * 关键不变量：
 *   - 必须先 julia_lipsync_begin() 再 julia_lipsync_play()，最后 julia_lipsync_end()；
 *     begin 会为 FUSED/EXPR_ONLY 调用 julia_ui_talking_start()，而 julia_ui 的
 *     set_mouth_openness 仅在该 talking 状态为真时才生效（见 julia_ui.c）；
 *   - 分档阈值针对 int16 原始采样（未归一化到 0..1），故量级在数百到数千；
 *   - 累积状态（EMA RMS、档位、开合度）跨帧持有，介于 begin/end 之间有效。
 */

#include "julia_lipsync.h"

#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "julia_audio.h"
#include "julia_ui.h"
#include "esp_log.h"

/* 每个嘴型帧的样本数：采样率 / 25 = 40ms 一帧（约 25fps）。
 * 16kHz 下为 640 样本；JULIA_AUDIO_SAMPLE_RATE 由 julia_audio.h 提供。
 * 帧长是嘴型动画与扬声器共用的最小节拍单元。 */
#define LIPSYNC_FRAME_SAMPLES (JULIA_AUDIO_SAMPLE_RATE / 25)

static const char *TAG = "JULIA_LIPSYNC";

/* ---- begin/end 会话内累积状态 ----
 * 这些成员在 julia_lipsync_begin() 清零，在一次会话内跨帧持有：
 *  - s_frame_count：已处理帧数，仅用于 end() 日志。
 *  - s_smoothed_rms：逐帧 RMS 的 EMA（(5*旧 + 3*新)/8），用于分档，抑制单帧噪声。
 *  - s_mouth_level：当前档位 0..3，配合上升/下降阈值做迟滞，避免阈值附近抖动。
 *  - s_visual_openness_q8：q8.8 固定点口型开合度（0..768，即 档位*256），带快攻慢放。
 * s_mode 标 volatile：可能由其他任务经 julia_lipsync_set_mode() 写入，而
 * begin/play/end 在播声任务上下文读取。默认 FUSED。 */
static size_t s_frame_count;
static uint32_t s_smoothed_rms;
static uint8_t s_mouth_level;
static uint16_t s_visual_openness_q8;
static volatile julia_lipsync_mode_t s_mode = JULIA_LIPSYNC_MODE_FUSED;

/**
 * @brief 设置运行期耦合模式。
 *
 * @param[in] mode 期望模式；若数值越界（> JULIA_LIPSYNC_MODE_EXPR_ONLY）会钳制为 FUSED。
 * @note 可被任意普通任务调用；仅做一次 volatile 写入 + 日志，不要在中断上下文调用。
 *       模式变化对正在进行中的 begin/play 会话即时生效（从下一帧起）。
 */
void julia_lipsync_set_mode(julia_lipsync_mode_t mode)
{
    if (mode > JULIA_LIPSYNC_MODE_EXPR_ONLY) mode = JULIA_LIPSYNC_MODE_FUSED;
    s_mode = mode;
    ESP_LOGI(TAG, "mode=%s", julia_lipsync_mode_name(mode));
}

julia_lipsync_mode_t julia_lipsync_get_mode(void) { return s_mode; }

/**
 * @brief 当前模式是否会驱动表情 UI。
 *
 * 仅 JULIA_LIPSYNC_MODE_VOICE_ONLY 为 false；供语音链路在只播声时跳过表情相关 UI 调用。
 */
bool julia_lipsync_ui_enabled(void)
{
    return s_mode != JULIA_LIPSYNC_MODE_VOICE_ONLY;
}

/**
 * @brief 把模式映射为日志友好的短字符串；未知值归一到 "fused"。
 *
 * @param[in] mode 模式。
 * @return 只读的静态字符串字面量，可直接用于 ESP_LOGI（无需释放）。
 */
const char *julia_lipsync_mode_name(julia_lipsync_mode_t mode)
{
    switch (mode) {
    case JULIA_LIPSYNC_MODE_VOICE_ONLY: return "voice";
    case JULIA_LIPSYNC_MODE_EXPR_ONLY: return "expr";
    case JULIA_LIPSYNC_MODE_FUSED:
    default: return "fused";
    }
}

/**
 * @brief 单帧 RMS 能量 -> 嘴型档位（0..3），带时间平滑与迟滞。
 *
 * 背景：40ms 帧的能量在相邻音节间会剧烈起伏（辅音-元音交替）。若直接用瞬时
 * RMS 映射，嘴在阈值附近会快速抖动；这里用两层处理：
 *   1. EMA 平滑：s_smoothed_rms = (5*旧 + 3*新)/8，抑制单帧噪声；
 *   2. 不对称阈值（上升/下降门限不同）：向上用较高的 rise[]，向下用较低的
 *      fall[]，构成迟滞回线——能量略高于下限也不会反复升降档。
 *
 * 阈值量级：这些是 int16 原始采样的 RMS 绝对值（未归一化到 0..1，故量级在
 * 数百到数千）。对满幅 32767 采样，RMS 上限约为 32767；rise = {300,950,2300}
 * 约对应 -41/-31/-23 dBFS（20*log10(v/32767)），属语音常见的低-中能量区间。
 * NOTE：档位 -> UI 亮度（0/30/65/95）的换算在 julia_ui_set_mouth_openness() 内。
 *
 * @param[in] samples 当前帧的 PCM 采样，不允许为 NULL。
 * @param[in] count   采样数；为 0 时不更新，函数直接返回当前档位（RMS 记 0）。
 * @return 平滑后的档位（0..3），并同步写入 s_mouth_level。
 */
static uint8_t mouth_level_for_frame(const int16_t *samples, size_t count)
{
    uint64_t energy = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = samples[i];
        energy += (uint64_t)(sample * sample);
    }
    uint32_t rms = count ? (uint32_t)sqrt((double)energy / count) : 0;
    /* 40 ms PCM frames can fluctuate sharply between adjacent syllables.
     * EMA plus asymmetric thresholds keeps articulation responsive without
     * making the mouth chatter at a threshold. */
    s_smoothed_rms = (s_smoothed_rms * 5U + rms * 3U) / 8U;
    /* 上升/下降门限成对出现：升档需跨越高一档的 rise[]，降档需跌破低一档的 fall[]。 */
    static const uint16_t rise[] = {300, 950, 2300};
    static const uint16_t fall[] = {180, 650, 1650};
    uint8_t target = s_mouth_level;
    if (target < 3 && s_smoothed_rms >= rise[target]) target++;
    else if (target > 0 && s_smoothed_rms < fall[target - 1]) target--;
    s_mouth_level = target;
    return target;
}

/**
 * @brief 开始一次嘴型/播声会话：清零累积状态，并按模式开启表情 UI。
 *
 * @note 必须先于 julia_lipsync_play()/play_file() 调用，并与 julia_lipsync_end() 配对。
 *       对 FUSED/EXPR_ONLY 会调用 julia_ui_talking_start()——只有 talking 状态为真，
 *       julia_ui_set_mouth_openness() 才真正驱动嘴型（见 julia_ui.c 的 s_talking 守卫）。
 *       VOICE_ONLY 不驱动任何表情 UI。
 */
void julia_lipsync_begin(void)
{
    s_frame_count = 0;
    s_smoothed_rms = 0;
    s_mouth_level = 0;
    s_visual_openness_q8 = 0;
    /* VOICE_ONLY 不驱动任何表情 UI；EXPR_ONLY/FUSED 需要 talking 状态
     * 才会让 julia_ui_set_mouth_openness() 生效。 */
    if (s_mode != JULIA_LIPSYNC_MODE_VOICE_ONLY) julia_ui_talking_start();
    ESP_LOGI(TAG, "started: frame=%dms mode=%s", 1000 / 25,
             julia_lipsync_mode_name(s_mode));
}

/**
 * @brief 处理一段 PCM：按帧驱动嘴型（可配）并写入扬声器（可配）。
 *
 * 按 LIPSYNC_FRAME_SAMPLES 把输入逐帧切分，直到用完 sample_count（末帧可能不足）：
 *   - FUSED/EXPR_ONLY：对每帧调 mouth_level_for_frame() 得档位，再经快攻慢放的
 *     平滑得到 q8.8 开合度并调 julia_ui_set_mouth_openness()；
 *   - FUSED/VOICE_ONLY：把该帧交给 julia_audio_play_start() 写入扬声器；
 *   - EXPR_ONLY：无扬声器可作节拍源，改用 vTaskDelay(40ms) 保持嘴型节奏。
 *
 * 快攻慢放：上升取 (delta+1)/2，下降取 delta/3——快速跟上音节、缓慢闭合，
 * 避免嘴型突然坍缩；中间值表示 UI 在相邻档位间平滑过渡。
 *
 * @param[in] samples      PCM16 采样首地址，不允许为 NULL。
 * @param[in] sample_count 采样数，必须大于 0。
 * @return ESP_OK 处理完毕；ESP_ERR_INVALID_ARG 参数非法；若扬声器写入失败，
 *         返回 julia_audio_play_start() 的错误并中止后续帧。
 * @note 在播声任务上下文调用；内部可能 vTaskDelay（EXPR_ONLY）或阻塞等待音频
 *       缓冲区（写入扬声器），因此不要在中断上下文调用。
 */
esp_err_t julia_lipsync_play(const int16_t *samples, size_t sample_count)
{
    if (!samples || !sample_count) return ESP_ERR_INVALID_ARG;
    size_t offset = 0;
    while (offset < sample_count) {
        size_t count = sample_count - offset;
        if (count > LIPSYNC_FRAME_SAMPLES) count = LIPSYNC_FRAME_SAMPLES;
        /* FUSED / EXPR_ONLY：计算能量并按帧驱动嘴型。 */
        if (s_mode != JULIA_LIPSYNC_MODE_VOICE_ONLY) {
            uint8_t level = mouth_level_for_frame(samples + offset, count);
            uint16_t target_q8 = (uint16_t)level * 256U;
            int32_t delta = (int32_t)target_q8 - s_visual_openness_q8;
            /* Fast attack follows syllables; slower release avoids snapping shut.
             * The UI blends adjacent source mouths at the fractional position. */
            s_visual_openness_q8 = (uint16_t)((int32_t)s_visual_openness_q8 +
                (delta > 0 ? (delta + 1) / 2 : delta / 3));
            julia_ui_set_mouth_openness(s_visual_openness_q8);
        }
        /* FUSED / VOICE_ONLY：把这一帧写入扬声器。 */
        if (s_mode != JULIA_LIPSYNC_MODE_EXPR_ONLY) {
            esp_err_t err = julia_audio_play_start((uint8_t *)(samples + offset),
                                                   count * sizeof(int16_t));
            if (err != ESP_OK) return err;
        } else {
            /* EXPR_ONLY 没有扬声器作为节拍源，按真实帧间隔推进，
             * 让嘴型动画保持与语音一致的节奏（便于单独观察）。 */
            vTaskDelay(pdMS_TO_TICKS(1000 / 25));
        }
        ++s_frame_count;
        offset += count;
    }
    return ESP_OK;
}

/**
 * @brief 从本地文件读取 raw PCM16 并播放（用于本地提示音等，不解析 WAV 头）。
 *
 * 逐帧读取最多 LIPSYNC_FRAME_SAMPLES 个 int16 交给 julia_lipsync_play()；读到不足一帧
 * （文件尾）结束循环，遇读取错误（ferror）置 ESP_FAIL。
 *
 * @param[in] path 文件路径，不允许为 NULL；须为原始 PCM16（非 WAV/容器格式）。
 * @return ESP_OK 正常播完；ESP_ERR_INVALID_ARG 路径为空；ESP_ERR_NOT_FOUND 打不开；
 *         其他为 julia_lipsync_play() 或读取错误转出的失败码。
 * @note 假定文件为单声道 PCM16；调用前应先完成 julia_lipsync_begin()。
 */
esp_err_t julia_lipsync_play_file(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    int16_t frame[LIPSYNC_FRAME_SAMPLES];
    esp_err_t err = ESP_OK;
    while (true) {
        size_t count = fread(frame, sizeof(int16_t), LIPSYNC_FRAME_SAMPLES, file);
        if (count && (err = julia_lipsync_play(frame, count)) != ESP_OK) break;
        if (count < LIPSYNC_FRAME_SAMPLES) {
            /* 不足一帧：到达 EOF；若此时 ferror 说明中途读取出错。 */
            if (ferror(file)) err = ESP_FAIL;
            break;
        }
    }
    fclose(file);
    return err;
}

/**
 * @brief 结束一次嘴型/播声会话：关闭表情 UI 与扬声器。
 *
 * @note 与 julia_lipsync_begin() 配对调用；对 FUSED/EXPR_ONLY 调 julia_ui_talking_stop()
 *       （闭合嘴型并恢复运动），对 FUSED/VOICE_ONLY 调 julia_audio_play_stop()。
 * @return ESP_OK 成功；否则为 julia_audio_play_stop() 的错误。
 */
esp_err_t julia_lipsync_end(void)
{
    if (s_mode != JULIA_LIPSYNC_MODE_VOICE_ONLY) julia_ui_talking_stop();
    esp_err_t err = ESP_OK;
    if (s_mode != JULIA_LIPSYNC_MODE_EXPR_ONLY) err = julia_audio_play_stop();
    ESP_LOGI(TAG, "stopped: frames=%u mode=%s result=%s", (unsigned)s_frame_count,
             julia_lipsync_mode_name(s_mode), esp_err_to_name(err));
    return err;
}

/**
 * @brief 合成一段模拟语音（350Hz 音节 + 停顿）并走完整 begin/play/end 链路。
 *
 * 用途：不联网、不唤醒时单独观察嘴型节奏。生成的 PCM 为单声道、采样率即
 * JULIA_AUDIO_SAMPLE_RATE；用 60% 峰值（6000，而非满幅 32767）压低 RMS，
 * 避免 EXPR_ONLY 下某个档位被顶死。播放与否取决于当前模式。
 *
 * @param[in] duration_ms 时长；0 或 >60000 时按 3000ms 处理。
 * @return ESP_OK 完成；ESP_ERR_NO_MEM PSRAM 分配失败；其余为播放链路错误。
 * @note 在普通任务上下文调用；内部会阻塞（播声/延迟），并临时占用一块 PSRAM 作为
 *       整段 PCM 缓冲。
 */
esp_err_t julia_lipsync_demo(uint32_t duration_ms)
{
    if (!duration_ms || duration_ms > 60000) duration_ms = 3000;
    const uint32_t syllable_ms = 180, pause_ms = 120;
    const uint32_t total_ms = duration_ms;
    /* 1000 样本/s * 毫秒数，单声道 16 kHz。 */
    size_t max_samples = (size_t)(JULIA_AUDIO_SAMPLE_RATE / 1000U) * total_ms;
    int16_t *pcm = heap_caps_malloc(max_samples * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) return ESP_ERR_NO_MEM;
    size_t total = 0;
    float phase = 0.0f;
    size_t syllable_samples = (size_t)(JULIA_AUDIO_SAMPLE_RATE / 1000U) * syllable_ms;
    size_t pause_samples = (size_t)(JULIA_AUDIO_SAMPLE_RATE / 1000U) * pause_ms;
    while (total < max_samples) {
        /* 音节：350 Hz 基频 + 幅度包络，模拟说话起伏（不开变调，保证 RMS 可测）。 */
        for (size_t i = 0; i < syllable_samples && total < max_samples; ++i) {
            float t = (float)i / (float)syllable_samples;
            float envelope = sinf(t * (float)M_PI);
            float level = envelope * envelope;
            phase += 2.0f * (float)M_PI * 350.0f / JULIA_AUDIO_SAMPLE_RATE;
            /* 60% 峰值，避免 EXPR_ONLY 下 RMS 全档爆表。 */
            pcm[total++] = (int16_t)(6000.0f * level * sinf(phase));
        }
        for (size_t i = 0; i < pause_samples && total < max_samples; ++i) {
            pcm[total++] = 0;
        }
    }
    julia_lipsync_begin();
    esp_err_t err = julia_lipsync_play(pcm, total);
    esp_err_t stop_err = julia_lipsync_end();
    heap_caps_free(pcm);
    if (err == ESP_OK) err = stop_err;
    ESP_LOGI(TAG, "demo done duration_ms=%u result=%s", total_ms, esp_err_to_name(err));
    return err;
}
