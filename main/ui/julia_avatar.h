#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Build the static Julia portrait and start the L1 micro-motion task. */
esp_err_t julia_avatar_init(void);

/** Mark the beginning/end of downlink speech. Safe before UI initialization. */
void julia_avatar_talking_start(void);
void julia_avatar_talking_stop(void);

/** Feed signed, mono 16-bit speaker PCM to the RMS mouth estimator. */
void julia_avatar_feed_pcm(const int16_t *samples, size_t sample_count);

bool julia_avatar_is_ready(void);
