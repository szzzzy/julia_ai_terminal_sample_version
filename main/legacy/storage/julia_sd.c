/**
 * @file    julia_sd.c
 * @brief   未接入当前构建的 SD 挂载与破坏性带宽基准参考实现。
 *
 * main/CMakeLists.txt 不包含本文件；当前应用使用 sd_card.c。两者使用同一 /sdcard
 * 挂载点但没有共享状态或锁，本模块也不负责经 TCA9554 保持 SD mode，因此不得与
 * 现用挂载器同时初始化。format_if_mount_failed=true 可能格式化用户卡；benchmark
 * 还会覆盖并删除固定路径文件，只能在专用测试卡上使用。
 */

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

/* 这是未验证的参考引脚副本；现用引脚以 Kconfig + sd_card.c 为准。 */
#define SD_CLK_GPIO 14
#define SD_CMD_GPIO 17
#define SD_D0_GPIO  16

static const char *TAG = "JULIA_SD";
/* 这些状态只属于本参考模块，不反映 sd_card.c 是否已经挂载同一路径。 */
static sdmmc_card_t *s_card;
static bool s_mounted;
static SemaphoreHandle_t s_sd_mutex;

/**
 * @brief 挂载 SD（FAT 到 /sdcard），可重复调用（已挂载则直接返回 ESP_OK）。
 *
 * @param[in] format_if_mount_failed true 会授权 FAT 层在挂载失败时格式化介质，可能永久
 *                                   删除卡上数据；生产路径必须传 false。
 * @return ESP_OK 已挂载；其他 esp_err_t 挂载/内存失败。
 * 调用上下文：任务上下文（使用阻塞式版本），不应在中断调用。
 */
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

/**
 * @brief 获取 SD 文件访问互斥锁。
 * @param[in] timeout 等待超时（ticks）。
 * @return true 已拿到锁；false 超时/无锁。
 * 拿到锁后应配对调用 julia_sd_unlock()；文件访问应始终在锁内进行，
 * 避免与其他任务（如 UI 主题扫描）并发读写文件。
 */
bool julia_sd_lock(TickType_t timeout)
{
    return s_sd_mutex && xSemaphoreTake(s_sd_mutex, timeout) == pdTRUE;
}

/** 只能由成功取得锁的同一任务配对调用；该锁不保护绕过本模块的 SD 访问。 */
void julia_sd_unlock(void)
{
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
}

/**
 * @brief 顺序读带宽基准测试：写一个 16MiB 文件，分 3 次顺序读并统计 MB/s。
 *
 * @param[out] average_mbps 平均带宽（可选，可传 NULL）。
 * @param[out] minimum_mbps 三次中的最小带宽（可选，可传 NULL）。
 * @return ESP_OK 测量成功；ESP_ERR_INVALID_STATE 未挂载；ESP_ERR_NO_MEM 缓冲分配失败；
 *                其他 ESP_FAIL 写/读/重开失败。
 * 会覆盖并最终删除 /sdcard/julia/bench.bin；若该路径已有文件，原内容不可恢复。
 * 写入耗时不计入结果，因此数值只能表示本次顺序读取，不能当作完整存储性能。
 */
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
