/**
 * @file    julia_sd.h
 * @brief   未接入当前构建的 SD 参考接口。
 *
 * 现用挂载器是 sd_card.c；两套接口不共享 mount 状态或 mutex，不得并行初始化。
 * 本接口保留格式化和覆盖基准文件的能力，只适用于专用测试卡。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/** SD 卡挂载点（与 voice_uri 的 "SD:/x" -> "/sdcard/x" 映射一致）。 */
#define JULIA_SD_MOUNT_POINT "/sdcard"

/** 挂载到 /sdcard；format_if_mount_failed=true 可能清空用户卡。 */
esp_err_t julia_sd_init(bool format_if_mount_failed);
/** 只返回本参考模块的状态，不反映 sd_card.c 的挂载结果。 */
bool julia_sd_is_mounted(void);
/** 获取本模块 mutex；timeout 单位为 FreeRTOS tick，成功后必须由同一任务释放。 */
bool julia_sd_lock(TickType_t timeout);
/** 与一次成功的 julia_sd_lock() 配对；不允许无所有权释放。 */
void julia_sd_unlock(void);
/**
 * 顺序读带宽基准；会覆盖并删除 /sdcard/julia/bench.bin。
 * @param[out] average_mbps 平均带宽（可空）。
 * @param[out] minimum_mbps 最小带宽（可空）。
 * @return ESP_OK 成功；其他 esp_err_t 失败。
 */
esp_err_t julia_sd_benchmark_read(float *average_mbps, float *minimum_mbps);
