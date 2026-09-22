#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief 接管配置的 BAT_Control（当前板卡使用 GPIO7）并保持电池供电路径导通。
 *
 * 必须在启动早期调用；实现先预装高电平再切换为输出，避免接管 GPIO 时产生低脉冲。
 * 返回成功只表示保持脚配置完成，不表示其它电源轨或外设已经就绪。
 * 该引脚由 CONFIG_JULIA_BAT_CONTROL_GPIO 选择；GPIO6（Key_BAT）和 GPIO8（BAT_ADC）
 * 是同板其它功能，不得改接到这里。
 */
esp_err_t julia_power_hold_enable(void);

/** app_main 在供电保持成功后调用一次。PWR 低有效，消抖 50ms；首次松手后
 * 才接受新长按，达到配置门限并松手后切断电池保持。USB 供电不会被切断。
 * 这是电源键断电，不执行业务保存或 OTA 等待；禁用配置时为空操作。 */
esp_err_t julia_power_key_start(void);

/**
 * @brief 应用启动阶段的低峰值 CPU 配置，并保持自动 Light-sleep 关闭。
 *
 * 该范围是当前产品配置，不是运行时测量结果；调用方应记录配置失败，但不能据此
 * 假定电池保持失效。可由 app_main 重复调用，已创建的 boost 计数锁会被复用。
 */
esp_err_t julia_power_management_init(void);

/**
 * @brief 本地外设和 Wi-Fi 已错峰启动后，切换到运行期 CPU 频率上限。
 *
 * 由 app_main 在启动后台网络任务之前调用；这里只把 DFS 上限设为运行档
 * （CONFIG_JULIA_RUNTIME_CPU_MAX_FREQ_MHZ，min 保持 80 MHz）。OTA／TLS 等操作通过
 * julia_power_boost_begin() 取得的锁只是要求 CPU 运行在已配置的最高频率（即该上限），
 * 不会超过它；释放后允许频率回落到 min。
 */
esp_err_t julia_power_runtime_profile_enable(void);
/**
 * 成功取得后必须对应释放。ESP-PM 计数锁使多个并行操作全部完成后才允许降频。
 *
 * 该锁不改变 DFS 上限：持有期间 CPU 被锁定在 julia_power_runtime_profile_enable()
 * 配置的档位上限上运行，不会突破上限；未调用 boost 时操作可能只在 min 频率下执行。
 *
 * 计数锁允许重复取得（每次都要释放一次），释放顺序不限，但不能凭空释放：只有
 * julia_power_boost_begin() 返回 true 才应调用 julia_power_boost_end()，否则持锁
 * 计数与实际持有者失配。PM 未初始化时 begin 返回 false、end 为空操作。
 *
 * 调用方是执行阻塞网络操作的任务：网络生命周期（Wi-Fi 连接与服务启动）、MQTT、
 * WSS 建连、语音状态上报和 OTA 下载，它们在操作前后成对调用，并用局部标志记录
 * begin() 是否成功。
 */
bool julia_power_boost_begin(void);
void julia_power_boost_end(void);
