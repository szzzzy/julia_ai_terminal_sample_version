#include "julia_lipsync.h"

#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "julia_audio.h"
#include "julia_ui.h"
#include "esp_log.h"

#define LIPSYNC_FRAME_SAMPLES (JULIA_AUDIO_SAMPLE_RATE / 25)

static const char *TAG = "JULIA_LIPSYNC";
static size_t s_frame_count;
static uint32_t s_smoothed_rms;
static uint8_t s_mouth_level;
static uint16_t s_visual_openness_q8;
static volatile julia_lipsync_mode_t s_mode = JULIA_LIPSYNC_MODE_FUSED;

void julia_lipsync_set_mode(julia_lipsync_mode_t mode)
{
    if (mode > JULIA_LIPSYNC_MODE_EXPR_ONLY) mode = JULIA_LIPSYNC_MODE_FUSED;
    s_mode = mode;
    ESP_LOGI(TAG, "mode=%s", julia_lipsync_mode_name(mode));
}

julia_lipsync_mode_t julia_lipsync_get_mode(void) { return s_mode; }

bool julia_lipsync_ui_enabled(void)
{
    return s_mode != JULIA_LIPSYNC_MODE_VOICE_ONLY;
}

const char *julia_lipsync_mode_name(julia_lipsync_mode_t mode)
{
    switch (mode) {
    case JULIA_LIPSYNC_MODE_VOICE_ONLY: return "voice";
    case JULIA_LIPSYNC_MODE_EXPR_ONLY: return "expr";
    case JULIA_LIPSYNC_MODE_FUSED:
    default: return "fused";
    }
}

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
    static const uint16_t rise[] = {300, 950, 2300};
    static const uint16_t fall[] = {180, 650, 1650};
    uint8_t target = s_mouth_level;
    if (target < 3 && s_smoothed_rms >= rise[target]) target++;
    else if (target > 0 && s_smoothed_rms < fall[target - 1]) target--;
    s_mouth_level = target;
    return target;
}

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
            if (ferror(file)) err = ESP_FAIL;
            break;
        }
    }
    fclose(file);
    return err;
}

esp_err_t julia_lipsync_end(void)
{
    if (s_mode != JULIA_LIPSYNC_MODE_VOICE_ONLY) julia_ui_talking_stop();
    esp_err_t err = ESP_OK;
    if (s_mode != JULIA_LIPSYNC_MODE_EXPR_ONLY) err = julia_audio_play_stop();
    ESP_LOGI(TAG, "stopped: frames=%u mode=%s result=%s", (unsigned)s_frame_count,
             julia_lipsync_mode_name(s_mode), esp_err_to_name(err));
    return err;
}

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
