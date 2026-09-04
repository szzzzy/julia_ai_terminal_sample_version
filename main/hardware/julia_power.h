#pragma once

#include "esp_err.h"

/**
 * @brief 接管 BAT_Control（GPIO7）并保持电池供电路径导通。
 *
 * 必须在启动早期调用；实现先预装高电平再切换为输出，避免接管 GPIO 时产生低脉冲。
 * 返回成功只表示保持脚配置完成，不表示其它电源轨或外设已经就绪。
 */
esp_err_t julia_power_hold_enable(void);

/**
 * @brief 将 DFS 范围设为 80～240 MHz，并保持自动 Light-sleep 关闭。
 *
 * 该范围是当前产品配置，不是运行时测量结果；调用方应记录配置失败，但不能据此
 * 假定电池保持失效。
 */
esp_err_t julia_power_management_init(void);
