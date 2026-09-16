/**
 * @file    ota_boot_health.h
 * @brief   OTA 首次启动验收、健康检查、确认与回滚接口。
 *
 * 此模块补充 ESP-IDF 官方 native OTA 例程的 GPIO 诊断：在确认 PENDING_VERIFY
 * 镜像前检查分区、物理 Flash、应用描述、堆和基础 FreeRTOS 队列。网络可用性
 * 不属于镜像健康条件，因此不在本模块中检查。
 *
 * 不变量：本模块只读取本地资源与 OTA 状态，任何检查都不得以联网为前提；后续增加
 * 验收项时必须保持这一点，否则弱网会把有效镜像判为不健康并触发回滚。
 *
 * 调用时序（由 ota_boot_flow.c 的 ota_boot_flow_run 在其它业务服务启动前驱动）：
 *   1. ota_boot_health_begin()  读取运行分区是否为 ESP_OTA_IMG_PENDING_VERIFY；
 *   2. ota_boot_health_check()   执行不依赖网络的本地健康检查；
 *   3. 通过 → ota_boot_health_confirm()；失败 → ota_boot_health_reject()（并伴随重启）。
 *
 * 接口语义（本模块只读写 ESP-IDF OTA 状态，不访问 NVS、不发起网络）：
 * - confirm 调用 esp_ota_mark_app_valid_cancel_rollback()，把 PENDING_VERIFY 镜像
 *   标记为 VALID、取消回滚、使镜像永久生效，**不重启**；
 * - reject 先检查 esp_ota_check_rollback_is_possible()，可行则调用
 *   esp_ota_mark_app_invalid_rollback_and_reboot() —— 该 API 会标记分区无效并**立即重启**
 *   回退到上一个应用槽；
 * - begin 在无 OTA 状态（初次刷机）时返回“非 pending”，是正常路径而非错误。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GPIO 诊断回调。
 *
 * @return true 板级诊断通过；false 表示应拒绝当前 PENDING_VERIFY 镜像。
 *
 * @note 回调由普通任务同步调用，可以访问板级 GPIO，但不能依赖网络可用性。
 */
typedef bool (*ota_boot_health_gpio_diagnostic_t)(void);

/** 读取当前运行镜像是否处于 PENDING_VERIFY。
 *  返回 ESP_OK 时 *pending_verify 指示是否处于待验收；无 OTA 状态视为“否”。 */
esp_err_t ota_boot_health_begin(bool *pending_verify);

/** 将通过健康检查的运行镜像标记为 VALID（取消回滚，不重启）。
 *  @return ESP_OK 已确认；其他值由 esp_ota_mark_app_valid_cancel_rollback 返回。 */
esp_err_t ota_boot_health_confirm(void);

/** 检查回滚可行性并拒绝当前待验收镜像。
 *  @return ESP_OK 已发起回滚并重启；ESP_ERR_OTA_ROLLBACK_FAILED 表示无可用旧镜像。 */
esp_err_t ota_boot_health_reject(const char *reason);

/**
 * @brief PENDING_VERIFY 镜像的产品验收钩子。
 *
 * 产品代码可提供非弱定义版本，用于初始化并校验自己的关键本地服务：只有这些服务
 * 确实就绪后才返回 true；返回 false 会阻止镜像被标记为 VALID 并请求回滚。
 * 该钩子不得依赖 Wi-Fi、DNS、MQTT 或其他远程可用性——网络暂时不可达不代表新镜像
 * 不健康，把联网当作验收条件会让正常固件在弱网环境下被回滚。
 *
 * 示例工程提供“直接成功”的弱默认实现；测试构建可用
 * CONFIG_OTA_TEST_FORCE_BOOT_HEALTH_FAIL 强制该钩子失败。
 */
bool ota_boot_health_product_check(void);

/**
 * @brief 运行不依赖网络的 OTA 本地健康检查。
 *
 * @param[in] include_gpio_diagnostic 是否执行 GPIO 诊断。
 * @param[in] gpio_diagnostic         GPIO 诊断回调；不执行诊断时可为 NULL。
 * @return true 全部本地检查通过。
 * @return false 必要资源、配置或诊断不符合要求。
 *
 * @note 若启用 GPIO 诊断，函数可阻塞；只能在普通任务上下文调用。
 * @note 检查顺序为运行/目标分区、物理 Flash、应用描述、产品配置、堆、FreeRTOS
 *       队列，最后执行可选 GPIO 诊断；不会检查 Wi-Fi、DNS 或 MQTT。
 * @note 堆门槛 CONFIG_OTA_MIN_FREE_HEAP 与 ota_stability.c 的提交前检查共用同一个
 *       Kconfig 值，改动会同时影响提交和启动验收。
 */
bool ota_boot_health_check(bool include_gpio_diagnostic,
                           ota_boot_health_gpio_diagnostic_t gpio_diagnostic);

/**
 * @brief 进入不启动通信客户端的永久安全模式。
 *
 * @param[in] reason 用于日志的原因字符串，可为 NULL。
 *
 * @note 函数不返回、不重启、不擦除 NVS；仅保持当前任务可调度并等待人工处理或复位。
 * @note 进入安全模式后不得继续调用 mqtt_comm_start() 或其他网络启动接口。
 */
void ota_boot_health_enter_safe_mode(const char *reason) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
