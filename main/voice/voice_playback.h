#pragma once

#include "board_audio.h"

/**
 * @file voice_playback.h
 * @brief 把服务器回答平稳地送到扬声器，并区分“已接收”和“已真正播放完成”。
 *
 * 调用方只负责开始一次播放、追加声音和声明输入结束。独立播放任务拥有扬声器，
 * 会先积累少量声音防止开头断续，再按采样率持续输出；缓冲耗尽、超时、溢出或
 * 被用户打断时都产生明确结果。旧播放的完成通知不会影响新一轮回答。
 */

/** 准备播放任务，并登记每个已播放声音块的通知函数。 */
esp_err_t voice_playback_init(audio_pcm_sink_t pcm_sink, void *ctx);
/** 开始一轮回答或扬声器自检，返回本轮编号用于拒绝迟到的旧结果。 */
esp_err_t voice_playback_start(uint32_t rate, bool self_test, uint32_t *generation);
/** 直接播放生命周期覆盖整个应用的本地 PCM16 资源，不占用网络抖动缓冲。 */
esp_err_t voice_playback_start_local(uint32_t rate, const uint8_t *pcm, size_t bytes,
                                     uint32_t *generation);
/** 追加一块服务器回答声音；缓冲区已满时明确返回错误。 */
esp_err_t voice_playback_write(const uint8_t *pcm, size_t bytes);
/** 声明服务器已经发完；设备仍会播完已接收声音和扬声器尾音后才报告完成。 */
void voice_playback_finish(void);
/** 立即取消尚未播放的声音并停止扬声器，用于用户插话或连接断开。 */
void voice_playback_stop(void);
/** 查询是否仍有一轮回答正在准备或播放。 */
bool voice_playback_is_active(void);
/** 取得一次尚未处理的播放结果；返回 false 表示没有新的完成或失败。 */
bool voice_playback_take_completion(uint32_t *generation, esp_err_t *result);
