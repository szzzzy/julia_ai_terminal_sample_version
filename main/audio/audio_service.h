/**
 * @file    audio_service.h
 * @brief   音频服务层：MQTT 音频控制面与音频下载引擎之间的业务协调入口。
 *
 * 本模块把已通过音频控制面校验的 audio_check_response 转换为音频下载动作：
 * - 解析并校验音频清单（转调 audio_control_plane）；
 * - 创建下载前做 OTA 优先的单向门禁：OTA 或已有音频下载运行中则拒绝本次启动；
 * - 将下载请求交给 audio_engine。
 *
 * 本模块不持有音频运行状态：运行中判断统一转调 audio_engine。
 *
 * 互斥语义（与 OTA 共用网络/Flash 资源）：OTA 下载进行中时拒绝启动音频下载，这是本层
 * 的"单向门禁"（OTA 优先）；音频下载自身也拒绝并发。真正的单飞保证由 audio_engine 内部
 * 的运行标志兜底，引擎不查询 OTA 状态，因此不存在双向原子互斥；且反向不成立
 * （OTA 准入不查询音频状态）。
 *
 * 现状（当前边界，不是待办实现）：MQTT 层没有注册 audio_check_response 主题，
 * audio_service_handle_response() 在当前构建内没有外部调用方，只有 audio_service_init()
 * 被 main.c 调用；本层的 audio_service_handle_response() 仍是 audio_engine_start() 的
 * 唯一调用点。详见 audio_service.c 文件头。
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化音频服务层。
 *
 * @return ESP_OK 初始化成功。
 *
 * @note 幂等；必须在 NVS 初始化完成后、网络启动前调用。
 * @note 当前服务层没有自有状态，本函数为装配点保留（恒返回 ESP_OK）；不允许在中断
 *       上下文调用。它是本模块在当前构建中唯一有调用点的接口。
 */
esp_err_t audio_service_init(void);

/**
 * @brief 处理服务器返回的完整 audio_check_response。
 *
 * 完整 JSON 响应由 MQTT 模块完成分片重组后调用本函数。`update=false` 时只
 * 记录并返回 ESP_OK；需要下载时经互斥检查后交给音频引擎创建唯一下载任务。
 *
 * @param[in] json     JSON 数据首地址，不允许为 NULL，不要求以 NUL 结尾。
 * @param[in] json_len JSON 有效长度，范围为 1～NATIVE_OTA_JSON_MAX_LEN；超长报文整体
 *                     判为无效，不截断后解析（截断会得到与响应不符的文档）。
 *
 * @return ESP_OK 响应有效；可能无需下载，也可能已创建音频下载任务。两种结果共用该
 *         返回码，调用方无法区分，也没有应用层回执可供服务器区分。
 * @return ESP_ERR_INVALID_ARG JSON 格式、字段、身份、清单或有效期无效。
 * @return ESP_ERR_INVALID_STATE request_id 过期，或已有 OTA/音频下载任务运行
 *         （无回执协议下服务器无法把该错误与"未执行"区分开）。
 * @return ESP_ERR_NO_MEM 参数分配或任务创建失败。
 *
 * @note 可由 MQTT 事件任务调用；函数只解析元数据并调度任务，不在调用者
 *       上下文中执行下载。不允许在中断上下文调用。
 * @note 现状：本函数在当前构建内没有外部调用方（MQTT 分发入口尚未注册）；文件内的
 *       audio_engine_start() 调用是引擎唯一的调用点。
 */
esp_err_t audio_service_handle_response(const char *json, size_t json_len);

#ifdef __cplusplus
}
#endif
