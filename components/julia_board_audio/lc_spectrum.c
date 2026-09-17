#include "lc_spectrum.h"
#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "dsps_fft2r.h"
#endif

bool lc_spectrum_init(lc_spectrum_t *s)
{
    memset(s, 0, sizeof(*s));
    for (unsigned i = 0; i < 320; ++i) {
        /* Round coefficients once; all per-frame arithmetic remains float32. */
        double angle = 6.2831853071795864769 * i / 320.0;
        s->twiddle[2*i] = (float)cos(angle);
        s->twiddle[2*i+1] = (float)-sin(angle);
        s->window[i] = (float)(0.5 - 0.5 * cos(angle));
        s->window_power += s->window[i] * s->window[i];
    }
#ifdef ESP_PLATFORM
    /* DSP owns its shared table; initialize once, never deinitialize here.
     * ANSI kernel supports our PSRAM buffers without SIMD alignment assumptions. */
    if (dsps_fft2r_init_fc32(NULL, 64) != ESP_OK || dsps_fft_w_table_size < 64)
        return false;
#endif
    s->ready = true;
    return true;
}

static bool fft64(float *x, const float *w)
{
#ifdef ESP_PLATFORM
    (void)w;
    return dsps_fft2r_fc32_ansi(x, 64) == ESP_OK &&
           dsps_bit_rev_fc32_ansi(x, 64) == ESP_OK;
#else
    /* Portable host radix-2 reference, same single-precision data flow. */
    for (unsigned i = 1, j = 0; i < 64; ++i) {
        unsigned bit = 32;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float re = x[2*i], im = x[2*i+1];
            x[2*i] = x[2*j]; x[2*i+1] = x[2*j+1];
            x[2*j] = re; x[2*j+1] = im;
        }
    }
    for (unsigned n = 2; n <= 64; n *= 2)
        for (unsigned base = 0; base < 64; base += n)
            for (unsigned j = 0; j < n/2; ++j) {
                unsigned a = 2*(base+j), b = a+n, t = 2*j*(320/n);
                float re = x[b]*w[t] - x[b+1]*w[t+1];
                float im = x[b]*w[t+1] + x[b+1]*w[t];
                x[b] = x[a]-re; x[b+1] = x[a+1]-im;
                x[a] += re; x[a+1] += im;
            }
    return true;
#endif
}

bool lc_spectrum_from_psd(const float p[161], lc_spectral_features_t *f)
{
    memset(f, 0, sizeof(*f));
    float total = 0, sum = 0, logs = 0, band = 0;
    for (unsigned k = 0; k <= 160; ++k) {
        if (!isfinite(p[k]) || p[k] < 0) return false;
        total += p[k];
        if (k >= 6 && k < 80) band += p[k];
        if (k) { sum += p[k]; logs += logf(fmaxf(p[k], 1e-30f)); }
    }
    if (!(sum > 0) || !isfinite(total)) return false;
    float entropy = 0;
    for (unsigned k = 1; k <= 160; ++k) {
        float q = p[k]/sum;
        if (q > 0) entropy -= q*logf(q);
    }
    f->flatness = expf(logs/160.0f)/(sum/160.0f);
    f->entropy = entropy/logf(160.0f);
    f->band_300_4000_ratio = band/total;
    f->valid = isfinite(f->flatness) && isfinite(f->entropy);
    if (!f->valid) memset(f, 0, sizeof(*f));
    return f->valid;
}

bool lc_spectrum_features(lc_spectrum_t *s, const int16_t pcm[320],
                          lc_spectral_features_t *f)
{
    memset(f, 0, sizeof(*f));
    if (!s->ready || !pcm) return false;
    /* Integer sum is exact and cannot overflow for 320 PCM16 samples. */
    int32_t sum = 0;
    for (unsigned i = 0; i < 320; ++i) sum += pcm[i];
    float mean = sum / (320.0f * 32768.0f);
    /* n=5*m+r: five 64-point FFTs, then X[k]=sum_r A_r[k%64] W320^(r*k).
     * This is an exact 320-point transform, not a 512-point zero-padded FFT. */
    for (unsigned r = 0; r < 5; ++r) {
        float *a = s->fft + 128*r;
        for (unsigned m = 0; m < 64; ++m) {
            unsigned n = 5*m+r;
            a[2*m] = (pcm[n]/32768.0f-mean)*s->window[n];
            a[2*m+1] = 0;
        }
        if (!fft64(a, s->twiddle)) return false;
    }
    for (unsigned k = 0; k <= 160; ++k) {
        float re = 0, im = 0;
        for (unsigned r = 0; r < 5; ++r) {
            unsigned a = 128*r + 2*(k%64), t = 2*((r*k)%320);
            re += s->fft[a]*s->twiddle[t] - s->fft[a+1]*s->twiddle[t+1];
            im += s->fft[a]*s->twiddle[t+1] + s->fft[a+1]*s->twiddle[t];
        }
        /* Real input has exactly real DC/Nyquist coefficients. */
        if (k == 0 || k == 160) im = 0;
        s->psd[k] = (re*re+im*im)/(16000.0f*s->window_power)*
                    ((k == 0 || k == 160) ? 1.0f : 2.0f);
    }
    return lc_spectrum_from_psd(s->psd, f);
}

bool lc_spectrum_accept(const lc_spectral_features_t *f)
{
    return f->valid && isfinite(f->flatness) && isfinite(f->entropy) &&
           f->flatness >= 0 && f->flatness <= LC_FLATNESS_MAX &&
           f->entropy >= LC_ENTROPY_MIN && f->entropy <= LC_ENTROPY_MAX;
}
