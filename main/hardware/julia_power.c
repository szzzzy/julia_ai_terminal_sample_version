/**
 * @file julia_power.c
 * @brief 建立板级电源保持，并配置当前固件采用的 CPU 频率范围。
 *
 * BAT_Control 必须在其它外设初始化前保持高电平；短暂低脉冲可能切断电池供电。
 * 本模块配置启动与运行期 CPU 上限，不负责关机时序、电量检测或外设级休眠。
 */
#include "julia_power.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "sdkconfig.h"

static const char *TAG = "JULIA_POWER";

static esp_err_t configure_cpu_limit(int max_freq_mhz, const char *profile)
{
    const esp_pm_config_t config = {
        .max_freq_mhz = max_freq_mhz,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s CPU profile max=%dMHz min=80MHz", profile, max_freq_mhz);
    }
    return err;
}

esp_err_t julia_power_hold_enable(void)
{
    /* Preload the output latch before enabling the pad so the battery switch
     * does not see an avoidable low pulse while app_main takes ownership. */
    const gpio_num_t hold_gpio = (gpio_num_t)CONFIG_JULIA_BAT_CONTROL_GPIO;
    esp_err_t err = gpio_set_level(hold_gpio, 1);
    if (err != ESP_OK) return err;

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << hold_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&config);
    if (err != ESP_OK) return err;
    err = gpio_set_level(hold_gpio, 1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "battery power hold enabled gpio=%d",
                 (int)hold_gpio);
    }
    return err;
}

esp_err_t julia_power_management_init(void)
{
    /* 启动阶段限制 CPU 峰值，避免与屏幕、音频和 Wi-Fi 上电浪涌叠加。自动
     * Light-sleep 尚未与各 owner 协调，因此仍保持关闭。 */
    return configure_cpu_limit(CONFIG_JULIA_BOOT_CPU_MAX_FREQ_MHZ, "boot");
}

esp_err_t julia_power_runtime_profile_enable(void)
{
    return configure_cpu_limit(CONFIG_JULIA_RUNTIME_CPU_MAX_FREQ_MHZ, "runtime");
}
