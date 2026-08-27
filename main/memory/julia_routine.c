#include "julia_routine.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "julia_sd.h"
#include "julia_voice.h"

#define TAG "routine"
#define ROUTINE_PATH JULIA_SD_MOUNT_POINT "/julia/memory/routine_v1.bin"
#define ROUTINE_MAGIC 0x4a525431U
#define ROUTINE_DAYS 7
#define ROUTINE_HOURS 24
#define FLUSH_INTERVAL_US (10LL * 60 * 1000000)
#define ACTIVE_GAP_SECONDS 120
#define DEVIATION_SECONDS (30 * 60)
#define REPORT_COOLDOWN_SECONDS (60 * 60)
#define PROBABILITY_PERCENT 15

typedef struct {
    uint16_t interactions;
    uint16_t active_minutes;
} routine_bucket_t;

typedef struct {
    uint32_t magic;
    uint32_t generation;
    int32_t day_id[ROUTINE_DAYS];
    routine_bucket_t buckets[ROUTINE_DAYS][ROUTINE_HOURS];
    uint32_t crc32;
} routine_store_t;

static SemaphoreHandle_t s_lock;
static routine_store_t s_store;
static bool s_dirty;
static int64_t s_last_flush_us;
static int64_t s_last_activity_second;
static int64_t s_continuous_start_second;
static int64_t s_last_report_second;
static int32_t s_last_active_minute = -1;
static uint8_t s_active_slot;

static uint32_t store_crc(const routine_store_t *store)
{
    return esp_crc32_le(0, (const uint8_t *)store, offsetof(routine_store_t, crc32));
}

static bool valid_store(const routine_store_t *store)
{
    return store->magic == ROUTINE_MAGIC && store->crc32 == store_crc(store);
}

static bool wall_clock(struct tm *local, time_t *now_out)
{
    time_t now = time(NULL);
    if (now < 1704067200) return false;
    localtime_r(&now, local);
    if (now_out) *now_out = now;
    return true;
}

static int slot_for_day(int32_t day_id, bool create)
{
    int empty = -1;
    int oldest = 0;
    for (int i = 0; i < ROUTINE_DAYS; ++i) {
        if (s_store.day_id[i] == day_id) return i;
        if (s_store.day_id[i] == 0 && empty < 0) empty = i;
        if (s_store.day_id[i] < s_store.day_id[oldest]) oldest = i;
    }
    if (!create) return -1;
    int slot = empty >= 0 ? empty : oldest;
    memset(s_store.buckets[slot], 0, sizeof(s_store.buckets[slot]));
    s_store.day_id[slot] = day_id;
    return slot;
}

static int learned_days_before(int32_t today)
{
    int days = 0;
    for (int i = 0; i < ROUTINE_DAYS; ++i)
        if (s_store.day_id[i] > 0 && s_store.day_id[i] < today) ++days;
    return days;
}

static esp_err_t write_snapshot(void)
{
    FILE *file = fopen(ROUTINE_PATH, "r+b");
    if (!file) file = fopen(ROUTINE_PATH, "w+b");
    if (!file) return ESP_FAIL;
    routine_store_t snapshot;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot = s_store;
    snapshot.generation++;
    snapshot.crc32 = store_crc(&snapshot);
    xSemaphoreGive(s_lock);
    uint8_t next_slot = s_active_slot ^ 1U;
    esp_err_t err = ESP_FAIL;
    if (fseek(file, (long)(next_slot * sizeof(snapshot)), SEEK_SET) == 0 &&
        fwrite(&snapshot, 1, sizeof(snapshot), file) == sizeof(snapshot) &&
        fflush(file) == 0 && fsync(fileno(file)) == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_store = snapshot;
        s_dirty = false;
        s_active_slot = next_slot;
        s_last_flush_us = esp_timer_get_time();
        xSemaphoreGive(s_lock);
        err = ESP_OK;
    }
    fclose(file);
    return err;
}

esp_err_t julia_routine_init(void)
{
    if (!julia_sd_is_mounted()) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    memset(&s_store, 0, sizeof(s_store));
    s_store.magic = ROUTINE_MAGIC;
    FILE *file = fopen(ROUTINE_PATH, "rb");
    if (file) {
        routine_store_t copies[2] = {0};
        fread(copies, sizeof(routine_store_t), 2, file);
        fclose(file);
        bool v0 = valid_store(&copies[0]);
        bool v1 = valid_store(&copies[1]);
        if (v0 || v1) {
            s_active_slot = v1 && (!v0 || copies[1].generation > copies[0].generation);
            s_store = copies[s_active_slot];
        }
    }
    s_last_flush_us = esp_timer_get_time();
    ESP_LOGI(TAG, "baseline ready: generation=%lu bytes=%u", (unsigned long)s_store.generation,
             (unsigned)sizeof(s_store) * 2U);
    return ESP_OK;
}

void julia_routine_on_activity(activity_kind_t kind)
{
    if (!s_lock || kind > JULIA_ACTIVITY_SENSOR) return;
    struct tm local;
    time_t now;
    if (!wall_clock(&local, &now)) return;
    int32_t day_id = (int32_t)(now / 86400);
    int32_t minute_id = (int32_t)(now / 60);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = slot_for_day(day_id, true);
    routine_bucket_t *bucket = &s_store.buckets[slot][local.tm_hour];
    if (kind == JULIA_ACTIVITY_DIALOG || kind == JULIA_ACTIVITY_WAKE) {
        if (bucket->interactions != UINT16_MAX) bucket->interactions++;
    }
    if (minute_id != s_last_active_minute) {
        if (bucket->active_minutes != UINT16_MAX) bucket->active_minutes++;
        s_last_active_minute = minute_id;
    }
    if (s_last_activity_second == 0 || now - s_last_activity_second > ACTIVE_GAP_SECONDS)
        s_continuous_start_second = now;
    s_last_activity_second = now;
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

bool julia_routine_is_deviation(void)
{
    if (!s_lock) return false;
    struct tm local;
    time_t now;
    if (!wall_clock(&local, &now)) return false;
    int32_t today = (int32_t)(now / 86400);
    bool deviation = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int days = learned_days_before(today);
    int active_days = 0;
    for (int i = 0; i < ROUTINE_DAYS; ++i) {
        if (s_store.day_id[i] > 0 && s_store.day_id[i] < today &&
            s_store.buckets[i][local.tm_hour].active_minutes > 0) ++active_days;
    }
    bool rare = days >= 3 && active_days * 100 < days * PROBABILITY_PERCENT;
    bool continuous = s_continuous_start_second > 0 &&
                      now - s_continuous_start_second >= DEVIATION_SECONDS &&
                      now - s_last_activity_second <= ACTIVE_GAP_SECONDS;
    bool cooled_down = s_last_report_second == 0 ||
                       now - s_last_report_second >= REPORT_COOLDOWN_SECONDS;
    if (rare && continuous && cooled_down) {
        s_last_report_second = now;
        deviation = true;
    }
    xSemaphoreGive(s_lock);
    if (deviation) {
        ESP_LOGW(TAG, "routine deviation: hour=%d active_days=%d/%d", local.tm_hour,
                 active_days, days);
        julia_voice_handle_event(EVT_ROUTINE_BREAK);
    }
    return deviation;
}

esp_err_t julia_routine_flush(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    return write_snapshot();
}

void julia_routine_background_flush(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool due = s_dirty && esp_timer_get_time() - s_last_flush_us >= FLUSH_INTERVAL_US;
    xSemaphoreGive(s_lock);
    if (due) {
        esp_err_t err = write_snapshot();
        if (err != ESP_OK) ESP_LOGE(TAG, "baseline flush failed: %s", esp_err_to_name(err));
    }
}
