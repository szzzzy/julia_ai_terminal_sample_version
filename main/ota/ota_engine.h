/**
 * @file    ota_engine.h
 * @brief   固件 OTA 下载引擎的公共接口。
 *
 * 本模块负责服务器响应调度、唯一下载任务、断点续传、Flash 写入、镜像校验、
 * 启动分区切换和重启。应用启动验收由 ota_boot_flow 负责。
 *
 * 生命周期（引擎侧驱动的阶段）：accepted → downloading → verifying → rebooting；
 * 成功后设备重启并以 booted_pending_verify 进入启动验收，最终转为 succeeded /
 * rolled_back（由 ota_boot_flow + ota_boot_health 完成本地确认或回滚）；failed 与
 * deferred 是本引擎失败路径上报的收纳状态。本头文件只暴露引擎侧入口；启动验收与
 * 回滚接口见 ota_boot_flow.h。
 *
 * 模块边界：本模块只负责“取回镜像并切换启动分区”。控制面 JSON 解析和服务器版本
 * 比较在 ota_control_plane；断点/隔离持久化在 ota_state_store；生命周期与进度上报在
 * ota_report；SHA-256、镜像头预检、提交前检查等参考实现在 ota_stability。本文件只
 * 通过上述模块的公共接口协作，不访问其内部状态。
 *
 * @note 运行状态只保存在 OTA 引擎内部，由短临界区保护；本函数不允许在中断
 *       上下文中调用。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 查询是否已有固件 OTA 下载任务正在运行。
 *
 * @return true 已有 OTA 任务运行；false 空闲。
 */
bool ota_engine_is_running(void);

/**
 * @brief 校验服务器 OTA 响应，并在需要升级时创建后台下载任务。
 *
 * @param[in] json     JSON 数据首地址，不要求以 NUL 结尾。
 * @param[in] json_len JSON 有效字节数。
 * @return ESP_OK 响应有效且无需升级或任务创建成功。
 * @return ESP_ERR_INVALID_ARG 响应格式或清单无效。
 * @return ESP_ERR_INVALID_STATE 已有 OTA 任务或请求状态不匹配。
 * @return ESP_ERR_NO_MEM 无法创建任务或复制参数。
 */
esp_err_t ota_engine_handle_server_json(const char *json, size_t json_len);

/**
 * @brief 兼容直接触发场景的 OTA JSON 入口。
 */
esp_err_t ota_engine_trigger_json(const char *json, size_t json_len);

#ifdef __cplusplus
}
#endif
