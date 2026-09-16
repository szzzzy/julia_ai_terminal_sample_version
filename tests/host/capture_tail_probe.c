/* 对同一组输入运行实际收音核心，可分别链接修改前后的实现，比较结束时机。 */
#include "local_capture.h"
#include <assert.h>
#include <stdio.h>

static local_capture_t capture;
static int elapsed_ms;
static bool ended, limited;

static bool output(void *ctx, const lc_record_t *record)
{
    (void)ctx;
    if (record->event == LC_END) {
        ended = true;
        limited = record->limit;
    }
    return true;
}

static void frame(int value)
{
    int16_t pcm[LC_FRAME_SAMPLES];
    for (unsigned i = 0; i < LC_FRAME_SAMPLES; ++i) pcm[i] = (int16_t)value;
    elapsed_ms += 20;
    assert(lc_process(&capture, pcm, elapsed_ms));
}

static void scenario(const char *name, unsigned spike_period)
{
    lc_init(&capture, output, NULL);
    lc_set_mode(&capture, LC_DIALOG);
    elapsed_ms = 0; ended = limited = false;
    for (unsigned i = 0; i < 6; ++i) frame(200);
    assert(capture.active);
    int speech_end_ms = elapsed_ms;
    for (unsigned i = 0; i < LC_MAX_FRAMES && !ended; ++i)
        frame(spike_period && i % spike_period == spike_period - 1 ? 200 : 1);
    assert(ended);
    printf("%s tail_ms=%d segment_ms=%u reason=%s\n", name,
           elapsed_ms - speech_end_ms, (unsigned)capture.frames * 20,
           limited ? "limit" : "silence");
}

static void hesitant_speech(void)
{
    lc_init(&capture, output, NULL);
    lc_set_mode(&capture, LC_DIALOG);
    elapsed_ms = 0; ended = limited = false;
    for (unsigned i = 0; i < 6; ++i) frame(200);
    int pattern_start_ms = elapsed_ms;
    /* 对比长停顿之间只有短语音时的代价，不将合成波形当作真实人声或空调测量。 */
    for (unsigned pause = 0; pause < 3 && !ended; ++pause) {
        for (unsigned i = 0; i < 30 && !ended; ++i) frame(1);
        for (unsigned i = 0; i < 8 && !ended; ++i) frame(200);
    }
    printf("600ms_pause_160ms_voice ended_during_pattern=%s elapsed_ms=%d\n",
           ended ? "true" : "false", elapsed_ms - pattern_start_ms);
}

int main(void)
{
    scenario("quiet_tail", 0);
    scenario("20ms_spike_every_200ms", 10);
    scenario("20ms_spike_every_100ms", 5);
    scenario("continuous_above_threshold", 1);
    hesitant_speech();
    return 0;
}
