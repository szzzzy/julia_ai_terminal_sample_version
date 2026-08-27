/**
 * @file    ota_boot_flow.c
 * @brief   OTA 镜像启动验收、确认、回滚与持久化状态对账。
 *
 * 本模块在其他业务服务启动前执行。它处理 PENDING_VERIFY 镜像的本地健康检查，
 * 必要时回滚或进入安全模式，并完成 OTA 报告与断点记录的启动对账。
 */
#include "ota_boot_flow.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"

#include "ota_boot_health.h"
#include "ota_report.h"
#include "ota_state_store.h"
#include "ota_types.h"

static const char *TAG = "ota_boot_flow";

/**
 * @brief 对升级后首次启动的固件执行最小 GPIO 诊断。
 *
 * @return true 诊断 GPIO 等待 5 s 后为高电平，当前实现视为诊断通过。
 * @return false 诊断 GPIO 为低电平，当前实现视为诊断失败。
 *
 * 函数将配置的 GPIO 设置为输入并开启内部上拉，等待外部电路稳定后读取电平，最后恢复
 * GPIO 默认状态。它用于 OTA 的 PENDING_VERIFY 流程，执行期间阻塞约 5 s；外部硬件必须
 * 遵循“高电平表示通过”的约定。
 *
 * @note 只能在普通 FreeRTOS 任务上下文调用，不能在中断上下文调用。
 */
static bool diagnostic(void)
{
    /* 诊断输入使用上拉，避免外部信号悬空时读到不确定电平。 */
    gpio_config_t io_conf;
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << CONFIG_EXAMPLE_GPIO_DIAGNOSTIC);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Diagnostics (5 sec)...");
    /* 给外部诊断电路留出稳定时间，避免刚重启时的瞬态导致误回滚。 */
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    /* 当前实现把高电平作为诊断成功条件。 */
    bool diagnostic_is_ok = gpio_get_level(CONFIG_EXAMPLE_GPIO_DIAGNOSTIC);

    /* 诊断结束后释放 GPIO 配置，避免长期占用该引脚的输入/上拉设置。 */
    gpio_reset_pin(CONFIG_EXAMPLE_GPIO_DIAGNOSTIC);
    return diagnostic_is_ok;
}

/**
 * @brief 执行不依赖网络连接的本地基础设施和资源健康检查。
 *
 * @param[in] include_gpio_diagnostic true 时额外执行配置的 GPIO 诊断；通常仅在
 *                                    PENDING_VERIFY 启动阶段传入 true。
 * @return true 当前运行分区、物理 Flash、应用描述、堆空间和基础队列检查均通过。
 * @return false 任一必要检查失败，调用者不得继续确认 pending 镜像或启动通信。
 *
 * 检查顺序先确认运行/目标分区和物理 Flash 容量，再检查应用描述、产品配置、堆空间和
 * FreeRTOS 队列收发；最后按参数选择 GPIO 诊断。函数不连接 Wi-Fi 或 MQTT，
 * 从而不会把网络暂时不可用误判为镜像健康。
 *
 * @note 创建的测试队列会在函数返回前删除；GPIO 诊断可能阻塞约 5 s，不能在中断调用。
 */
static bool ota_local_health_check(bool include_gpio_diagnostic)
{
    return ota_boot_health_check(include_gpio_diagnostic, diagnostic);
}

/**
 * @brief 进入不启动通信客户端的 OTA 安全模式。
 *
 * @param[in] reason 进入安全模式的诊断原因，可为 NULL。
 *
 * @note 本函数不返回、不重启，也不擦除 NVS；通过每秒延时保持任务可调度，等待人工处理
 *       或外部复位。只能在普通任务上下文调用。
 */
static void __attribute__((noreturn)) ota_enter_safe_mode(const char *reason)
{
    ota_boot_health_enter_safe_mode(reason);
}

/**
 * @brief 读取最近一次被 bootloader 判定无效的镜像版本，用于回滚结果关联。
 *
 * @param[out] version 接收版本字符串的缓冲区；函数失败时写入空串。
 * @param[in] version_size 缓冲区容量，单位为字节，必须大于 0。
 * @return true 找到无效分区且成功读取非空版本。
 * @return false 参数无效、没有无效分区或分区描述读取失败。
 *
 * @note 只读 bootloader/分区描述信息，不修改 OTA 状态；调用方负责与持久化报告上下文
 *       比较版本后再决定是否上报 rolled_back。
 */
static bool ota_get_last_invalid_version(char *version, size_t version_size)
{
    if (version == NULL || version_size == 0) {
        return false;
    }
    version[0] = '\0';
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (invalid == NULL) {
        return false;
    }
    esp_app_desc_t invalid_desc;
    if (esp_ota_get_partition_description(invalid, &invalid_desc) != ESP_OK ||
        invalid_desc.version[0] == '\0') {
        return false;
    }
    strncpy(version, invalid_desc.version, version_size - 1U);
    version[version_size - 1U] = '\0';
    return true;
}

void ota_boot_flow_run(void)
{
    bool pending_verify = false;
    esp_err_t err = ota_boot_health_begin(&pending_verify);
    if (err != ESP_OK) {
        ota_enter_safe_mode("cannot read OTA boot state");
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (pending_verify) {
            ESP_LOGE(TAG, "NVS format is incompatible during PENDING_VERIFY; refusing to erase NVS");
            err = ota_boot_health_reject("NVS format incompatible");
            ota_enter_safe_mode(err == ESP_ERR_OTA_ROLLBACK_FAILED ?
                                "rollback unavailable after NVS failure" :
                                "rollback failed after NVS failure");
        }
        ESP_LOGW(TAG, "Erasing NVS because no image is pending verification");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ota_enter_safe_mode("NVS initialization failed");
    }
    ota_state_store_log_nvs_usage(TAG, "startup", ESP_OK);

    err = native_ota_report_init();
    if (err != ESP_OK) {
        /* 状态上报不可用不能阻断已有 OTA/回滚主流程。 */
        ESP_LOGW(TAG, "OTA report initialization failed: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (pending_verify) {
        err = native_ota_report_boot_pending_verify();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not report booted_pending_verify: %s", esp_err_to_name(err));
        }
    }

    if (!ota_local_health_check(pending_verify)) {
        if (pending_verify) {
            native_ota_failure_reason_t rollback_reason =
                esp_ota_check_rollback_is_possible() ?
                NATIVE_OTA_FAILURE_BOOT_SELF_TEST_FAILED :
                NATIVE_OTA_FAILURE_ROLLBACK_UNAVAILABLE;
            err = native_ota_report_boot_rolled_back(rollback_reason);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Could not report rolled_back: %s", esp_err_to_name(err));
            }
            err = ota_boot_health_reject("boot self-test failed");
            ota_enter_safe_mode(err == ESP_ERR_OTA_ROLLBACK_FAILED ?
                                "rollback unavailable after self-test" :
                                "rollback failed after self-test");
        }
        ota_enter_safe_mode("local health check failed");
    }

    /* A product override must finish critical local business initialization before a
     * PENDING_VERIFY image can become VALID. Remote connectivity is intentionally not
     * an acceptance condition. */
    if (pending_verify && !ota_boot_health_product_check()) {
        native_ota_failure_reason_t rollback_reason =
            esp_ota_check_rollback_is_possible() ?
            NATIVE_OTA_FAILURE_BOOT_SELF_TEST_FAILED :
            NATIVE_OTA_FAILURE_ROLLBACK_UNAVAILABLE;
        err = native_ota_report_boot_rolled_back(rollback_reason);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not report rolled_back after product health failure: %s",
                     esp_err_to_name(err));
        }
        err = ota_boot_health_reject("product boot health check failed");
        ota_enter_safe_mode(err == ESP_ERR_OTA_ROLLBACK_FAILED ?
                            "rollback unavailable after product health check" :
                            "rollback failed after product health check");
    }

    if (pending_verify) {
        err = ota_boot_health_confirm();
        if (err != ESP_OK) {
            native_ota_failure_reason_t rollback_reason =
                esp_ota_check_rollback_is_possible() ?
                NATIVE_OTA_FAILURE_BOOT_SELF_TEST_FAILED :
                NATIVE_OTA_FAILURE_ROLLBACK_UNAVAILABLE;
            (void)native_ota_report_boot_rolled_back(rollback_reason);
            err = ota_boot_health_reject("cannot confirm healthy image");
            ota_enter_safe_mode("cannot confirm or rollback image");
        }
        err = native_ota_report_boot_succeeded();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not report succeeded: %s", esp_err_to_name(err));
        }
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        esp_app_desc_t running_desc;
        if (esp_ota_get_partition_description(running, &running_desc) == ESP_OK) {
            ESP_LOGI(TAG, "Running firmware version: %s", running_desc.version);
            char last_invalid_version[sizeof(running_desc.version)] = { 0 };
            const char *invalid_version =
                ota_get_last_invalid_version(last_invalid_version, sizeof(last_invalid_version)) ?
                last_invalid_version : NULL;
            if (!pending_verify && invalid_version != NULL) {
                err = native_ota_report_reconcile_rollback(running_desc.version,
                                                           invalid_version);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Could not reconcile OTA rollback report: %s",
                             esp_err_to_name(err));
                }
            }
            err = ota_state_store_reconcile_running_version(running_desc.version);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to reconcile OTA resume record: %s", esp_err_to_name(err));
            }
        }
    }
}

