/**
 * @file voice_playback.c
 * @brief 用固定容量缓冲吸收网络抖动，并由单一后台任务连续驱动扬声器。
 *
 * 收到开始命令后先等待约 80 ms 声音，避免刚开播就因网络小间隔产生断续；
 * 输入短暂停顿时重新积累，连续 15 秒没有可播放数据才判定超时。服务器声明结束后，
 * 设备会播放所有已接受声音并补足扬声器硬件尾音，再报告“实际播放完成”。
 * 用户插话或新一轮播放会使旧编号失效，旧任务不能覆盖新一轮状态。
 */
#include "voice_playback.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pcm_buffer.h"

#define PLAYBACK_CAPACITY_BYTES (64U * 1024U)
#define PLAYBACK_CHUNK_SAMPLES 160U
#define PLAYBACK_PREBUFFER_MS 80U
#define PLAYBACK_PREBUFFER_WAIT_MS 120U
#define PLAYBACK_STARVE_MS 15000U
/* 结束输入后再写入略多于扬声器硬件队列容量的静音，确保已接受的尾音真正离开硬件。 */
#define PLAYBACK_DRAIN_SAMPLES (5U * PLAYBACK_CHUNK_SAMPLES)

static const char *TAG = "voice_playback";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static pcm_buffer_t s_buffer;
static uint32_t s_generation;
static uint32_t s_rate;
static bool s_active;
static bool s_test;
static int64_t s_last_input_us;
static uint32_t s_completion_generation;
static esp_err_t s_completion_result;
static size_t s_high_water;
static uint32_t s_overflows;
static audio_pcm_sink_t s_pcm_sink;
static void *s_pcm_ctx;

/* 互斥只保护播放进度和小块内存复制；扬声器写入不持锁，避免阻塞新数据和打断请求。 */
static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void complete(uint32_t generation, esp_err_t result)
{
    lock();
    if (s_generation == generation && s_active) {
        s_active = false;
        pcm_buffer_reset(&s_buffer);
        s_completion_generation = generation;
        s_completion_result = result;
    }
    unlock();
}

static void playback_task(void *arg)
{
    (void)arg;
    uint32_t owned_generation = 0;
    bool device_started = false;
    bool buffering = true;
    int64_t buffering_since = 0;
    size_t drain_samples = 0;
    size_t test_sample = 0;
    int16_t pcm[PLAYBACK_CHUNK_SAMPLES];

    for (;;) {
        size_t bytes = 0;
        lock();
        uint32_t generation = s_generation;
        uint32_t rate = s_rate;
        bool active = s_active;
        bool test = s_test;
        bool ended = s_buffer.ended;
        size_t queued = s_buffer.size;
        int64_t last_input = s_last_input_us;
        unlock();

        if (owned_generation != generation || !active) {
            if (device_started) (void)board_audio_speaker_stop();
            device_started = false;
            owned_generation = generation;
            buffering = true;
            buffering_since = 0;
            drain_samples = 0;
            test_sample = 0;
        }
        if (!active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!device_started) {
            esp_err_t err = board_audio_speaker_start(rate);
            if (err != ESP_OK) {
                (void)board_audio_speaker_stop();
                complete(generation, err);
                continue;
            }
            device_started = true;
        }

        int64_t now = esp_timer_get_time();
        if (test && !ended) {
            /* Three 256ms tones with short silence gaps; cancellation is checked
             * every 160 samples, unlike the blocking board self-test API. */
            const size_t tone_samples = 6144;
            const size_t period = tone_samples + 512;
            if (test_sample < period * 3) {
                static const unsigned hz[] = {440, 660, 880};
                for (size_t i = 0; i < PLAYBACK_CHUNK_SAMPLES; ++i, ++test_sample) {
                    size_t tone = test_sample / period;
                    size_t phase = test_sample % period;
                    pcm[i] = tone < 3 && phase < tone_samples
                                 ? (int16_t)(sin(2.0 * M_PI * hz[tone] * phase / rate) * 26000)
                                 : 0;
                }
                bytes = sizeof(pcm);
            } else {
                ended = true;
            }
        } else if (queued > 0) {
            if (buffering_since == 0) buffering_since = now;
            if (buffering && !ended && queued < rate * 2U * PLAYBACK_PREBUFFER_MS / 1000U &&
                now - buffering_since < PLAYBACK_PREBUFFER_WAIT_MS * 1000LL) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
                continue;
            }
            buffering = false;
            lock();
            if (s_active && generation == s_generation) {
                bytes = pcm_buffer_read(&s_buffer, pcm, sizeof(pcm));
            }
            unlock();
        } else if (!ended) {
            buffering = true;
            buffering_since = 0;
            if (now - last_input >= PLAYBACK_STARVE_MS * 1000LL) {
                ESP_LOGW(TAG, "generation=%lu starved for %ums", (unsigned long)generation,
                         PLAYBACK_STARVE_MS);
                complete(generation, ESP_ERR_TIMEOUT);
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            continue;
        }

        bool audio = bytes != 0;
        if (bytes == 0 && ended) {
            if (drain_samples >= PLAYBACK_DRAIN_SAMPLES) {
                (void)board_audio_speaker_stop();
                device_started = false;
                complete(generation, ESP_OK);
                continue;
            }
            memset(pcm, 0, sizeof(pcm));
            bytes = sizeof(pcm);
            drain_samples += PLAYBACK_CHUNK_SAMPLES;
        }
        lock();
        bool current = s_active && s_generation == generation;
        unlock();
        if (bytes == 0 || !current) continue;

        esp_err_t err = board_audio_speaker_write((const uint8_t *)pcm, bytes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "generation=%lu I2S failed: %s", (unsigned long)generation,
                     esp_err_to_name(err));
            complete(generation, err);
            continue;
        }
        /* UI receives played chunks, never network arrival bursts. Cancellation
         * may race this callback by one chunk; it cannot restore talking state. */
        lock();
        current = s_active && s_generation == generation;
        unlock();
        if (audio && current && s_pcm_sink != NULL) {
            s_pcm_sink(pcm, bytes / sizeof(int16_t), s_pcm_ctx);
        }
    }
}

esp_err_t voice_playback_init(audio_pcm_sink_t pcm_sink, void *ctx)
{
    if (s_task != NULL) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    uint8_t *storage = heap_caps_malloc(PLAYBACK_CAPACITY_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    pcm_buffer_init(&s_buffer, storage, PLAYBACK_CAPACITY_BYTES);
    s_pcm_sink = pcm_sink;
    s_pcm_ctx = ctx;
    if (xTaskCreatePinnedToCore(playback_task, "voice_playback", 4096, NULL, 6,
                               &s_task, 0) != pdPASS) {
        heap_caps_free(storage);
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready buffer=%uB prebuffer=%ums starvation=%ums",
             PLAYBACK_CAPACITY_BYTES, PLAYBACK_PREBUFFER_MS, PLAYBACK_STARVE_MS);
    return ESP_OK;
}

esp_err_t voice_playback_start(uint32_t rate, bool self_test, uint32_t *generation)
{
    if (s_task == NULL) return ESP_ERR_INVALID_STATE;
    if (generation == NULL || (rate != 16000 && rate != 24000)) return ESP_ERR_INVALID_ARG;
    lock();
    if (++s_generation == 0) ++s_generation;
    *generation = s_generation;
    pcm_buffer_reset(&s_buffer);
    s_rate = rate;
    s_test = self_test;
    s_active = true;
    s_completion_generation = 0;
    s_last_input_us = esp_timer_get_time();
    s_high_water = 0;
    unlock();
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t voice_playback_write(const uint8_t *pcm, size_t bytes)
{
    if (s_task == NULL) return ESP_ERR_INVALID_STATE;
    if (pcm == NULL || bytes == 0 || (bytes & 1U) || bytes > 1200U) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t result = ESP_OK;
    if (!s_active || s_test || s_buffer.ended) {
        result = ESP_ERR_INVALID_STATE;
    } else if (!pcm_buffer_write(&s_buffer, pcm, bytes)) {
        /* Abort the whole turn explicitly; never silently truncate speech. */
        ++s_overflows;
        s_completion_generation = s_generation;
        s_completion_result = ESP_ERR_NO_MEM;
        s_active = false;
        pcm_buffer_reset(&s_buffer);
        result = ESP_ERR_NO_MEM;
    } else {
        s_last_input_us = esp_timer_get_time();
        if (s_buffer.size > s_high_water) s_high_water = s_buffer.size;
    }
    unlock();
    xTaskNotifyGive(s_task);
    return result;
}

void voice_playback_finish(void)
{
    if (s_task == NULL) return;
    lock();
    pcm_buffer_end(&s_buffer);
    unlock();
    xTaskNotifyGive(s_task);
}

void voice_playback_stop(void)
{
    if (s_task == NULL) return;
    lock();
    if (++s_generation == 0) ++s_generation;
    s_active = false;
    s_completion_generation = 0;
    pcm_buffer_reset(&s_buffer);
    unlock();
    xTaskNotifyGive(s_task);
}

bool voice_playback_is_active(void)
{
    if (s_task == NULL) return false;
    lock();
    bool active = s_active;
    unlock();
    return active;
}

bool voice_playback_take_completion(uint32_t *generation, esp_err_t *result)
{
    if (s_task == NULL || generation == NULL || result == NULL) return false;
    lock();
    bool ready = s_completion_generation != 0;
    *generation = s_completion_generation;
    *result = s_completion_result;
    size_t high_water = s_high_water;
    uint32_t overflows = s_overflows;
    s_completion_generation = 0;
    unlock();
    if (ready) ESP_LOGI(TAG, "done generation=%lu result=%s high_water=%u overflow_total=%lu",
                       (unsigned long)*generation, esp_err_to_name(*result),
                       (unsigned)high_water, (unsigned long)overflows);
    return ready;
}
