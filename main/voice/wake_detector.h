/**
 * @file    wake_detector.h
 * @brief   本地唤醒词检测（WakeNet "你好小智"）的对外接口。
 *
 * 职责边界：
 * - 只做"本地唤醒"：把板级 20ms PCM 帧喂给 ESP-SR AFE，AFE 在设备侧推理；
 * - 检测到唤醒词 -> 自动调用 voice_service_mic_start()（等效服务器下发的
 *   MIC_START 效果），把麦克风流推给服务器，由服务器做 ASR/LLM/TTS，
 *   回推的 PCM 由 voice_service 播报——本模块不参与识别/生成/播放；
 * - 不接 FSM、不做对话相位、不采集音频、不写 NVS。
 *
 * 线程模型：
 * - wake_afe_feed 由 board_audio 的 mic_task 上下文每 20ms 回调一次（数据源）；
 * - wake_detect_task 常驻 core1，从 AFE fetch 结果中找唤醒事件并触发动作；
 * - wake_detector_init 在 app 装配阶段（app_main）调用，做一次性初始化。
 *
 * 依赖：
 * - 必须在 board_audio_init() 与 voice_service_init() 之后调用（wake 需要
 *   board_audio 的 AFE sink 与 voice_service 的 mic_start 通道）；
 * - 依赖 "model" 分区已烧录（构建时 esp-sr 自动打包 srmodels.bin）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化本地唤醒词检测（WakeNet "你好小智"，wn9_nihaoxiaozhi_tts）。
 *
 * 过程：初始化 AFE/WakeNet 实例 -> 创建 feed 缓冲 -> 挂板级 mic AFE sink ->
 * 创建 wake_detect_task。初始化完成后检测器自动工作，无需再调用。
 *
 * @note 必须在 board_audio_init() 与 voice_service_init() 之后调用；
 *       依赖 "model" 分区构建时已烧录 esp-sr 的 srmodels.bin。
 * @return ESP_OK 检测器就绪；ESP_ERR_INVALID_STATE CONFIG_USE_WAKENET 未启用；
 *         ESP_ERR_NOT_FOUND 模型分区/唤醒模型不可用；
 *         ESP_ERR_NO_MEM AFE/缓冲/任务创建失败。
 */
esp_err_t wake_detector_init(void);

/** 返回检测器是否就绪（初始化成功且任务已创建）。 */
bool wake_detector_is_ready(void);

#ifdef __cplusplus
}
#endif
