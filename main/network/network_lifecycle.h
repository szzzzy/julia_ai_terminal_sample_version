/**
 * @file    network_lifecycle.h
 * @brief   负责连接 Wi-Fi，并在取得网络地址后启动所有需要联网的服务。
 *
 * 找不到热点、密码错误或服务器暂时不可用时，设备本地界面仍能完成启动。
 * 本模块会持续尝试恢复 Wi-Fi；获得 IPv4 地址后，再按注册顺序启动 MQTT、语音连接
 * 和时间同步。某项服务启动失败只延后该服务，不会让其它服务永久失去重试机会。
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 联网服务的启动函数；设备获得 IPv4 地址后调用，失败时稍后重试。
 *
 * 该函数运行在普通后台任务中，可以建立连接或同步时间，但不能无限等待，
 * 否则会推迟其它联网服务和下一次 Wi-Fi 恢复尝试。
 */
typedef esp_err_t (*network_ip_ready_cb_t)(void *arg);

/** 可注册的 IP 就绪回调数量上限。 */
#define NETWORK_MAX_IP_READY_CALLBACKS 4

/**
 * @brief 登记一项“联网后才能启动”的服务。
 *
 * 每次重新获得 IPv4 地址，所有已登记服务都会按顺序重新确认启动。成功后在本次
 * 网络连接期间不再重复调用；失败时逐步延长等待时间并加入随机偏移，避免多台设备
 * 同时重试压垮服务器。Wi-Fi 再次断开后，本轮启动结果自动失效。
 *
 * @param[in] callback 联网后要执行的启动函数，不允许为 NULL。
 * @param[in] arg      传给该启动函数的业务数据，可为 NULL。
 *
 * @return ESP_OK 注册成功。
 * @return ESP_ERR_INVALID_ARG callback 为 NULL。
 * @return ESP_ERR_NO_MEM 回调表已满。
 * @return ESP_ERR_INVALID_STATE 网络生命周期已经启动（必须在启动前注册）。
 *
 * @note 不允许在中断上下文中调用。
 */
esp_err_t network_lifecycle_register_ip_ready(network_ip_ready_cb_t callback, void *arg);

/**
 * @brief 启动 Wi-Fi 客户端和后台恢复任务。
 *
 * 初始化网络接口并开始连接热点。热点暂时不可用不会阻塞应用启动；后台任务持续
 * 恢复连接，取得 IPv4 地址后启动已登记的联网服务。
 *
 * @return ESP_OK 生命周期已启动（重复调用幂等返回）。
 * @return ESP_ERR_NOT_SUPPORTED 构建未启用 Wi-Fi 连接示例。
 * @return 其他 esp_err_t Wi-Fi 初始化失败（部分资源已清理）。
 *
 * @note 不允许在中断上下文中调用。
 */
esp_err_t network_lifecycle_start(void);

/** 本地依赖刚刚就绪时，立即重试尚未成功启动的联网服务。 */
void network_lifecycle_retry_services(void);

#ifdef __cplusplus
}
#endif
