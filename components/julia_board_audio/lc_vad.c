#include "lc_vad.h"
#include "esp_vad.h"
#include <stddef.h>

/* Pinned ESP-SR 1.9.4 exports these WebRTC C entry points. vad_create returns
 * the WebRTC instance directly (verified in esp_vad.c.obj). Use the underlying
 * reset to avoid allocation on mode/epoch changes, and Process to preserve its
 * -1 error (the vendor vad_process wrapper turns any nonzero result into speech).
 * Revalidate this adapter if the vendored ESP-SR version changes. */
extern int WebRtcVad_Init(void *handle);
extern int WebRtcVad_set_mode(void *handle, int mode);
extern int WebRtcVad_Process(void *handle, int rate, const int16_t *pcm, size_t samples);

bool lc_vad_init(lc_vad_t *v, int mode)
{
    v->handle = NULL;
    v->mode = mode;
    if (mode < 0 || mode > 3) return false;
    v->handle = vad_create((vad_mode_t)mode);
    return v->handle != NULL;
}
void lc_vad_destroy(lc_vad_t *v)
{
    if (v->handle) vad_destroy(v->handle);
    v->handle = NULL;
}
bool lc_vad_reset(void *ctx)
{
    lc_vad_t *v = ctx;
    return v->handle && WebRtcVad_Init(v->handle) == 0 &&
           WebRtcVad_set_mode(v->handle, v->mode) == 0;
}
bool lc_vad_frame(void *ctx, const int16_t *pcm, bool *speech)
{
    lc_vad_t *v = ctx;
    *speech = false;
    if (!v->handle || !pcm) return false;
    int result = WebRtcVad_Process(v->handle, 16000, pcm, 320);
    if (result < 0) return false;
    *speech = result > 0;
    return true;
}
