/* ESP-SR 1.9.4 的 WebRTC C 入口封装：本模块只把每帧 PCM 交给 WebRTC VAD 并回传语音标志，
 * 不修改 PCM，也不承担能量、频谱或底噪判决；VAD 结果与其它门控是“与”的关系，
 * 单独为真只说明这一帧通过了 VAD，不构成起音或延续的充分条件。 */
#include "lc_vad.h"
#include "esp_vad.h"
#include <stddef.h>

/* 固定版本的 ESP-SR 1.9.4 会导出这些 WebRTC C 入口。vad_create 直接返回 WebRTC 实例；
 * 这里改用底层的 reset 以避开模式/代次切换时的内存分配，并直接调用 Process 以保留它的
 * -1 错误码（厂商的 vad_process 包装会把任何非零结果当成语音）。
 * 若升级 vendored 的 ESP-SR 版本，必须重新核对这些符号和返回值语义。 */
extern int WebRtcVad_Init(void *handle);
extern int WebRtcVad_set_mode(void *handle, int mode);
extern int WebRtcVad_Process(void *handle, int rate, const int16_t *pcm, size_t samples);

bool lc_vad_init(lc_vad_t *v, int mode)
{
    v->handle = NULL;
    v->mode = mode;
    /* mode 采用 WebRTC 的 0～3 档，数字越大判定越保守；越界时直接返回 false，
     * 失败只表示 VAD 不可用，调用方仍可继续用能量（和已开启的 FFT）口径采音。 */
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
    /* 复位复用已分配的实例（模式/连接切换、代次变化时调用），不做堆分配；
     * 返回值 false 会被上层视为分类器致命错误，而不是“本帧非语音”。 */
    return v->handle && WebRtcVad_Init(v->handle) == 0 &&
           WebRtcVad_set_mode(v->handle, v->mode) == 0;
}
bool lc_vad_frame(void *ctx, const int16_t *pcm, bool *speech)
{
    lc_vad_t *v = ctx;
    /* 先落到 false，保证任何失败路径下 speech 都有确定值，调用方不会读到未初始化内容。 */
    *speech = false;
    if (!v->handle || !pcm) return false;
    /* 帧格式固定为 16 kHz / 320 样本，即 20 ms；WebRTC VAD 只接受 10/20/30 ms 帧，
     * 改变采样率或帧长必须同步改这里和上层 20 ms 的时间轴。 */
    int result = WebRtcVad_Process(v->handle, 16000, pcm, 320);
    if (result < 0) return false;
    *speech = result > 0;
    return true;
}
