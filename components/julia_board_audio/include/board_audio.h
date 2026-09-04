#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file board_audio.h
 * @brief 管理板级 I2S MIC/Speaker，并把采集帧分发给 AFE 与 WSS 上层。
 *
 * MIC task 是采集缓冲和两个 sink callback 的唯一执行者；callback 必须快速返回，
 * 需要异步使用数据时应在返回前复制。Speaker 的 start/write/stop/self-test 由内部
 * mutex 串行化，但正常播放顺序和尾音排空仍由上层播放 owner 负责。
 *
 * 本组件不做回声消除。播放期间 AFE 仍接收 MIC，因此采样可能包含扬声器回声；
 * 不能把持续采集描述成云端一定能够过滤回声。
 */

/** 16 kHz mono PCM16；指针只在 MIC task 本次 callback 返回前有效。 */
typedef void (*audio_pcm_sink_t)(const int16_t *pcm, size_t samples, void *ctx);

/** 完整 PCM1 消息；内部缓冲会被下一帧复用，callback 必须同步复制。 */
typedef void (*audio_frame_sink_t)(const uint8_t *frame, size_t bytes, void *ctx);

/** 初始化两个 I2S 通道并启动唯一 MIC task；重复调用不创建第二个采集任务。 */
esp_err_t board_audio_init(void);

/* ---- MIC 上行 fanout（§8.4） ---- */

/** 注册 AFE sink；MICS/MICW 和 WSS 上行开关均不暂停该路径。 */
esp_err_t board_audio_set_afe_sink(audio_pcm_sink_t sink, void *ctx);

/** 注册 PCM1 sink；是否上传由语音会话策略调用 board_audio_enable_wss_mic() 决定。 */
esp_err_t board_audio_set_wss_sink(audio_frame_sink_t sink, void *ctx);

/** 打开/关闭 WSS 上行。关闭时复位触发/预录状态，AFE sink 不受影响。 */
void board_audio_enable_wss_mic(bool enabled);

/** 进入门限触发模式；background 单位为 0.01 dBFS，当前阈值固定高于背景 5 dB。 */
void board_audio_mic_sleep(int16_t background_dbfs_x100);

/** 退出门限触发模式；是否实际上传仍受 WSS 上行开关控制。 */
void board_audio_mic_wake(void);

/* ---- Speaker 下行（§8.3 / §9.4） ---- */

/** 开始或接管播放；0 使用 24 kHz，已有播放会先停止，因此调用成功会使旧流失效。 */
esp_err_t board_audio_speaker_start(uint32_t sample_rate);

/** 同步写 mono PCM16；长度必须为偶数且不超过 4096 B，只供播放 owner 调用。 */
esp_err_t board_audio_speaker_write(const uint8_t *mono_pcm, size_t bytes);

/** 立即停播并释放 I2S TX，丢弃残留 DMA；正常结束须由播放任务先排空尾音。 */
esp_err_t board_audio_speaker_stop(void);

/** 音量 0-100（越界钳位）。 */
void board_audio_speaker_set_volume(uint8_t percent);

/** 返回 playing 标志的瞬时快照，不表示 DMA 已排空或声音已实际播放完成。 */
bool board_audio_speaker_is_playing(void);

/** 独占 Speaker 播放 440/660/880 Hz 测试音；会打断其它播放源。 */
esp_err_t board_audio_speaker_self_test(void);

#ifdef __cplusplus
}
#endif
