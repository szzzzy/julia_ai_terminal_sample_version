/**
 * @file    julia_time.c
 * @brief   设备壁钟管理的实现：RTC 备份/恢复 + SNTP 网络校时 + 墙钟有效性查询。
 *
 * 职责边界：
 *   - 只解决"系统时钟/墙钟是否可信、如何让它可信"，与 julia_context 的 FSM 事件
 *     无关。两份时间是"同源不同侧重"：本模块负责给操作系统 set wall time 并跟踪
 *     SNTP 是否成功。
 *   - 关键的不变式：以 2024-01-01 (JULIA_VALID_EPOCH) 为"时间尚未被设置"的判据。
 *     任何早于此的时间一律视为无效（避免把 1970 年当成真时间触发行事）。
 *
 * 线程模型：
 *   - s_time_valid / s_sntp_started 由 s_lock（portMUX）保护，可被 IP-ready 回调
 *     （网络任务）与 julia_time_valid()（可能来自其它任务）并发读写。
 *   - 与 julia_context.c 是两条独立同步路径，都会写同一块 PCF85063 RTC（见问题清单）。
 */
#include "julia_time.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "pcf85063_shared.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

/* "时间已设置"的判定基准：2024-01-01 00:00:00 UTC 的 epoch。
 * 任何早于此的系统时间/ RTC 值都被视为无效（RTC 尚未写过、SNTP 还没同步）。 */
#define JULIA_VALID_EPOCH 1704067200LL /* 2024-01-01 00:00:00 UTC */

/* 日志标签与 julia_context 共用，便于在同一日志流里观察时间相关消息。 */
static const char *TAG = "JULIA_CONTEXT";
/* 墙钟是否已因"RTC 恢复或 SNTP 同步"而被标记为可信。 */
static bool s_time_valid;
/* SNTP 是否已经发起过（用于防止 ip_ready 回调重复启动）。 */
static bool s_sntp_started;
/* 保护上面两个布尔量不受并发读写竞争的临界区锁（可在 ISR? 否——SNTP 回调在任务上下文）。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * 校验一个板级 RTC 时间值的合理性。范围比 julia_context.c 的 valid_time 更严格
 * （含 minute/second < 60）。年份限制到 2069，与 RTC 芯片表示范围一致。
 * 返回 false 表示该值不可信（RTC 未设过/读出为 0/乱码），不应写入系统时钟。
 */
static bool datetime_valid(const board_rtc_datetime_t *value)
{
    return value != NULL && value->year >= 2024U && value->year <= 2069U &&
           value->month >= 1U && value->month <= 12U &&
           value->day >= 1U && value->day <= 31U &&
           value->hour < 24U && value->minute < 60U && value->second < 60U;
}

/*
 * 在临界区内置 s_time_valid = true。任何一方（RTC 恢复成功 / SNTP 同步成功）调用它，
 * 都说明系统获得了可信墙钟。
 */
static void mark_time_valid(void)
{
    portENTER_CRITICAL(&s_lock);
    s_time_valid = true;
    portEXIT_CRITICAL(&s_lock);
}

/*
 * @brief SNTP 同步回调：把同步后的系统时间写回 RTC（PCF85063）。
 * @param tv SNTP 交给回调的参数（通常是当前时间），本实现未使用。
 *
 * 调用上下文：SNTP 同步成功后由 lwIP/SNTP 任务回调，因此在任务上下文而非中断中。
 * 前置条件：RTC 已就绪且系统时钟有效（now >= JULIA_VALID_EPOCH）。
 * 失败路径：RTC 未就绪或系统时钟无效 -> 直接返回；写 RTC 失败 -> 只打警告、
 *           不置 valid（下次同步可能会再尝试）。
 * 副作用：写 RTC（I2C 事务）；成功后调用 mark_time_valid() 标记墙钟可信。
 */
static void sync_rtc_from_system(struct timeval *tv)
{
    (void)tv;
    if (!board_rtc_ready()) return;
    time_t now = time(NULL);
    if ((int64_t)now < JULIA_VALID_EPOCH) return;
    struct tm local;
    localtime_r(&now, &local);
    board_rtc_datetime_t value = {
        .year = (uint16_t)(local.tm_year + 1900),
        .month = (uint8_t)(local.tm_mon + 1),
        .day = (uint8_t)local.tm_mday,
        .dotw = (uint8_t)local.tm_wday,
        .hour = (uint8_t)local.tm_hour,
        .minute = (uint8_t)local.tm_min,
        .second = (uint8_t)local.tm_sec,
    };
    esp_err_t err = board_rtc_set_time(&value);
    if (err == ESP_OK) {
        mark_time_valid();
        ESP_LOGI(TAG, "SNTP synchronized RTC: %04u-%02u-%02u %02u:%02u:%02u",
                 value.year, value.month, value.day, value.hour, value.minute,
                 value.second);
    } else {
        ESP_LOGW(TAG, "RTC update after SNTP failed: %s", esp_err_to_name(err));
    }
}

/*
 * @brief 从板级 RTC 读时间并写回系统时钟（settimeofday）。
 * @return ESP_OK 成功；否则描述失败原因（读 RTC 失败 / RTC 无有效时间 / settimeofday 失败）。
 *
 * 目的：设备掉电期间靠 RTC（PCF85063 有独立电源）维持墙钟，重新上电后立即恢复，
 *       不必等网络/SNTP。这样时间相关判定（夜间、22 点、长离隔）在无网时也能工作。
 *
 * 细节：
 *   - 取值范围已由 datetime_valid() 校验，保证 tm 结构合法，mktime 才会得到正确的 epoch。
 *   - tm_isdst = -1 表示"让 mktime 决定是否夏令时"，避免人为指定错误。
 *   - 成功后才 mark_time_valid()。
 */
static esp_err_t restore_system_time_from_rtc(void)
{
    board_rtc_datetime_t value = {0};
    ESP_RETURN_ON_ERROR(board_rtc_read_time(&value), TAG, "read RTC failed");
    if (!datetime_valid(&value)) {
        ESP_LOGW(TAG, "RTC contains no valid time");
        return ESP_ERR_INVALID_STATE;
    }
    struct tm local = {
        .tm_sec = value.second,
        .tm_min = value.minute,
        .tm_hour = value.hour,
        .tm_mday = value.day,
        .tm_mon = value.month - 1,
        .tm_year = value.year - 1900,
        .tm_isdst = -1,
    };
    time_t epoch = mktime(&local);
    if (epoch < 0) return ESP_ERR_INVALID_STATE;
    struct timeval system_time = {.tv_sec = epoch, .tv_usec = 0};
    ESP_RETURN_ON_FALSE(settimeofday(&system_time, NULL) == 0, ESP_FAIL,
                        TAG, "settimeofday failed");
    mark_time_valid();
    ESP_LOGI(TAG, "restored wall time from RTC: %04u-%02u-%02u %02u:%02u:%02u",
             value.year, value.month, value.day, value.hour, value.minute, value.second);
    return ESP_OK;
}

/*
 * @brief 初始化设备壁钟：设置时区，并（若启用 RTC）从 RTC 恢复系统时间。
 * @return ESP_OK；即使 RTC 不可用也返回 OK（时间只是"努力凑"，不阻塞启动）。
 *
 * 前置条件：可由 app_main 早期调用；RTC 恢复失败或不可用均不致命（返回 OK），
 * 仅打日志，后续由 SNTP 异步补齐。配置项 CONFIG_JULIA_TIMEZONE 决定时区串。
 */
esp_err_t julia_time_init(void)
{
    setenv("TZ", CONFIG_JULIA_TIMEZONE, 1);
    tzset();
#if CONFIG_JULIA_RTC_ENABLE
    esp_err_t err = board_rtc_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC unavailable: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    err = restore_system_time_from_rtc();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "RTC restore failed: %s", esp_err_to_name(err));
    }
#endif
    return ESP_OK;
}

/*
 * @brief IP-ready 回调：网络取得 IPv4 后启动 SNTP 校时，并让同步回调写回 RTC。
 * @param arg 未使用（兼容 network_lifecycle 注册签名）。
 * @return ESP_OK；SNTP 启动失败返回对应错误，且允许下次重试（s_sntp_started 被复位）。
 *
 * 调用上下文：网络生命周期任务在拿到 IPv4 后调用，因此本函数在任务上下文执行；
 * 但设为"仅首次生效"：若已启动过（s_sntp_started=true）则立即返回 ESP_OK，防重复 init。
 * 失败路径：esp_netif_sntp_init 失败 -> 复位 s_sntp_started 并返回错误；
 *          本函数不阻塞等待同步（SNTP 是异步的，同步回调才处理结果）。
 */
esp_err_t julia_time_ip_ready(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_lock);
    bool already_started = s_sntp_started;
    if (!already_started) s_sntp_started = true;
    portEXIT_CRITICAL(&s_lock);
    if (already_started) return ESP_OK;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_JULIA_SNTP_SERVER);
    config.sync_cb = sync_rtc_from_system;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_sntp_started = false;
        portEXIT_CRITICAL(&s_lock);
        return err;
    }
    ESP_LOGI(TAG, "SNTP started server=%s timezone=%s",
             CONFIG_JULIA_SNTP_SERVER, CONFIG_JULIA_TIMEZONE);
    return ESP_OK;
}

/*
 * @brief 查询墙钟是否可信。
 * @return true 表示系统时间可用（RTC 恢复或 SNTP 同步已把这时间标为有效，
 *              或即便标志未置、系统时钟本身也已越过 2024-01-01 这一有效基准）。
 *
 * 设计要点：即便 s_time_valid 是 false（例如只做了 RTC 恢复但没走 mark_time_valid 的
 * 情况——实际都会走），也会用 now >= JULIA_VALID_EPOCH 兜底判断；因此"是否可信"
 * 最终由"系统时钟是否已超过 2024-01-01"这一硬基准决定，宽容地接受两条同步路径。
 */
bool julia_time_valid(void)
{
    bool valid;
    portENTER_CRITICAL(&s_lock);
    valid = s_time_valid;
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        time_t now = time(NULL);
        valid = (int64_t)now >= JULIA_VALID_EPOCH;
    }
    return valid;
}

