#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** The sole dialogue-phase definition shared by the voice and avatar layers. */
typedef enum {
    JULIA_AVATAR_DIALOG_IDLE = 0,
    JULIA_AVATAR_DIALOG_LISTENING,
    JULIA_AVATAR_DIALOG_THINKING,
    JULIA_AVATAR_DIALOG_SPEAKING,
} julia_avatar_dialog_phase_t;

/** Build the static Julia portrait and start the L1 micro-motion task. */
esp_err_t julia_avatar_init(void);

/** Mark the beginning/end of downlink speech. Safe before UI initialization. */
void julia_avatar_talking_start(void);
void julia_avatar_talking_stop(void);

/** Feed signed, mono 16-bit speaker PCM to the RMS mouth estimator. */
void julia_avatar_feed_pcm(const int16_t *samples, size_t sample_count);

/**
 * Set the voice dialogue phase.  This may be called from WSS and command
 * queue tasks; redundant changes are ignored and all LVGL work is locked.
 */
void julia_avatar_set_dialog_phase(julia_avatar_dialog_phase_t phase);

/** Return the current dialogue phase without touching LVGL. */
julia_avatar_dialog_phase_t julia_avatar_get_dialog_phase(void);

bool julia_avatar_is_ready(void);
