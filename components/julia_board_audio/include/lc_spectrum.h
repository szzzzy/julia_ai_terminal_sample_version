#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 16 kHz、320 样本（20 ms）帧的频谱包络门限：周期 Hann 窗、单边 PSD、bin 宽 50 Hz，
 * 只使用 bin 1～160（50～8000 Hz）。三个门限是 S0002 语料得到的候选值，
 * 尚未用独立标注的实机录音验证，需上板标定；关闭 CONFIG_JULIA_CAPTURE_FFT_GATE 即不使用。 */
#define LC_FLATNESS_MAX 0.199f
#define LC_ENTROPY_MIN 0.070f
#define LC_ENTROPY_MAX 0.809f
typedef struct {
    /* band_300_4000_ratio 名义为 300–4000 Hz 的能量占比，但实现按 bin 6..79 求和（即 300～3950 Hz），
     * 4000 Hz 那个 bin 未被计入；该边界差异尚未确认是否有意，改动前先确认调用方口径。 */
    float flatness, entropy, band_300_4000_ratio;
    /* 单边 PSD 分母为 bin 0..160（含 DC 与 Nyquist）的总能量；
     * 高频带为闭区间 1000..8000 Hz，即 bin 20..160。 */
    float band_1000_8000_ratio, centroid_hz;
    bool valid;
} lc_spectral_features_t;
typedef struct {
    float window[320], twiddle[640], fft[640], psd[161];
    float window_power;
    bool ready;
} lc_spectrum_t;
/* 只做初始化；缓冲区属于采集 owner，任何回调都不得借用或长期持有该结构。 */
bool lc_spectrum_init(lc_spectrum_t *s);
/* 输入必须是恰好 320 个样本的单声道 PCM16；返回 false 表示该帧无有效特征，
 * 调用方应把它当作“谱门控不通过”，而不是把 features 当零值继续使用。 */
bool lc_spectrum_features(lc_spectrum_t *s, const int16_t pcm[320],
                          lc_spectral_features_t *features);
/* 供数值边界测试直接注入 PSD。输入非法或全零时输出全零且 valid=false 的有限值。 */
bool lc_spectrum_from_psd(const float psd[161], lc_spectral_features_t *features);
bool lc_spectrum_accept(const lc_spectral_features_t *features);
