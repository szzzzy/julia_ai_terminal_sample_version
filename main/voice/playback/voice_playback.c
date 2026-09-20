/**
 * @file voice_playback.c
 * @brief 用固定容量缓冲吸收网络抖动，并由单一后台任务连续驱动扬声器。
 *
 * 收到首批声音即开始输出，不等待预缓冲；输入短暂停顿后有数据即继续输出，
 * 连续 15 秒没有可播放数据才判定超时。服务器声明结束后，
 * 设备会播放所有已接受声音并补足扬声器硬件尾音，再报告“实际播放完成”。
 * 用户插话或新一轮播放会使旧编号失效，旧任务不能覆盖新一轮状态。固件内嵌
 * PCM 由同一 Task 直接分块读取，避免固定提示被网络缓冲容量限制。
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

#define PLAYBACK_CAPACITY_BYTES (128U * 1024U)
#define PLAYBACK_CHUNK_SAMPLES 160U
#define PLAYBACK_STARVE_MS 15000U
/* 结束输入后再写入略多于扬声器硬件队列容量的静音，确保已接受的尾音真正离开硬件。
 * 板级 DMA 队列为 4×160 帧（components/julia_board_audio/board_audio.c 的
 * dma_desc_num/dma_frame_num），5×160 样本必须大于该容量；改板级 DMA 配置时同步此处。 */
#define PLAYBACK_DRAIN_SAMPLES (5U * PLAYBACK_CHUNK_SAMPLES)

static const char *TAG = "voice_playback";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static pcm_buffer_t s_buffer;
static uint32_t s_generation;
static uint32_t s_rate;
static bool s_active;
static bool s_keep_resources;
static bool s_test;
/* 本地源不复制，三个字段均受 s_lock 保护；调用方必须保证源覆盖播放生命周期。 */
static const uint8_t *s_local_pcm;
static size_t s_local_bytes;
static size_t s_local_offset;
static int64_t s_last_input_us;
static uint32_t s_completion_generation;
static esp_err_t s_completion_result;
static voice_playback_timing_t s_timing;
static size_t s_high_water;
static uint32_t s_overflows;
static int64_t s_last_output_us;
static voice_playback_buffer_status_t s_flow;
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
        s_timing.completed_us = esp_timer_get_time();
    }
    unlock();
}

static void playback_task(void *arg)
{
    (void)arg;
    uint32_t owned_generation = 0;
    bool device_started = false;
    bool resources_kept = false;
    int64_t last_write_us = 0;
    size_t drain_samples = 0;
    size_t test_sample = 0;
    int16_t pcm[PLAYBACK_CHUNK_SAMPLES];

    for (;;) {
        size_t bytes = 0;
        lock();
        uint32_t generation = s_generation;
        uint32_t rate = s_rate;
        bool active = s_active;
        bool keep_resources = s_keep_resources;
        bool test = s_test;
        bool local = s_local_pcm != NULL;
        bool ended = local ? s_local_offset >= s_local_bytes : s_buffer.ended;
        size_t queued = s_buffer.size;
        int64_t last_input = s_last_input_us;
        unlock();

        if (resources_kept != keep_resources) {
            if (board_audio_speaker_retain(keep_resources) == ESP_OK)
                resources_kept = keep_resources;
        }

        if (owned_generation != generation || !active) {
            if (device_started) (void)board_audio_speaker_stop();
            device_started = false;
            owned_generation = generation;
            drain_samples = 0;
            test_sample = 0;
        }
        if (!active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        int64_t now = esp_timer_get_time();
        /* 距上次成功写入 60ms（毫秒）后仍没有待播数据，就释放硬件时钟；
         * 无待播输入时不持续输出空时钟。 */
        if (device_started && !queued && !local && !test && !ended &&
            now - last_write_us >= 60000) {
            (void)board_audio_speaker_stop();
            device_started = false;
        }
        if (test && !ended) {
            /* 三段 256 ms 音调之间留短静音；每 160 sample 检查一次取消，避免采用
             * 板级阻塞自检时无法及时响应新播放代次。 */
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
        } else if (local && !ended) {
            /* 静态资源无需网络预缓冲；锁内只复制当前块，I2S 写入仍在锁外完成。 */
            lock();
            if (s_active && generation == s_generation && s_local_pcm != NULL) {
                size_t remaining = s_local_bytes - s_local_offset;
                bytes = remaining < sizeof(pcm) ? remaining : sizeof(pcm);
                memcpy(pcm, s_local_pcm + s_local_offset, bytes);
                s_local_offset += bytes;
            }
            unlock();
        } else if (queued > 0) {
            /* 首包和断流后的数据都立即输出，不额外等待积累音频。 */
            lock();
            if (s_active && generation == s_generation) {
                bytes = pcm_buffer_read(&s_buffer, pcm, sizeof(pcm));
                s_flow.dequeued_bytes += bytes;
            }
            unlock();
        } else if (!ended) {
            /* 输入短暂停顿不立即判失败：等待新数据唤醒；只有距最近一次输入／
             * 开播达到 15 秒仍无可播数据才以 ESP_ERR_TIMEOUT 结束本轮。 */
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

        if (!device_started) {
            /* 播放任务是唯一 I2S 写入者；board_audio_speaker_start() 在已有播放源时
             * 会先停掉旧源（后开优先），所以不能再引入第二个写入者接管同一通道。 */
            lock();
            if (generation == s_generation) s_flow.i2s_request_us = esp_timer_get_time();
            unlock();
            esp_err_t start_err = board_audio_speaker_start(rate);
            if (start_err != ESP_OK) {
                (void)board_audio_speaker_stop();
                complete(generation, start_err);
                continue;
            }
            device_started = true;
            lock();
            if (generation == s_generation) {
                s_flow.i2s_started_us = esp_timer_get_time();
                s_flow.configured_rate = rate;
            }
            unlock();
        }

        esp_err_t err = board_audio_speaker_write((const uint8_t *)pcm, bytes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "generation=%lu I2S failed: %s", (unsigned long)generation,
                     esp_err_to_name(err));
            complete(generation, err);
            continue;
        }
        last_write_us = esp_timer_get_time();
        /* UI 只接收已经写入 I2S 的块，不接收网络突发。取消最多与一个块并发，
         * talking 门控会拒绝该迟到块重新驱动嘴型。 */
        lock();
        current = s_active && s_generation == generation;
        if (audio && current && s_timing.first_output_us == 0)
            s_timing.first_output_us = esp_timer_get_time();
        if (audio && current) {
            s_last_output_us = esp_timer_get_time();
            s_flow.output_bytes += bytes;
        }
        unlock();
        if (audio && current && s_pcm_sink != NULL) {
            s_pcm_sink(pcm, bytes / sizeof(int16_t), s_pcm_ctx);
        }
    }
}

esp_err_t voice_playback_init(audio_pcm_sink_t pcm_sink, void *ctx)
{
    /* 幂等：s_task 非空即视为已初始化。失败路径释放锁和缓冲，但 public 接口都以
     * s_task == NULL 作为“未初始化”判据，因此不会访问已释放的资源。 */
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
    ESP_LOGI(TAG, "ready buffer=%uB immediate_output starvation=%ums",
             PLAYBACK_CAPACITY_BYTES, PLAYBACK_STARVE_MS);
    return ESP_OK;
}

void voice_playback_set_interaction(bool enabled)
{
    /* 由 FSM 观察者按主状态调用：S4/S2 为 true，保留 I2S 通道减少反复重建；
     * 其他状态为 false，仅在空闲时删除通道，不打断仍在播放的语音。 */
    if (!s_lock) return;
    lock();
    s_keep_resources = enabled;
    unlock();
    if (s_task) xTaskNotifyGive(s_task);
}

esp_err_t voice_playback_start(uint32_t rate, bool self_test, uint32_t *generation)
{
    if (s_task == NULL) return ESP_ERR_INVALID_STATE;
    if (generation == NULL || (rate != 16000 && rate != 24000)) return ESP_ERR_INVALID_ARG;
    lock();
    /* generation 每轮 +1 并跳过 0；0 在本地表示“无播放代次”，也是无条件停止的入参，
     * 因此对外暴露的代次永不为 0。 */
    if (++s_generation == 0) ++s_generation;
    *generation = s_generation;
    s_timing = (voice_playback_timing_t){.generation = s_generation};
    s_last_output_us = 0;
    s_flow = (voice_playback_buffer_status_t){.started_us = esp_timer_get_time()};
    pcm_buffer_reset(&s_buffer);
    s_rate = rate;
    s_test = self_test;
    s_local_pcm = NULL;
    s_local_bytes = 0;
    s_local_offset = 0;
    s_active = true;
    s_completion_generation = 0;
    s_last_input_us = esp_timer_get_time();
    s_high_water = 0;
    unlock();
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

static esp_err_t start_local(uint32_t rate, const uint8_t *pcm, size_t bytes,
                              bool only_if_idle, uint32_t *generation)
{
    if (s_task == NULL) return ESP_ERR_INVALID_STATE;
    if (generation == NULL || pcm == NULL || bytes == 0 || (bytes & 1U) ||
        (rate != 16000 && rate != 24000)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    /* 网络播放的完成结果仍由语音服务消费，提示不能提前覆盖其收尾通知。 */
    if (only_if_idle && (s_active ||
        (s_completion_generation != 0 && s_local_pcm == NULL))) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (++s_generation == 0) ++s_generation;
    *generation = s_generation;
    s_timing = (voice_playback_timing_t){.generation = s_generation};
    s_last_output_us = 0;
    s_flow = (voice_playback_buffer_status_t){.started_us = esp_timer_get_time()};
    pcm_buffer_reset(&s_buffer);
    s_rate = rate;
    s_test = false;
    s_local_pcm = pcm;
    s_local_bytes = bytes;
    s_local_offset = 0;
    s_active = true;
    s_completion_generation = 0;
    s_last_input_us = esp_timer_get_time();
    s_high_water = 0;
    unlock();
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t voice_playback_start_local(uint32_t rate, const uint8_t *pcm, size_t bytes,
                                     uint32_t *generation)
{
    return start_local(rate, pcm, bytes, false, generation);
}

esp_err_t voice_playback_try_start_local(uint32_t rate, const uint8_t *pcm, size_t bytes,
                                         uint32_t *generation)
{
    return start_local(rate, pcm, bytes, true, generation);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

esp_err_t voice_playback_start_local_wav(const uint8_t *wav, size_t bytes,
                                         bool only_if_idle, uint32_t *generation)
{
    /* 资源由构建系统控制；只认固定 44 字节头且 data 块必须位于偏移 36，
     * 否则按错误采样格式驱动扬声器。其他结构合法但布局不同的 WAV 会被拒绝。 */
    if (wav == NULL || bytes < 44 || memcmp(wav, "RIFF", 4) != 0 ||
        memcmp(wav + 8, "WAVE", 4) != 0 || memcmp(wav + 36, "data", 4) != 0 ||
        read_le16(wav + 20) != 1 || read_le16(wav + 22) != 1 ||
        read_le32(wav + 24) != 16000 || read_le16(wav + 34) != 16) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t pcm_bytes = read_le32(wav + 40);
    if (pcm_bytes > bytes - 44) pcm_bytes = bytes - 44;
    pcm_bytes &= ~(size_t)1U;
    return start_local(16000, wav + 44, pcm_bytes, only_if_idle, generation);
}

esp_err_t voice_playback_write(const uint8_t *pcm, size_t bytes)
{
    if (s_task == NULL) return ESP_ERR_INVALID_STATE;
    if (pcm == NULL || bytes == 0 || (bytes & 1U) || bytes > 1200U) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t result = ESP_OK;
    size_t queued = s_buffer.size;
    uint32_t generation = s_generation, rate = s_rate;
    int64_t first_output = s_timing.first_output_us, last_output = s_last_output_us;
    bool receiving = s_active && !s_test && s_local_pcm == NULL && !s_buffer.ended;
    if (receiving) {
        s_flow.received_bytes += bytes;
        if (!s_flow.first_input_us) s_flow.first_input_us = esp_timer_get_time();
        s_flow.last_input_us = esp_timer_get_time();
    }
    if (!s_active || s_test || s_local_pcm != NULL || s_buffer.ended) {
        result = ESP_ERR_INVALID_STATE;
    } else if (!pcm_buffer_write(&s_buffer, pcm, bytes)) {
        /* 中间丢一块会破坏整句连续性，因此显式终止本轮，不能静默截断后继续。
         * 完成槽有两个写入者：这里的溢出路径与播放任务的 complete()，两者都必须在
         * s_lock 内写并携带自己的 generation，否则旧任务会覆盖新一轮的完成结果。 */
        ++s_overflows;
        s_completion_generation = s_generation;
        s_completion_result = ESP_ERR_NO_MEM;
        s_timing.completed_us = esp_timer_get_time();
        s_active = false;
        pcm_buffer_reset(&s_buffer);
        result = ESP_ERR_NO_MEM;
    } else {
        s_flow.accepted_bytes += bytes;
        s_last_input_us = esp_timer_get_time();
        if (s_buffer.size > s_high_water) s_high_water = s_buffer.size;
    }
    voice_playback_buffer_status_t flow = s_flow;
    unlock();
    if (result == ESP_ERR_NO_MEM) {
        int64_t now = esp_timer_get_time();
        ESP_LOGW(TAG, "overflow generation=%lu queued=%u capacity=%u incoming=%u rate=%lu "
                      "rx=%llu accepted=%llu dequeued=%llu i2s_bytes=%llu "
                      "configured_rate=%lu first_rx_us=%lld last_rx_us=%lld "
                      "i2s_request_us=%lld i2s_started_us=%lld first_output_us=%lld output_age_ms=%lld",
                 (unsigned long)generation, (unsigned)queued, PLAYBACK_CAPACITY_BYTES,
                 (unsigned)bytes, (unsigned long)rate,
                 (unsigned long long)flow.received_bytes, (unsigned long long)flow.accepted_bytes,
                 (unsigned long long)flow.dequeued_bytes, (unsigned long long)flow.output_bytes,
                 (unsigned long)flow.configured_rate, (long long)flow.first_input_us,
                 (long long)flow.last_input_us, (long long)flow.i2s_request_us,
                 (long long)flow.i2s_started_us, (long long)first_output,
                 (long long)(last_output ? (now-last_output)/1000 : -1));
    }
    xTaskNotifyGive(s_task);
    return result;
}

bool voice_playback_get_buffer_status(voice_playback_buffer_status_t *out)
{
    if (!s_task || !out) return false;
    lock();
    *out = s_flow;
    out->generation = s_generation;
    out->rate = s_rate;
    out->queued = s_buffer.size;
    out->capacity = s_buffer.capacity;
    out->first_output_us = s_timing.first_output_us;
    out->last_output_us = s_last_output_us;
    bool receiving = s_active && !s_test && !s_local_pcm && !s_buffer.ended;
    unlock();
    return receiving;
}

void voice_playback_finish(void)
{
    if (s_task == NULL) return;
    lock();
    pcm_buffer_end(&s_buffer);
    unlock();
    xTaskNotifyGive(s_task);
}

static bool stop_generation(uint32_t generation)
{
    if (s_task == NULL) return false;
    lock();
    /* 代次不符直接放弃：迟到的收尾不得停掉已经开始的下一轮播放。 */
    if (generation != 0 && generation != s_generation) {
        unlock();
        return false;
    }
    if (++s_generation == 0) ++s_generation;
    s_active = false;
    s_completion_generation = 0;
    pcm_buffer_reset(&s_buffer);
    s_local_pcm = NULL;
    s_local_bytes = 0;
    s_local_offset = 0;
    unlock();
    xTaskNotifyGive(s_task);
    return true;
}

void voice_playback_stop(void)
{
    (void)stop_generation(0);
}

bool voice_playback_stop_generation(uint32_t generation)
{
    return generation != 0 && stop_generation(generation);
}

bool voice_playback_generation_is_active(uint32_t generation)
{
    if (s_task == NULL || generation == 0) return false;
    lock();
    bool active = s_active && s_generation == generation;
    unlock();
    return active;
}

bool voice_playback_is_active(void)
{
    if (s_task == NULL) return false;
    lock();
    bool active = s_active;
    unlock();
    return active;
}

bool voice_playback_get_timing(uint32_t generation, voice_playback_timing_t *out)
{
    if (!s_task || !out || !generation) return false;
    lock();
    bool match = generation == s_timing.generation;
    if (match) *out = s_timing;
    unlock();
    return match;
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
    voice_playback_buffer_status_t flow = s_flow;
    s_completion_generation = 0;
    unlock();
    if (ready) ESP_LOGI(TAG, "done generation=%lu result=%s high_water=%u overflow_total=%lu "
                       "rx=%llu accepted=%llu dequeued=%llu i2s_bytes=%llu configured_rate=%lu "
                       "i2s_request_us=%lld i2s_started_us=%lld",
                       (unsigned long)*generation, esp_err_to_name(*result),
                       (unsigned)high_water, (unsigned long)overflows,
                       (unsigned long long)flow.received_bytes, (unsigned long long)flow.accepted_bytes,
                       (unsigned long long)flow.dequeued_bytes, (unsigned long long)flow.output_bytes,
                       (unsigned long)flow.configured_rate, (long long)flow.i2s_request_us,
                       (long long)flow.i2s_started_us);
    return ready;
}
