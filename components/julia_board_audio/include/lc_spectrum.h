#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 320-point periodic Hann, 50..8000 Hz (bins 1..160), single-sided PSD.
 * Candidate envelope from S0002; these are not validated noise thresholds. */
#define LC_FLATNESS_MAX 0.199f
#define LC_ENTROPY_MIN 0.070f
#define LC_ENTROPY_MAX 0.809f
typedef struct {
    float flatness, entropy, band_300_4000_ratio;
    bool valid;
} lc_spectral_features_t;
typedef struct {
    float window[320], twiddle[640], fft[640], psd[161];
    float window_power;
    bool ready;
} lc_spectrum_t;
/* Initialization only; buffers belong to the capture owner, never to callbacks. */
bool lc_spectrum_init(lc_spectrum_t *s);
bool lc_spectrum_features(lc_spectrum_t *s, const int16_t pcm[320],
                          lc_spectral_features_t *features);
/* Exposed for numerical boundary tests. Invalid/zero PSD yields finite zeros. */
bool lc_spectrum_from_psd(const float psd[161], lc_spectral_features_t *features);
bool lc_spectrum_accept(const lc_spectral_features_t *features);
