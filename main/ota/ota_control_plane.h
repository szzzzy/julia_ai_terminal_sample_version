/**
 * @file    ota_control_plane.h
 * @brief   生成版本检查请求，并拒绝不属于本设备、本次请求或不安全的升级清单。
 *
 * 每次检查使用新的请求编号。服务器响应必须匹配该编号、本机身份、产品和硬件版本；
 * 下载地址、版本、大小、摘要、安全版本和有效期也必须全部合法。通过后只输出清单，
 * 不创建下载任务、不写固件分区。
 *
 * 只接受最近一次检查的响应，迟到旧响应和已隔离制品都会被拒绝。本模块只回答
 * “是否值得下载”，服务器可见的升级进度由状态报告模块维护。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "ota_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 获取设备生命周期内稳定的身份标识，用于主题隔离和服务器响应校验。 */
esp_err_t native_ota_get_device_id(char *device_id, size_t device_id_size);

/** 生成包含设备身份、硬件版本和当前固件版本的检查请求。 */
esp_err_t native_ota_build_check_request(char *json, size_t json_size, size_t *json_len);

/**
 * @brief 校验服务器响应是否属于本次检查，并判断是否需要下载。
 *
 * 当响应声明 update=false 或目标版本已经运行时，函数返回 ESP_OK 且将
 * download_requested 置为 false；当需要下载时，manifest 返回完整深拷贝。
 *
 * @param[in]  json               JSON 数据首地址，不要求以 NUL 结尾。
 * @param[in]  json_len           JSON 有效长度，单位为字节。
 * @param[out] manifest           接收已校验清单的结构体，不允许为 NULL。
 * @param[out] download_requested 接收是否应创建下载任务的标志，不允许为 NULL。
 * @return ESP_OK 响应有效。
 * @return ESP_ERR_INVALID_ARG JSON、设备身份、清单字段、有效期或 URL 无效。
 * @return ESP_ERR_INVALID_STATE request_id 不匹配或 artifact 已被隔离。
 * @return ESP_ERR_NO_MEM cJSON 临时对象创建失败。
 *
 * @note 函数可在通信事件任务中调用，但会分配 cJSON 临时对象，不能在中断中调用。
 */
esp_err_t ota_control_plane_parse_server_response(const char *json, size_t json_len,
                                                  native_ota_manifest_t *manifest,
                                                  bool *download_requested);

#ifdef __cplusplus
}
#endif
