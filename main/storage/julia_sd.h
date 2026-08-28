/**
 * @file    julia_sd.h
 * @brief   SD 卡文件辅助接口：挂载/加锁/顺序读基准（供 UI 与文件读取）。
 *
 * 实现见 julia_sd.c。挂载点为 /sdcard；文件访问一律先 julia_sd_lock() 再
 * julia_sd_unlock()，避免多任务并发。本模块与 main/storage/sd_card.c 并存，
 * 都使用 /sdcard 挂载点，需注意两套实现的初始化顺序与挂载来源（见 .c 文件头 NOTE）。
 * julia_sd_init() 目前未见调用点（详见 .c），UI 侧 is_mounted 可能恒为 false。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/** SD 卡挂载点（与 voice_uri 的 "SD:/x" -> "/sdcard/x" 映射一致）。 */
#define JULIA_SD_MOUNT_POINT "/sdcard"

/** 挂载 SD（FAT 到 /sdcard）。@param[in] format_if_mount_failed 失败时是否格式化。 */
esp_err_t julia_sd_init(bool format_if_mount_failed);
/** SD 是否已挂载。 */
bool julia_sd_is_mounted(void);
/** 获取 SD 访问锁。@param[in] timeout 等待超时（ticks）。@return true 拿到锁。 */
bool julia_sd_lock(TickType_t timeout);
/** 释放 SD 访问锁。 */
void julia_sd_unlock(void);
/**
 * 顺序读带宽基准。
 * @param[out] average_mbps 平均带宽（可空）。
 * @param[out] minimum_mbps 最小带宽（可空）。
 * @return ESP_OK 成功；其他 esp_err_t 失败。
 */
esp_err_t julia_sd_benchmark_read(float *average_mbps, float *minimum_mbps);
