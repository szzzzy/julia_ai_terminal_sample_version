#pragma once

#include "board_audio.h"

/* All control APIs only touch the bounded buffer; I2S belongs to the worker. */
esp_err_t voice_playback_init(audio_pcm_sink_t pcm_sink, void *ctx);
esp_err_t voice_playback_start(uint32_t rate, bool self_test, uint32_t *generation);
esp_err_t voice_playback_write(const uint8_t *pcm, size_t bytes);
void voice_playback_finish(void); /* Drain accepted PCM, then report completion. */
void voice_playback_stop(void);   /* Cancel queued PCM, interrupt the worker. */
bool voice_playback_is_active(void);
bool voice_playback_take_completion(uint32_t *generation, esp_err_t *result);
