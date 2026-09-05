/**
 * @file    ota_boot_flow.c
 * @brief   OTA 镜像启动验收、确认、回滚与持久化状态对账。
 *
 * 本模块在其他业务服务启动前执行。它处理 PENDING_VERIFY 镜像的本地健康检查，
 * 必要时回滚或进入安全模式，并完成 OTA 报告与断点记录的启动对账。
 *
 * 生命周期与编排（app_main 最先调用本模块）：
 *   ota_boot_health_begin() 判定启动镜像是否处于 PENDING_VERIFY →
 *   初始化 NVS / 网络接口 / 事件循环 → 上报 booted_pending_verify →
 *   本地健康检查（ota_boot_health_check + 可选 GPIO 诊断）→ 产品验收钩子
 *   （ota_boot_health_product_check）→ 通过则 confirm（esp_ota_mark_app_valid_
 *   cancel_rollback）并上报 succeeded；失败则 reject（标记无效并回滚/进入安全
 *   模式）并上报 rolled_back。最后与运行版本对账，清理已确认版本的断点记录。
 *
 * 模块边界：本模块只做“启动验收”的编排与对账，本身不解析 JSON、不上报 MQTT
 * 原始消息、不写断点记录；具体健康判定在 ota_boot_health，生命周期事件在
 * ota_report，断点/隔离在 ota_state_store，运行版本来源为 esp_ota_ops 分区描述。
 * 网络可用性刻意不作为镜像健康状况的判定条件（见 ota_boot_health）。
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
#include "julia_fault.h"
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
static bool s_fault_nvs_ready;
static bool s_pending_verify;
static void ota_reconcile_boot_state(bool pending_verify);

static void __attribute__((noreturn)) ota_enter_safe_mode(
    julia_fault_reason_t fault_reason, esp_err_t error, const char *reason)
{
    if (s_fault_nvs_ready) {
        /* OTA 启动验收早于行为 FSM；以 S7.2 身份落盘后沿用现有安全模式，
         * 不在无可回滚镜像时反复 reset。 */
        (void)julia_fault_record(fault_reason, error,
                                 JULIA_MAIN_STATE_S7_FAULT,
                                 JULIA_S2_SUB_STATE_NONE);
    }
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

/**
 * @brief 执行 OTA 启动验收与状态对账（应用初始化入口）。
 *
 * 函数在业务服务启动前，决定“刚从引擎写的镜像”是否保持有效。整体流程：
 *   1) 读取启动镜像 OTA 状态，得到 PENDING_VERIFY 标志；
 *   2) 初始化 NVS（不可恢复/不兼容时按“是否 PENDING_VERIFY”决定是否擦除）；
 *   3) 初始化状态上报与网络接口/事件循环；
 *   4) 上报 booted_pending_verify（若有待验收镜像）；
 *   5) 本地健康检查（分区/Flash/描述/堆/队列 + 可选 GPIO 诊断）与产品验收钩子；
 *   6) 通过 → confirm（标记 VALID 并取消回滚）+ 上报 succeeded；
 *      失败且可见回滚 → reject（进入回滚）+ 上报 rolled_back；
 *   7) 最后与运行版本对账：识别上次失败升级的回滚并上报，清理已确认版本的记录。
 *
 * 副作用：可能擦除/写 NVS，调用 esp_ota_mark_app_valid_cancel_rollback() 或
 * esp_ota_mark_app_invalid_rollback_and_reboot()，并上报若干生命周期事件；
 * 不可恢复错误会进入安全模式且不返回。
 *
 * 成功路径返回值为 void：不再向调用方报告错误，因为任何不可恢复错误已经
 * 在函数内部转入安全模式；只有“已确认/已对账”的正常结果才会返回，可继续启动
 * 业务服务。
 *
 * @note 必须在其他业务服务之前调用；只能在普通任务上下文调用（内部可能阻塞
 *       于 GPIO 诊断/NVS/Flash 操作）。不依赖 Wi-Fi 或 MQTT 可用性。
 */
void ota_boot_flow_run(void)
{
    bool pending_verify = false;
    esp_err_t err = ota_boot_health_begin(&pending_verify);
    if (err != ESP_OK) {
        ota_enter_safe_mode(JULIA_FAULT_FLASH_IO, err,
                            "cannot read OTA boot state");
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (pending_verify) {
            ESP_LOGE(TAG, "NVS format is incompatible during PENDING_VERIFY; refusing to erase NVS");
            err = ota_boot_health_reject("NVS format incompatible");
            ota_enter_safe_mode(JULIA_FAULT_OTA_ROLLBACK_UNAVAILABLE, err,
                                err == ESP_ERR_OTA_ROLLBACK_FAILED ?
                                "rollback unavailable after NVS failure" :
                                "rollback failed after NVS failure");
        }
        ESP_LOGW(TAG, "Erasing NVS because no image is pending verification");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ota_enter_safe_mode(JULIA_FAULT_NVS_UNRECOVERABLE, err,
                            "NVS initialization failed");
    }
    s_fault_nvs_ready = true;
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
            /* 区分“新镜像事实上无效、可回滚”与“根本没有旧镜像可回滚”：
             * 前者上报 BOOT_SELF_TEST_FAILED，后者上报 ROLLBACK_UNAVAILABLE。
             * 该三元判定在下方 product/confirm 失败分支中重复出现，含义相同。 */
            native_ota_failure_reason_t rollback_reason =
                esp_ota_check_rollback_is_possible() ?
                NATIVE_OTA_FAILURE_BOOT_SELF_TEST_FAILED :
                NATIVE_OTA_FAILURE_ROLLBACK_UNAVAILABLE;
            err = native_ota_report_boot_rolled_back(rollback_reason);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Could not report rolled_back: %s", esp_err_to_name(err));
            }
            err = ota_boot_health_reject("boot self-test failed");
            ota_enter_safe_mode(JULIA_FAULT_OTA_ROLLBACK_UNAVAILABLE, err,
                                err == ESP_ERR_OTA_ROLLBACK_FAILED ?
                                "rollback unavailable after self-test" :
                                "rollback failed after self-test");
        }
        ota_enter_safe_mode(JULIA_FAULT_CRITICAL_INIT, ESP_FAIL,
                            "local health check failed");
    }

    s_pending_verify = pending_verify;
    if (!pending_verify) ota_reconcile_boot_state(false);
}

static void ota_reconcile_boot_state(bool pending_verify)
{
    esp_err_t err;
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

void ota_boot_flow_complete(bool app_healthy)
{
    bool pending_verify = s_pending_verify;
    if (!pending_verify) return;
    esp_err_t err;
    /* A product override must finish critical local business initialization before a
     * PENDING_VERIFY image can become VALID. Remote connectivity is intentionally not
     * an acceptance condition. */
    if (!app_healthy || !ota_boot_health_product_check()) {
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
        ota_enter_safe_mode(JULIA_FAULT_OTA_ROLLBACK_UNAVAILABLE, err,
                            err == ESP_ERR_OTA_ROLLBACK_FAILED ?
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
            ota_enter_safe_mode(JULIA_FAULT_OTA_ROLLBACK_UNAVAILABLE, err,
                                "cannot confirm or rollback image");
        }
        err = native_ota_report_boot_succeeded();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not report succeeded: %s", esp_err_to_name(err));
        }
    }

    s_pending_verify = false;
    ota_reconcile_boot_state(true);
}
