/**
 * @file    ota_engine.h
 * @brief   在后台安全下载新固件，完整校验后才切换下次启动版本。
 *
 * 服务器清单通过身份、版本、大小和摘要检查后，本模块才创建下载任务。同一时间
 * 只允许一个升级。下载中断可保留进度；镜像头、大小和 SHA-256 全部正确后才设置
 * 下次启动分区。新固件重启后的本地健康检查由启动验收模块负责。
 *
 * 对服务器可见的过程是：已接受、正在下载、正在校验、准备重启。重启后先进入
 * “新固件待确认”，健康检查通过才报告成功；失败时回到上一版本。电源、内存或
 * 业务条件暂时不满足时保留已校验镜像，稍后再提交，而不是重新下载。
 *
 * 本模块只负责取得并安装镜像。服务器报文解析、恢复记录、状态上报和启动健康检查
 * 分别由对应模块完成，避免下载任务同时承担通信、持久化和产品验收。
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
 * @brief 查询设备是否正在下载、校验或提交一项固件升级。
 *
 * @return true 已有 OTA 任务运行；false 空闲。
 */
bool ota_engine_is_running(void);

/**
 * @brief 处理服务器升级响应；确实需要且允许升级时启动后台任务。
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
