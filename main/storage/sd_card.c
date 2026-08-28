/**
 * @file    sd_card.c
 * @brief   SD 卡（FAT）挂载：SDMMC 1-bit + TCA9554 控制 CS。
 *
 * 接法与 VOICE DATA BENCHMARK/components/board_hal/sd_card.c 一致（该参考
 * 工程已在目标板成功写入 SD）：
 * - SDMMC 1-bit：CLK=GPIO14、CMD=GPIO17、D0=GPIO16，20 MHz；
 * - SD 卡 D3/CS 经 TCA9554 P2（Extend_IO3）路由，挂载前先把 CS 拉高，
 *   确保卡不误入 SPI 模式；
 * - 挂载点 /sdcard，format_if_mount_failed=false（绝不格式化用户的卡）；
 * - 缺卡不阻断应用：返回错误，voice_service 推送会以 ERROR file_open_failed
 *   呈现，不会影响 OTA/MQTT/WSS 会话。
 *
 * NOTE（需结合调用方确认）：
 * - 本文件是“单次同步挂载”，内部没有失败自动重试或卡拔出检测逻辑；main.c 中
 *   “挂载失败会自动重试”的注释与本实现不符（真实行为是失败即返回，依赖上层/
 *   网络生命周期另行处理）。若确实需要自动重试应在此处或在调用方自行实现。
 * - voice_service.c 的注释称“由 sd_card.c 提供强符号 julia_wireless_sd_lock/unlock”，
 *   但当前仓库未见这两个强符号定义（仅 voice_service.c 内有弱默认实现），SD 访问锁
 *   是否真实生效需确认。
 * - /sdcard 挂载点同时被 main/storage/julia_sd.c 使用（其 julia_sd_init() 也会挂载同一
 *   挂载点）。两者若同时调用 esp_vfs_fat_sdmmc_mount() 会因已挂载而冲突，需确认启用顺序。
 */

#include <stdio.h>

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "sd_card.h"
#include "tca9554.h"

/** 挂载点：与 voice_uri.c 的 "SD:/x" -> "/sdcard/x" 映射一致。 */
#define SD_CARD_MOUNT_POINT "/sdcard"
/** 同时打开的最大文件数（voice_service 单文件推送 + 预留）。 */
#define SD_CARD_MAX_FILES 4

/** 本模块统一使用的日志标签。 */
static const char *TAG = "sd_card";

/** 保护挂载状态标志的自旋锁；挂载本身在 app_main 启动路径中串行执行。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_mounted;

esp_err_t sd_card_start(void)
{
#if CONFIG_SD_CARD_ENABLE
    /* SD 卡 D3/CS 经 TCA9554 P2 (Extend_IO3) 路由：先初始化扩展器并把 CS 拉高，
     * 再触碰卡片，确保卡始终处于 SDMMC(SD) 模式。 */
    esp_err_t err = tca9554_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tca9554 init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = tca9554_write_pin(TCA9554_PIN_SD_CS, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set SD CS high failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG; /* 强制 1-bit */
    host.max_freq_khz = CONFIG_SD_CARD_FREQ_KHZ;                    /* 默认 20 MHz */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = CONFIG_SD_CARD_PIN_CLK;
    slot.cmd = CONFIG_SD_CARD_PIN_CMD;
    slot.d0 = CONFIG_SD_CARD_PIN_D0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC; /* CS 在 TCA9554 上，不在 GPIO */
    slot.width = 1;
    slot.flags = 0;

    /* 绝不格式化用户的卡：挂载失败就报错，而不是销毁数据。 */
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = SD_CARD_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        /* 缺卡/接触不良：不阻断应用，推送会以 ERROR file_open_failed 呈现。 */
        ESP_LOGW(TAG, "SD mount failed (%s); voice FILE_SEND will report file_open_failed",
                 esp_err_to_name(err));
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    s_mounted = true;
    portEXIT_CRITICAL(&s_lock);
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD mounted at %s (name=%s, size=%llu MB, freq=%d kHz)",
             SD_CARD_MOUNT_POINT, (const char *)card->cid.name,
             ((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024),
             card->real_freq_khz);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool sd_card_is_mounted(void)
{
    bool mounted;
    portENTER_CRITICAL(&s_lock);
    mounted = s_mounted;
    portEXIT_CRITICAL(&s_lock);
    return mounted;
}
