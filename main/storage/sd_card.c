/**
 * @file    sd_card.c
 * @brief   以 SDMMC 1-bit 挂载 FAT，并通过共享 TCA9554 保证卡处于 SD mode。
 *
 * 只允许启动 owner 调用一次；没有 unmount、热插拔或失败重试。挂载失败不格式化
 * 介质，也不阻断其它业务。s_mounted 只是启动期结果快照，拔卡后不会自动清除。
 * SD 数据引脚和频率来自 Kconfig；共享 I2C 引脚由 tca9554.c 固定，当前
 * CONFIG_SD_CARD_I2C_SCL/SDA 不参与实际配置。
 */

#include <stdio.h>

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "sd_card.h"
#include "tca9554.h"

/* 该字符串同时属于 FILE_SEND URI 契约，修改时必须同步更新 voice_uri。 */
#define SD_CARD_MOUNT_POINT "/sdcard"
/** 同时打开的最大文件数（voice_service 单文件推送 + 预留）。 */
#define SD_CARD_MAX_FILES 4

static const char *TAG = "sd_card";

/* 只保护状态快照；不串行化文件 IO，也不能检测运行中拔卡。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_mounted;

esp_err_t sd_card_start(void)
{
#if CONFIG_SD_CARD_ENABLE
    /* 首次 SD clock 前必须先拉高 D3/CS，否则卡可能锁入 SPI mode。 */
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
    /* D3/CS 不连接主芯片，4-bit 模式无法满足引脚契约，只能使用 1-bit。 */
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    host.max_freq_khz = CONFIG_SD_CARD_FREQ_KHZ;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = CONFIG_SD_CARD_PIN_CLK;
    slot.cmd = CONFIG_SD_CARD_PIN_CMD;
    slot.d0 = CONFIG_SD_CARD_PIN_D0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
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
        /* 挂载失败不改变其它启动结果；后续 FILE_SEND 通过未挂载状态失败。 */
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
