/**
 * @file    sd_card.h
 * @brief   SD 卡（FAT）挂载，供 voice_service 的 "SD:/<name>" 文件推送使用。
 *
 * 挂载点为 /sdcard，与 voice_uri.c 中 "SD:/x" -> "/sdcard/x" 的映射一致。
 *
 * 板级接法（Waveshare ESP32-S3-LCD-1.85）：
 * - SDMMC 1-bit：CLK=IO14、CMD=IO17、D0=IO16；
 * - D3/CS 经 TCA9554 P2（Extend_IO3）控制，初始化时先拉高，卡保持 SD 模式；
 * - TCA9554 在 I2C_NUM_0：SCL=IO10、SDA=IO11，7-bit 地址 0x20，400 kHz。
 *
 * 只有 SDMMC 数据引脚和频率实际读取 Kconfig。CONFIG_SD_CARD_I2C_SCL/SDA 当前未被
 * tca9554.c 使用，不能据此认为共享 I2C 已重映射。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂载 SD 卡；只允许启动 owner 串行调用一次。
 *
 * 先初始化 TCA9554 并把 CS 拉高，再以 SDMMC 1-bit 模式挂载 FAT 到 /sdcard。
 * 缺卡或挂载失败不阻断应用：返回错误码，调用方仅记录日志；之后 voice_service
 * 的 FILE_SEND 会以 ERROR file_open_failed 呈现。
 *
 * @return ESP_OK 已挂载。
 * @return ESP_ERR_NOT_SUPPORTED SD 卡功能未启用（CONFIG_SD_CARD_ENABLE=n）。
 * @note  失败不格式化介质，也没有自动重试／反初始化；不要从多个任务并发调用。
 */
esp_err_t sd_card_start(void);

/**
 * @brief 返回启动期挂载结果的快照；运行中拔卡后可能仍为 true。
 */
bool sd_card_is_mounted(void);

#ifdef __cplusplus
}
#endif
