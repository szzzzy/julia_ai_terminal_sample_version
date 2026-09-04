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

/**
 * 创建唯一播放 Task 和 64 KiB PSRAM 缓冲。pcm_sink 在播放 Task 中同步调用，收到
 * 的 PCM 只在回调期间有效；回调不得阻塞或反向调用播放控制接口。
 */
esp_err_t voice_playback_init(audio_pcm_sink_t pcm_sink, void *ctx);
/** 开始网络回答或异步自检；新代次会取消旧播放，generation 用于隔离迟到结果。 */
esp_err_t voice_playback_start(uint32_t rate, bool self_test, uint32_t *generation);
/**
 * 播放只读的本地 PCM16，并使当前网络播放代次失效。数据不复制到 64 KiB 网络
 * 缓冲，因此 pcm 必须保持有效直至完成或取消；仅适合固件内嵌等静态资源。
 * bytes 必须为非零偶数，rate 只接受 16/24 kHz。
 */
esp_err_t voice_playback_start_local(uint32_t rate, const uint8_t *pcm, size_t bytes,
                                     uint32_t *generation);
/**
 * 非阻塞复制一块 PCM 到网络缓冲；单块不超过 1200 B。缓冲满会终止整轮播放并
 * 返回 ESP_ERR_NO_MEM，不能在错误后继续追加残缺语音。
 */
esp_err_t voice_playback_write(const uint8_t *pcm, size_t bytes);
/** 声明服务器已经发完；设备仍会播完已接收声音和扬声器尾音后才报告完成。 */
void voice_playback_finish(void);
/** 立即取消尚未播放的声音并停止扬声器，用于用户插话或连接断开。 */
void voice_playback_stop(void);
/** 查询是否仍有一轮回答正在准备或播放。 */
bool voice_playback_is_active(void);
/**
 * 取走一个完成结果；结果只有一个消费槽，调用成功后即清除。上层必须以 generation
 * 拒绝旧代次，且只能指定一个结果消费者。
 */
bool voice_playback_take_completion(uint32_t *generation, esp_err_t *result);
