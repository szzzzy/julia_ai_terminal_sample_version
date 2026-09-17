#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Capture-owner-only adapter for the vendored ESP-SR 1.9.4 WebRTC VAD. */
typedef struct { void *handle; int mode; } lc_vad_t;
bool lc_vad_init(lc_vad_t *v, int mode);
void lc_vad_destroy(lc_vad_t *v);
bool lc_vad_reset(void *ctx);
bool lc_vad_frame(void *ctx, const int16_t *pcm, bool *speech);
