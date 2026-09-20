/**
 * @file    ota_boot_flow.c
 * @brief   OTA 启动流程编排：run() 在外设之前识别镜像、保护 NVS、准备网络基础并对账；
 *          complete() 在本地结果确定后验收 pending 镜像，不以网络可用性判断健康。
 *
 * 边界：只做启动期编排与验收判定——不下载、不写应用分区（在 ota_engine.c），也不决定状态
 * 上报内容（在 ota_report.c）。
 * 约束：可选 GPIO 诊断必须早于外设复用引脚；其余额外检查不阻塞正常画面交接。
 */
#include "ota_boot_flow.h"

#include <stdbool.h>
#include <stdatomic.h>
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
 * @brief 已完成启动阶段 NVS 可用性判定的标志；就绪后才允许写 S7.2 故障快照。
 *
 * app_main 在创建验收任务前发布该值；此后只读。pending 标志另外使用原子访问。
 */
static bool s_fault_nvs_ready;
/** 本次启动是否在验收 PENDING_VERIFY 镜像；由 ota_boot_flow_run() 写入，
 *  ota_boot_flow_complete() 消费后清为 false。 */
static atomic_bool s_pending_verify;
static void ota_reconcile_boot_state(bool pending_verify);

/**
 * @brief 进入不启动通信客户端的 OTA 安全模式。
 *
 * @param[in] fault_reason 写入故障快照的原因分类。
 * @param[in] error 触发安全模式的底层错误码。
 * @param[in] reason 进入安全模式的诊断原因，可为 NULL。
 *
 * @note 本函数不返回、不重启，也不擦除 NVS；通过每秒延时保持任务可调度，等待人工处理
 *       或外部复位。只能在普通任务上下文调用。
 * @note 只有 NVS 已可用时才落盘 S7.2 故障快照：启动验收早于行为 FSM，此时无法保证
 *       能写 NVS，因此不在这里反复 reset。
 */
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

/* 由 app_main 串行调用。pending 下 NVS 不兼容绝不擦除；GPIO 诊断保留早期独占。 */
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
        /* 无待验收镜像时擦除 NVS 是最后手段：ota_report 未确认的关键上报和 ota_resume
         * 下载断点都保存在 NVS 中，擦除后一起丢失。代价是服务器可能收不到上一次的终态
         * 事件、设备只能从零重新下载；只有在 NVS 自身不可用（无空闲页/版本不兼容）时
         * 才接受这个代价。 */
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

    /* GPIO 诊断会重配引脚（当前 GPIO4 与 JULIA_LED_GPIO 复用），只能在外设启用前执行。
     * 仅 pending 镜像且显式启用诊断时保留该早期例外，不计入普通启动收益。 */
#if CONFIG_OTA_ENABLE_GPIO_DIAGNOSTIC
    if (pending_verify && !diagnostic()) {
        (void)native_ota_report_boot_rolled_back(esp_ota_check_rollback_is_possible() ?
            NATIVE_OTA_FAILURE_BOOT_SELF_TEST_FAILED : NATIVE_OTA_FAILURE_ROLLBACK_UNAVAILABLE);
        err = ota_boot_health_reject("early GPIO diagnostic failed");
        ota_enter_safe_mode(JULIA_FAULT_OTA_ROLLBACK_UNAVAILABLE, err, "GPIO diagnostic rejected");
    }
#endif

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

/**
 * @brief 在关键本地业务初始化完成后确认或回滚 PENDING_VERIFY 镜像。
 *
 * @param[in] app_healthy 关键显示/音频/语音/FSM 等本地服务是否已就绪。这个判断只允许
 *            依赖本地初始化结果：把 Wi-Fi、MQTT、DNS 等远程可用性算进来，会让弱网
 *            环境下的有效镜像被判定不健康并触发回滚。
 *
 * 只有本次启动确实在验收 PENDING_VERIFY 镜像时才起作用，重复调用是空操作。
 * 漏调的后果：镜像始终保持 PENDING_VERIFY，既不会被标记 VALID，也不会在本次启动产生
 * succeeded/rolled_back 上报；下一次复位时 bootloader 会把它当作未确认镜像判为无效并
 * 回滚，同时 READY_TO_COMMIT 断点记录也不会被对账清理。
 */
void ota_boot_flow_complete(bool app_healthy)
{
    bool pending_verify = s_pending_verify;
    if (!pending_verify) return;
    esp_err_t err;
    /* 产品验收钩子必须先完成关键本地业务初始化；远程连通性刻意不作为验收条件。 */
    if (!app_healthy || !ota_local_health_check(false) || !ota_boot_health_product_check()) {
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

    ota_reconcile_boot_state(true);
    s_pending_verify = false;
}

bool ota_boot_flow_pending(void)
{
    return atomic_load(&s_pending_verify);
}
