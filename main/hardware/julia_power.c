/**
 * @file julia_power.c
 * @brief 建立板级电源保持，并配置当前固件采用的 CPU 频率范围。
 *
 * BAT_Control 必须在其它外设初始化前保持高电平；短暂低脉冲可能切断电池供电。
 * 本模块配置 CPU 上限并检测 PWR 长按松手以切断电池保持；不负责电量检测或外设级休眠。
 */
#include "julia_power.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_power_key.h"

static const char *TAG = "JULIA_POWER";
static esp_pm_lock_handle_t s_cpu_boost;
static bool s_hold_ready;

#if CONFIG_JULIA_PWR_KEY_ENABLE
static TaskHandle_t s_key_task;
static void power_key_task(void *arg)
{
    (void)arg;
    julia_power_key_t key = {0};
    for (;;) {
        bool pressed = gpio_get_level(CONFIG_JULIA_PWR_KEY_GPIO) == 0;
        if (julia_power_key_update(&key, pressed, esp_timer_get_time() / 1000,
                                   CONFIG_JULIA_PWR_OFF_HOLD_MS)) {
            ESP_LOGW(TAG, "PWR long press released: disabling battery power hold");
            esp_err_t err = gpio_set_level(CONFIG_JULIA_BAT_CONTROL_GPIO, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "battery power off failed: %s", esp_err_to_name(err));
            } else {
                /* USB 直供无法由 BAT_Control 切断；不假装已关机，也不自动复位。 */
                vTaskDelay(pdMS_TO_TICKS(250));
                ESP_LOGW(TAG, "still powered (USB/external supply); battery hold remains disabled");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1);
    }
}
#endif

esp_err_t julia_power_key_start(void)
{
#if CONFIG_JULIA_PWR_KEY_ENABLE
    /* 仅由 app_main 调用；保持配置失败时不能重新配置同一电源路径。 */
    if (!s_hold_ready) return ESP_ERR_INVALID_STATE;
    if (s_key_task) return ESP_OK;
    if (CONFIG_JULIA_PWR_KEY_GPIO == CONFIG_JULIA_BAT_CONTROL_GPIO ||
        !GPIO_IS_VALID_GPIO(CONFIG_JULIA_PWR_KEY_GPIO)) return ESP_ERR_INVALID_ARG;
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_JULIA_PWR_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    return xTaskCreate(power_key_task, "pwr_key", 3072, NULL, 3, &s_key_task) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
#else
    return ESP_OK;
#endif
}

/* ESP-PM 计数锁（ESP_PM_CPU_FREQ_MAX）：持有期间只要求 CPU 运行在"已配置的最高频率"
 * 上，即当前 boot／runtime 档位给出的 DFS 上限，并不会超过该上限；最后一个持有者释放后
 * 才允许 DFS 回落到 min（80 MHz）。每次 begin() 成功都必须对应一次 end()：调用方用局部
 * 标志记住 begin() 是否成功（未初始化 PM 时返回 false），未配对或凭空释放都会让持锁计数
 * 与实际持锁者失配。 */
bool julia_power_boost_begin(void)
{
    return s_cpu_boost && esp_pm_lock_acquire(s_cpu_boost) == ESP_OK;
}

void julia_power_boost_end(void)
{
    if (s_cpu_boost) (void)esp_pm_lock_release(s_cpu_boost);
}

/* 只调整 DFS 上下限：min 固定 80 MHz，max 由 Kconfig 的启动／运行档位给出；
 * light_sleep 保持关闭，自动休眠尚未与各外设 owner 协调。 */
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
    /* 先把输出锁存预置为高，再把 pad 切成输出：电池开关不会在 app_main 接管的瞬间
     * 看到可避免的低脉冲。 */
    /* 保持脚由 CONFIG_JULIA_BAT_CONTROL_GPIO 选择（当前板卡 GPIO7）；同板 GPIO6 是
     * Key_BAT、GPIO8 是 BAT_ADC，都不能改用作电池供电保持。 */
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
        s_hold_ready = true;
        ESP_LOGI(TAG, "battery power hold enabled gpio=%d",
                 (int)hold_gpio);
    }
    return err;
}

/* 由 app_main 调用：创建供 OTA／TLS 等阻塞操作把 CPU 拉到"当前档位上限"的计数锁
 * （锁名只是调试标签），再把 CPU 上限设为启动档位。可重复调用（已创建则复用）；
 * 锁创建失败会直接返回，这一次连启动档位也不会应用，且 boost 接口此后恒为 false。 */
esp_err_t julia_power_management_init(void)
{
    if (!s_cpu_boost) {
        esp_err_t err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "network_ota", &s_cpu_boost);
        if (err != ESP_OK) return err;
    }
    /* 启动阶段限制 CPU 峰值，避免与屏幕、音频和 Wi-Fi 上电浪涌叠加。自动
     * Light-sleep 尚未与各 owner 协调，因此仍保持关闭。 */
    return configure_cpu_limit(CONFIG_JULIA_BOOT_CPU_MAX_FREQ_MHZ, "boot");
}

/* 切换到运行档位：调用者是 app_main，时机在本地显示／音频／FSM 启动完成之后、
 * 启动后台网络生命周期之前。这里设置的是 DFS 上限（CONFIG_JULIA_RUNTIME_CPU_MAX_FREQ_MHZ，
 * min 仍为 80 MHz）；持有 boost 锁的操作只会把 CPU 锁在该上限上运行，不会突破它。 */
esp_err_t julia_power_runtime_profile_enable(void)
{
    return configure_cpu_limit(CONFIG_JULIA_RUNTIME_CPU_MAX_FREQ_MHZ, "runtime");
}
