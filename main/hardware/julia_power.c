/**
 * @file julia_power.c
 * @brief 建立板级电源保持，并配置当前固件采用的 CPU 频率范围。
 *
 * BAT_Control 必须在其它外设初始化前保持高电平；短暂低脉冲可能切断电池供电。
 * 本模块只配置运行期电源策略，不负责关机时序、电量检测或外设级休眠。
 */
#include "julia_power.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"

#define JULIA_BAT_CONTROL_GPIO GPIO_NUM_8

static const char *TAG = "JULIA_POWER";

esp_err_t julia_power_hold_enable(void)
{
    /* Preload the output latch before enabling the pad so the battery switch
     * does not see an avoidable low pulse while app_main takes ownership. */
    esp_err_t err = gpio_set_level(JULIA_BAT_CONTROL_GPIO, 1);
    if (err != ESP_OK) return err;

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << JULIA_BAT_CONTROL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&config);
    if (err != ESP_OK) return err;
    err = gpio_set_level(JULIA_BAT_CONTROL_GPIO, 1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "battery power hold enabled gpio=%d",
                 (int)JULIA_BAT_CONTROL_GPIO);
    }
    return err;
}

esp_err_t julia_power_management_init(void)
{
    /* 240 MHz 来自工程构建基线；80 MHz 是当前运行策略的下限。自动 Light-sleep
     * 尚未与显示、音频和网络 owner 协调，因此保持关闭，不能把 DFS 等同于整机休眠。 */
    const esp_pm_config_t config = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "dynamic frequency scaling enabled max=240MHz min=80MHz");
    }
    return err;
}
