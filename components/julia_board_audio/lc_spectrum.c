/* S0002 320 点频谱包络特征：对 16 kHz、20 ms（320 样本）的单声道 PCM16 帧计算单边 PSD，
 * 再导出谱平坦度、归一化谱熵、两个频带能量占比和谱质心。 */
/* 本文件只做特征与门限判定，不修改、不增益也不降噪 PCM；缓冲区全部来自采集 owner 传入的
 * lc_spectrum_t，因此只允许在该 owner 的上下文中调用。 */
/* 频率换算：采样率 16 kHz、变换长度 320，bin k 的中心频率为 k×16000/320＝k×50 Hz，
 * 结果只取 bin 0（DC）～160（Nyquist，8000 Hz），即正频率半边，k>0 时按单边谱乘 2 补偿。 */
/* 使用的变换是精确的 320 点 DFT（分解为 5 个 64 点 FFT 再做旋转因子重组），不是 512 点补零 FFT，
 * 因此没有补零带来的频率泄漏差异；去均值和周期 Hann 窗在每帧内完成，窗能量归一化参与 PSD。 */
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
        /* 旋转因子与窗系数只在初始化时用 double 计算一次并取整为 float，
         * 之后每帧的运算保持 float32，避免同一帧内混用两种精度。 */
        double angle = 6.2831853071795864769 * i / 320.0;
        s->twiddle[2*i] = (float)cos(angle);
        s->twiddle[2*i+1] = (float)-sin(angle);
        s->window[i] = (float)(0.5 - 0.5 * cos(angle));
        s->window_power += s->window[i] * s->window[i];
    }
#ifdef ESP_PLATFORM
    /* DSP 库的旋转因子表是全局共享资源，只初始化、不在本模块反初始化，避免影响同进程其他使用者；
     * 选择 ANSI 实现是因为它在使用 PSRAM 缓冲区时不依赖 SIMD 对齐假设。 */
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
    /* 主机侧的 radix-2 参考实现，与固件路径保持同样的单精度运算顺序，只用于回归对照。 */
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
    /* 各频带按 bin 索引累加，乘以 50 Hz/bin 即得频率：DC 用 bin 0，高频带从 bin 20（1 kHz）起，
     * 300–4000 Hz 名义带实际累加 bin 6..79，即 300～3950 Hz，不含 4000 Hz 那个 bin。 */
    float total = 0, sum = 0, logs = 0, band = 0, high = 0, weighted = 0;
    for (unsigned k = 0; k <= 160; ++k) {
        if (!isfinite(p[k]) || p[k] < 0) return false;
        total += p[k];
        if (k >= 20) high += p[k];
        weighted += p[k] * (50.0f * k);
        if (k >= 6 && k < 80) band += p[k];
        if (k) { sum += p[k]; logs += logf(fmaxf(p[k], 1e-30f)); }
    }
    if (!(sum > 0) || !isfinite(total)) return false;
    /* 谱熵与平坦度都排除 DC（k=0）：直流分量只反映帧内均值，与语音/噪声区分无关；
     * 熵按 log(160) 归一化，平坦度用几何均值/算术均值，两者域内取值均为 0～1。 */
    float entropy = 0;
    for (unsigned k = 1; k <= 160; ++k) {
        float q = p[k]/sum;
        if (q > 0) entropy -= q*logf(q);
    }
    f->flatness = expf(logs/160.0f)/(sum/160.0f);
    f->entropy = entropy/logf(160.0f);
    f->band_300_4000_ratio = band/total;
    f->band_1000_8000_ratio = high/total;
    f->centroid_hz = weighted/total;
    f->valid = isfinite(f->flatness) && isfinite(f->entropy) &&
               isfinite(f->centroid_hz) && isfinite(f->band_1000_8000_ratio);
    if (!f->valid) memset(f, 0, sizeof(*f));
    return f->valid;
}

bool lc_spectrum_features(lc_spectrum_t *s, const int16_t pcm[320],
                          lc_spectral_features_t *f)
{
    memset(f, 0, sizeof(*f));
    if (!s->ready || !pcm) return false;
    /* 整数求和精确且 320 个 PCM16 样本不会溢出 int32；去均值后直流分量接近 0，
     * 避免大 DC 偏移抬高低频带占比。 */
    int32_t sum = 0;
    for (unsigned i = 0; i < 320; ++i) sum += pcm[i];
    float mean = sum / (320.0f * 32768.0f);
    /* n=5*m+r：先做 5 个 64 点 FFT，再按 X[k]=Σ_r A_r[k%64]·W320^(r·k) 重组，
     * 得到精确的 320 点变换，而不是 512 点补零 FFT。 */
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
        /* 实数输入的 DC 与 Nyquist 系数虚部恒为 0，显式清零可避免重组后的浮点残差。 */
        if (k == 0 || k == 160) im = 0;
        /* 单边 PSD 归一化：分母为采样率（16 kHz，来自本模块的输入契约）与窗能量之积，
         * 除 DC/Nyquist 外乘 2 补回负频率的能量，因此各频带占比可直接在 0～1 之间比较。 */
        s->psd[k] = (re*re+im*im)/(16000.0f*s->window_power)*
                    ((k == 0 || k == 160) ? 1.0f : 2.0f);
    }
    return lc_spectrum_from_psd(s->psd, f);
}

bool lc_spectrum_accept(const lc_spectral_features_t *f)
{
    /* 判据含义：平坦度与熵都在 0～1 之间，平坦度越接近 1 谱形越接近均匀噪声，熵越低能量越集中；
     * 这里要求平坦度不高于上限、熵落在区间内，两侧都取闭区间，等于门限即算通过。
     * 三个门限是 S0002 数据得到的候选值，跨设备泛化依据未确认，需上板标定。 */
    return f->valid && isfinite(f->flatness) && isfinite(f->entropy) &&
           f->flatness >= 0 && f->flatness <= LC_FLATNESS_MAX &&
           f->entropy >= LC_ENTROPY_MIN && f->entropy <= LC_ENTROPY_MAX;
}
