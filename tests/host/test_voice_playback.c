/* Deterministic scheduling of the real worker, with only RTOS/I2S/time mocked.
 * Injection during a write exercises cancellation and stale completion fences.
 * This is not an I2S/DMA timing or multicore stress test. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "freertos/task.h"
#include "voice_playback.h"

static void (*worker)(void *);
static jmp_buf finished;
static int64_t now_us;
static unsigned waits, starts, stops, writes, pcm_callbacks;
static unsigned nonzero_samples, old_samples, new_samples;
static uint32_t current_generation, completion_generation;
static esp_err_t completion_result;
static bool inject_cancel, inject_gap, fail_write, was_injected;
static bool expect_no_completion;
static int16_t old_pcm[320], new_pcm[160];
static int64_t first_write_us;

int64_t esp_timer_get_time(void) { return now_us; }

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack,
                                 void *arg, unsigned priority, TaskHandle_t *out, int core)
{
    (void)name; (void)stack; (void)arg; (void)priority; (void)core;
    worker = fn;
    *out = (void *)1;
    return pdPASS;
}

esp_err_t board_audio_speaker_start(uint32_t rate)
{
    assert(rate == 16000 || rate == 24000);
    ++starts;
    return ESP_OK;
}

esp_err_t board_audio_speaker_stop(void) { ++stops; return ESP_OK; }

esp_err_t board_audio_speaker_write(const uint8_t *pcm, size_t bytes)
{
    ++writes;
    assert(bytes <= 320 && (bytes & 1U) == 0);
    if (fail_write) return ESP_FAIL;
    const int16_t *samples = (const int16_t *)pcm;
    for (size_t i = 0; i < bytes / 2; ++i) {
        if (samples[i] != 0) ++nonzero_samples;
        if (samples[i] == 111) ++old_samples;
        if (samples[i] == 222) ++new_samples;
    }
    if (first_write_us == 0) first_write_us = now_us;
    now_us += 10000; /* one bounded I2S transfer */
    if (inject_cancel && !was_injected) {
        was_injected = true;
        voice_playback_stop();
        assert(voice_playback_start(24000, false, &current_generation) == ESP_OK);
        assert(voice_playback_write((const uint8_t *)new_pcm, sizeof(new_pcm)) == ESP_OK);
        voice_playback_finish();
    }
    return ESP_OK;
}

static void on_pcm(const int16_t *pcm, size_t count, void *ctx)
{
    (void)pcm; (void)ctx;
    assert(count <= 160);
    ++pcm_callbacks;
}

unsigned ulTaskNotifyTake(int clear, TickType_t wait)
{
    (void)clear;
    assert(++waits < 5000);
    if (wait == portMAX_DELAY) {
        bool completed = voice_playback_take_completion(&completion_generation, &completion_result);
        assert(completed != expect_no_completion);
        longjmp(finished, 1);
    }
    now_us += (int64_t)wait * 1000;
    if (inject_gap && !was_injected && now_us >= 2000000) {
        was_injected = true;
        assert(voice_playback_is_active()); /* still open after a 1-second gap */
        assert(voice_playback_write((const uint8_t *)new_pcm, sizeof(new_pcm)) == ESP_OK);
        voice_playback_finish();
    }
    return 0;
}

static void reset(void)
{
    voice_playback_stop();
    now_us = 1000000;
    waits = starts = stops = writes = pcm_callbacks = 0;
    old_samples = new_samples = nonzero_samples = 0;
    inject_cancel = inject_gap = fail_write = was_injected = expect_no_completion = false;
    first_write_us = 0;
    completion_generation = 0;
    completion_result = -999;
}

static void run(void)
{
    if (setjmp(finished) == 0) worker(NULL);
}

int main(void)
{
    for (unsigned i = 0; i < 320; ++i) old_pcm[i] = 111;
    for (unsigned i = 0; i < 160; ++i) new_pcm[i] = 222;
    assert(voice_playback_init(on_pcm, NULL) == ESP_OK);

    reset();
    assert(voice_playback_start(16000, false, &current_generation) == ESP_OK);
    assert(voice_playback_write((const uint8_t *)old_pcm, sizeof(old_pcm)) == ESP_OK);
    voice_playback_finish();
    run();
    assert(old_samples == 320 && writes == 7); /* two PCM + five DMA drain chunks */
    assert(first_write_us == 1000000 && completion_result == ESP_OK);
    assert(completion_generation == current_generation && stops == 1);
    puts("PASS: short END drains the entire tail without prebuffer delay");

    reset();
    inject_cancel = true;
    assert(voice_playback_start(16000, false, &current_generation) == ESP_OK);
    assert(voice_playback_write((const uint8_t *)old_pcm, sizeof(old_pcm)) == ESP_OK);
    voice_playback_finish();
    run();
    assert(old_samples == 160 && new_samples == 160); /* remaining old chunk cancelled */
    assert(starts == 2 && pcm_callbacks == 1 && stops == 2);
    assert(completion_generation == current_generation && completion_result == ESP_OK);
    puts("PASS: interrupt/restart suppresses old buffered PCM and stale completion");

    reset();
    inject_gap = true;
    assert(voice_playback_start(24000, false, &current_generation) == ESP_OK);
    assert(voice_playback_write((const uint8_t *)old_pcm, 320) == ESP_OK);
    run();
    assert(first_write_us >= 1120000 && first_write_us < 1200000);
    assert(old_samples == 160 && new_samples == 160 && completion_result == ESP_OK);
    puts("PASS: bounded prebuffer wait and recovery after a one-second gap");

    reset();
    assert(voice_playback_start(24000, false, &current_generation) == ESP_OK);
    run();
    assert(completion_result == ESP_ERR_TIMEOUT && now_us >= 16000000);
    puts("PASS: no-input starvation returns an explicit timeout");

    reset();
    assert(voice_playback_start(16000, false, &current_generation) == ESP_OK);
    unsigned accepted = 0;
    while (voice_playback_write((const uint8_t *)old_pcm, sizeof(old_pcm)) == ESP_OK) ++accepted;
    assert(accepted == 102 && !voice_playback_is_active());
    run();
    assert(writes == 0 && completion_result == ESP_ERR_NO_MEM);
    puts("PASS: overflow aborts explicitly without silently dropping a middle packet");

    reset();
    fail_write = true;
    assert(voice_playback_start(16000, false, &current_generation) == ESP_OK);
    assert(voice_playback_write((const uint8_t *)old_pcm, sizeof(old_pcm)) == ESP_OK);
    voice_playback_finish();
    run();
    assert(writes == 1 && completion_result == ESP_FAIL && stops == 1);
    puts("PASS: I2S failure terminates and reports the active generation");

    reset();
    assert(voice_playback_start(24000, true, &current_generation) == ESP_OK);
    voice_playback_finish();
    run();
    assert(nonzero_samples == 0 && completion_result == ESP_OK);
    puts("PASS: END can stop an asynchronous self-test before tone generation");
    return 0;
}
