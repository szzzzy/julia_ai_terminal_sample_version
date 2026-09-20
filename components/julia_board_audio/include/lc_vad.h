#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 仅采集 owner 使用的适配层：包装 vendored 的 ESP-SR 1.9.4 WebRTC VAD。
 * 只在 CONFIG_JULIA_CAPTURE_VAD_ENABLE 编译；关闭时本地判决回到能量（以及仍开启的频谱）口径。
 * 帧格式固定为 16 kHz、320 样本（20 ms），且只接受已在采集层应用过板级增益的 PCM16；
 * 它内部保留跨帧状态，模式或连接切换必须调用 reset，否则旧状态会影响新一段的首帧判决。 */
typedef struct { void *handle; int mode; } lc_vad_t;
/* mode 为 WebRTC 的 0～3 档，数字越大判定越保守；失败只表示 VAD 不可用，不阻止采音。 */
bool lc_vad_init(lc_vad_t *v, int mode);
void lc_vad_destroy(lc_vad_t *v);
/* 复用已有实例清空内部状态，不重新分配；返回 false 由上层按致命分类器错误处理。 */
bool lc_vad_reset(void *ctx);
/* 每帧调用一次（包括低能量帧）：VAD 需要连续输入才能维持自己的噪声估计；
 * 返回 false 表示分类器出错，true 时 speech 才有效。 */
bool lc_vad_frame(void *ctx, const int16_t *pcm, bool *speech);
