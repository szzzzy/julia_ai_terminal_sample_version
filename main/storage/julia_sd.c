#include "julia_sd.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define SD_CLK_GPIO 14
#define SD_CMD_GPIO 17
#define SD_D0_GPIO  16

static const char *TAG = "JULIA_SD";
static sdmmc_card_t *s_card;
static bool s_mounted;
static SemaphoreHandle_t s_sd_mutex;

esp_err_t julia_sd_init(bool format_if_mount_failed)
{
    if (s_mounted) return ESP_OK;
    if (!s_sd_mutex) {
        s_sd_mutex = xSemaphoreCreateMutex();
        if (!s_sd_mutex) return ESP_ERR_NO_MEM;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 40000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK_GPIO;
    slot.cmd = SD_CMD_GPIO;
    slot.d0 = SD_D0_GPIO;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(JULIA_SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount%s failed: %s",
                 format_if_mount_failed ? "/format" : "", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    uint64_t bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    ESP_LOGI(TAG, "SD mounted: %llu MB, FAT filesystem ready", bytes / (1024 * 1024));
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool julia_sd_is_mounted(void) { return s_mounted; }

bool julia_sd_lock(TickType_t timeout)
{
    return s_sd_mutex && xSemaphoreTake(s_sd_mutex, timeout) == pdTRUE;
}

void julia_sd_unlock(void)
{
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
}

esp_err_t julia_sd_benchmark_read(float *average_mbps, float *minimum_mbps)
{
    enum { TEST_BYTES = 16 * 1024 * 1024, BLOCK_BYTES = 64 * 1024, PASSES = 3 };
    const char *path = JULIA_SD_MOUNT_POINT "/julia/bench.bin";
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    vTaskDelay(pdMS_TO_TICKS(500));
    mkdir(JULIA_SD_MOUNT_POINT "/julia", 0775);
    uint8_t *buffer = heap_caps_malloc(BLOCK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) { ESP_LOGE(TAG, "SD benchmark PSRAM buffer allocation failed"); return ESP_ERR_NO_MEM; }
    memset(buffer, 0xa5, BLOCK_BYTES);

    /* 先生成连续文件；写入耗时不计入顺序读取带宽。 */
    FILE *file = fopen(path, "wb");
    if (!file) { ESP_LOGE(TAG, "SD benchmark create failed: %s errno=%d", path, errno); free(buffer); return ESP_FAIL; }
    for (size_t done = 0; done < TEST_BYTES; done += BLOCK_BYTES) {
        if (fwrite(buffer, 1, BLOCK_BYTES, file) != BLOCK_BYTES) {
            ESP_LOGE(TAG, "SD benchmark write failed at %u bytes", (unsigned)done);
            fclose(file); remove(path); free(buffer); return ESP_FAIL;
        }
    }
    fflush(file);
    fclose(file);

    float total = 0.0f;
    float minimum = 1000.0f;
    esp_err_t result = ESP_OK;
    for (int pass = 0; pass < PASSES; ++pass) {
        file = fopen(path, "rb");
        if (!file) { ESP_LOGE(TAG, "SD benchmark reopen failed, pass=%d", pass + 1); result = ESP_FAIL; break; }
        size_t bytes = 0;
        int64_t started_us = esp_timer_get_time();
        while (bytes < TEST_BYTES) {
            size_t got = fread(buffer, 1, BLOCK_BYTES, file);
            if (got == 0) break;
            bytes += got;
        }
        int64_t elapsed_us = esp_timer_get_time() - started_us;
        fclose(file);
        if (bytes != TEST_BYTES || elapsed_us <= 0) {
            ESP_LOGE(TAG, "SD benchmark short read: pass=%d bytes=%u elapsed=%lld",
                     pass + 1, (unsigned)bytes, elapsed_us);
            result = ESP_FAIL; break;
        }
        float mbps = ((float)bytes / (1024.0f * 1024.0f)) /
                     ((float)elapsed_us / 1000000.0f);
        total += mbps;
        if (mbps < minimum) minimum = mbps;
        ESP_LOGI(TAG, "SD sequential read pass %d: %.2f MB/s (%u bytes, %lld us)",
                 pass + 1, (double)mbps, (unsigned)bytes, elapsed_us);
    }
    remove(path);
    free(buffer);
    if (result == ESP_OK) {
        if (average_mbps) *average_mbps = total / PASSES;
        if (minimum_mbps) *minimum_mbps = minimum;
        ESP_LOGI(TAG, "SD sequential read summary: avg=%.2f MB/s min=%.2f MB/s",
                 (double)(total / PASSES), (double)minimum);
    }
    return result;
}
