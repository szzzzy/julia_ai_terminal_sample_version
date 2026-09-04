/**
 * @file    julia_routine.c
 * @brief   未参与当前构建的日常活动偏差参考实现。
 *
 * main/CMakeLists.txt 未包含本文件；当前固件不会学习或上报用户例行模式。以下阈值
 * 是参考实现参数，不是已经标定或验收的产品指标。
 *
 * 数据流（输入 → 记账 → 偏差判定 → 输出）：
 *  - 输入：各任务上报的活动种类（DIALOG/WAKE/BUTTON/SENSOR），以及
 *    context 任务每 500ms 轮询的当前时间。
 *  - 记账：julia_routine_on_activity() 把活动累加到“某天(day_id)×某小时”桶里，
 *    桶含 interactions（交互次数）与 active_minutes（活跃分钟）。
 *  - 基线：以“天”为单位滚动，ROUTINE_DAYS=7 个槽位，写满后覆盖最旧的一天；
 *    以“小时”分桶（ROUTINE_HOURS=24）。整体持久化在 SD 卡 routine_v1.bin，
 *    采用双份拷贝（ping-pong）+ generation 老化回退，防崩溃损坏。
 *  - 偏差判定：julia_routine_is_deviation() 三条件“与”起来——
 *    ① 当前小时在本窗口“活跃天数 / 学习天数”占比 < 15%（PROBABILITY_PERCENT）；
 *    ② 连续活跃未中断（ACTIVE_GAP_SECONDS=120s 内有活动）且已持续 >=30 分钟
 *       （DEVIATION_SECONDS）；
 *    ③ 距上次触发已超过 1 小时（REPORT_COOLDOWN_SECONDS，冷却防刷屏）。
 *  - 输出：三条件成立即发 julia_voice_handle_event(EVT_ROUTINE_BREAK)，OS 进入
 *    “例行偏差”子状态并主动关心用户。
 *
 * 时间前提：所有记账都以 wall-clock 为准（now >= 1704067200，即 2024-01-01）。
 * 在 RTC/SNTP 对齐前各入口静默返回，避免把 uptime 误当日历时间。
 *
 * 并发：s_lock 保护 s_store 与一组“上次…”计时器；写盘由后台 flush 触发，
 * READ 端（is_deviation）持锁读取一致快照。写盘失败仅日志，不影响内存基线。
 */

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
#define ROUTINE_MAGIC 0x4a525431U                      /* 'JRT1' */
#define ROUTINE_DAYS 7                                 /* 滚动天数窗口 */
#define ROUTINE_HOURS 24                               /* 一天 24 小时分桶 */
#define FLUSH_INTERVAL_US (10LL * 60 * 1000000)        /* 后台落盘周期：10 分钟 */
#define ACTIVE_GAP_SECONDS 120                         /* 超过 120s 无活动视为连续中断 */
#define DEVIATION_SECONDS (30 * 60)                    /* 连续活跃达到 30 分钟才算“异常密集” */
#define REPORT_COOLDOWN_SECONDS (60 * 60)              /* 两次触发最少间隔 1 小时 */
#define PROBABILITY_PERCENT 15                         /* 当前小时活跃占比低于 15% 视为反常 */

/* 单个“天×小时”桶：交互次数与活跃分钟，均为 u16（达上限后不再累加）。 */
typedef struct {
    uint16_t interactions;
    uint16_t active_minutes;
} routine_bucket_t;

/* 基线存档。day_id[i] 是该槽位对应的“天编号”（now/86400），0 表示空槽。
 * buckets[ROUTINE_DAYS][ROUTINE_HOURS] 为该天各小时的桶。
 * 布局为定长（含 magic + generation + crc32），便于按 sizeof 整体读写与校验。 */
typedef struct {
    uint32_t magic;
    uint32_t generation;
    int32_t day_id[ROUTINE_DAYS];
    routine_bucket_t buckets[ROUTINE_DAYS][ROUTINE_HOURS];
    uint32_t crc32;
} routine_store_t;

/* 状态：s_store 为当前基线；s_dirty 表示内存基线比磁盘新；s_last_flush_us 为上次
 * 成功落盘的时刻；一系列“上次…”记录用于连续活跃/冷却判定；s_active_slot 指示当前
 * 正在使用的拷贝槽位（配对写另一份）。 */
static SemaphoreHandle_t s_lock;
static routine_store_t s_store;
static bool s_dirty;
static int64_t s_last_flush_us;
static int64_t s_last_activity_second;
static int64_t s_continuous_start_second;
static int64_t s_last_report_second;
static int32_t s_last_active_minute = -1;
static uint8_t s_active_slot;

/* 对 crc32 字段之前的所有字节做 CRC32。 */
static uint32_t store_crc(const routine_store_t *store)
{
    return esp_crc32_le(0, (const uint8_t *)store, offsetof(routine_store_t, crc32));
}

static bool valid_store(const routine_store_t *store)
{
    return store->magic == ROUTINE_MAGIC && store->crc32 == store_crc(store);
}

/* 取得本地时区时间；若 wall-clock 尚未同步到 2024 之后则失败返回 false。 */
static bool wall_clock(struct tm *local, time_t *now_out)
{
    time_t now = time(NULL);
    if (now < 1704067200) return false;
    localtime_r(&now, local);
    if (now_out) *now_out = now;
    return true;
}

/* 查找 day_id 对应的槽位。
 *  - 命中已有槽位直接返回；有一个空槽则使用之；
 *  - 否则覆盖 day_id 最小（最久远）的槽位（环形覆盖），并清零该槽位的桶；
 *  - create=false 时只查不建，用于查询历史；找不到返回 -1。
 * 说明：day_id == 0 被当作“空槽”标志，因此真实的“天编号 0”不可用（无实际影响）。 */
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

/* 统计窗口内严格早于“今天”的已学习天数（用于算活跃占比的分母）。 */
static int learned_days_before(int32_t today)
{
    int days = 0;
    for (int i = 0; i < ROUTINE_DAYS; ++i)
        if (s_store.day_id[i] > 0 && s_store.day_id[i] < today) ++days;
    return days;
}

/* 把当前基线写盘：先拷一份快照（+generation、重算 CRC），fsync 后更新
 * 内存中的 s_store/s_dirty/s_active_slot/s_last_flush_us。
 * 写入相邻（异或）槽位，崩溃时保留上一版合法快照。 */
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

/* 初始化例行检测。前置：SD 已挂载。创建锁、置空基线、读入两份快照（取合法且
 * generation 更大者作为当前基线），并记录当前时刻为“上次落盘时间”。 */
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
            /* 任一份合法即可；都合法时取 generation 更大者（更晚写入）。 */
            s_active_slot = v1 && (!v0 || copies[1].generation > copies[0].generation);
            s_store = copies[s_active_slot];
        }
    }
    s_last_flush_us = esp_timer_get_time();
    ESP_LOGI(TAG, "baseline ready: generation=%lu bytes=%u", (unsigned long)s_store.generation,
             (unsigned)sizeof(s_store) * 2U);
    return ESP_OK;
}

/* 上报一次活动。kind 非法(>SENSOR)或时钟未同步则忽略。
 * 记账规则：
 *  - DIALOG/WAKE 累加当前小时桶的 interactions（u16 饱和）；
 *  - 若“当前分钟”与上次不同，则累加当前小时桶的 active_minutes（u16 饱和）；
 *  - 若距上次活动超过 ACTIVE_GAP_SECONDS（或首次），重置“连续活跃”起点；
 *  - 更新 s_last_activity_second，并置 s_dirty 表示内存基线比磁盘新。
 * 副作用：仅改内存，落盘交给后台 flush（10 分钟周期）。 */
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

/* 检测“日常偏差”，三条件同时成立才触发：
 *  ① rare：已学习天数 >=3，且当前小时“活跃天数 / 学习天数”不足 15%；
 *  ② continuous：自连续活跃起点已 >=30 分钟，且最近一次活动在 120s 内（仍在持续）；
 *  ③ cooled_down：距上次触发已 >=1 小时。
 * 命中后记录本次触发时刻并返回 true；随后触发 EVT_ROUTINE_BREAK。
 * 由 julia_context 的 context 任务每 500ms 轮询（仅供判定，不负责记账）。 */
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

/* 立即落盘当前基线。 */
esp_err_t julia_routine_flush(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    return write_snapshot();
}

/* 后台延迟落盘：仅当存在未落盘改动(s_dirty)且距上次成功落盘 >=10 分钟时写入。
 * 由记忆模块的事件写入任务（memory_writer）每个循环调用一次，避免另起线程。 */
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
