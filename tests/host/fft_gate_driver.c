/* ctypes bridge: execute production capture and verify every emitted PCM byte. */
#include "local_capture.h"
#include <assert.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
static local_capture_t capture;
static int16_t history[1024][320];
static int position, begin, reject_event;
static unsigned audio;
static int result[10];
static bool output(void *ctx, const lc_record_t *r)
{
    (void)ctx;
    if ((int)r->event == reject_event) return false;
    if (r->event == LC_START) {
        result[0]++; begin = position-(int)capture.pre_count+1; audio = 0;
        result[6] = begin;
    }
    if (r->event == LC_AUDIO) {
        assert(r->index == audio);
        assert(!memcmp(r->pcm, history[(begin+audio)%1024], 640));
        ++audio; result[1]++;
    }
    if (r->event == LC_END) {
        assert(r->index == audio);
        result[2]++; result[3] = r->limit; result[7] = audio;
    }
    if (r->event == LC_ABORT) result[4]++;
    return true;
}
EXPORT void gate_reset(int enabled, int mode)
{
    lc_init(&capture, output, NULL);
    capture.fft_enabled = enabled != 0;
    lc_set_mode(&capture, (lc_mode_t)mode);
    position = -1; reject_event = -1;
    memset(result, 0, sizeof(result));
}
EXPORT void gate_mode(int mode) { lc_set_mode(&capture, (lc_mode_t)mode); }
EXPORT void gate_reject(int event) { reject_event = event; }
EXPORT void gate_unready(void) { capture.spectrum.ready = false; }
EXPORT int gate_step(const int16_t *pcm, int *out)
{
    ++position;
    memcpy(history[position%1024], pcm, 640);
    int ok = lc_process(&capture, pcm, (position+1)*20);
    result[5] = capture.active;
    result[8] = capture.silence;
    result[9] = capture.frames;
    memcpy(out, result, sizeof(result));
    return ok;
}
EXPORT int gate_features(const int16_t *pcm, float *out)
{
    lc_spectral_features_t f;
    bool valid = lc_spectrum_features(&capture.spectrum, pcm, &f);
    out[0] = f.flatness; out[1] = f.entropy; out[2] = f.band_300_4000_ratio;
    return valid;
}
EXPORT int gate_psd(const float *psd, float *out)
{
    lc_spectral_features_t f;
    bool valid = lc_spectrum_from_psd(psd, &f);
    out[0] = f.flatness; out[1] = f.entropy; out[2] = f.band_300_4000_ratio;
    return valid;
}
