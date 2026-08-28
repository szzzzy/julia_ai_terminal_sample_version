/**
 * @file    julia_display_theme.c
 * @brief   显示功率/主题策略（fused 移植；当前未纳入构建）。
 *
 * 模块职责与边界：
 *   - 用"显示功率状态机"管理 LCD 背光与画面：完整亮度 → 自动调暗 → 进入 doze
 *     （低亮度呼吸 + 睡眠立绘）→ 唤醒回落。同时提供主题（背景色/呼吸灯色）的加载
 *     与切换，并支持从 SD 卡 /sdcard/julia/themes/<name>/theme.json 读取自定义主题。
 *   - 上游：julia_ui（状态迁移时回调 julia_display_theme_on_state_transition）、
 *     交互事件（julia_display_theme_on_interaction）、以及一个周期更新回调
 *     julia_display_theme_update()（调用方按帧率/固定节奏驱动）。
 *
 * 构建状态（重要）：
 *   - main/CMakeLists.txt 的 srcs 未纳入本文件；运行时实际生效的息屏/功率策略由
 *     app/julia_idle_display.c 承担（更精简的三态 ACTIVE/QUIET/SLEEP）。因此本文件为
 *     遗留/备用实现，可能由另一条链路或未来版本启用。
 *   NOTE：需结合调用方确认——是否仍被引用；否则本文件可整体裁剪。
 *
 * 关键设计：
 *   - 显示降功耗：24/7 居家伙伴不宜让背光/全彩面板常亮（过热/耗电/缩短续航），因此
 *     空闲时把显示功率分级下压，保留 WS2812 呼吸灯作为低功耗状态指示，语音/状态处理
 *     不受影响（见文件头部注释）。
 *   - 亮度渐变：每个折向都从硬件当前值出发，用二次缓动（ease-out）插值，避免跳变。
 *   - doze 进入/退出：进入时暂停微动、切睡眠立绘、起低亮度呼吸；退出时先淡入再恢复，
 *     并用 lvgl_port 的 flush 计数探测"首帧已出"，避免唤醒闪黑。
 */
#include "julia_display_theme.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "julia_led.h"
#include "julia_backlight.h"
#include "julia_memory.h"
#include "julia_sd.h"
#include "julia_ui.h"
#include "lvgl_port.h"
#include "nvs.h"
#include "freertos/task.h"
#include "avatar_micro_motion.h"
#include "breathing_led.h"
#include "avatar_face.h"

/* A 24/7 home companion cannot leave its LCD backlight and full-color panel
 * active indefinitely: they dominate power use, heat the enclosure, shorten
 * unplugged runtime, and increase long-term plugged-in risk. Idle display
 * power is therefore staged down while the existing WS2812 remains the sole
 * low-power status indicator; voice/state processing continues unchanged. */
/* 可调策略参数（#ifndef 以便编译期覆盖）：屏保超时、调暗/夜间亮度、夜间时段、
 * doze 背光百分比、doze 开关、doze 进入/退出渐变时长等全部集中在此，避免散落魔法数。 */
#define THEME_MAX 12
#define THEME_NAME 24
#ifndef SCREEN_OFF_TIMEOUT_S
#define SCREEN_OFF_TIMEOUT_S 300U
#endif
#ifndef SCREEN_DIM_PERCENT
#define SCREEN_DIM_PERCENT 30U
#endif
#ifndef SCREEN_NIGHT_DIM_PERCENT
#define SCREEN_NIGHT_DIM_PERCENT 15U
#endif
#ifndef SCREEN_NIGHT_START_HOUR
#define SCREEN_NIGHT_START_HOUR 23
#endif
#ifndef SCREEN_NIGHT_END_HOUR
#define SCREEN_NIGHT_END_HOUR 7
#endif
#ifndef DOZE_BACKLIGHT_PERCENT
#define DOZE_BACKLIGHT_PERCENT 5U
#endif
#ifndef JULIA_DOZE_ENABLE
#define JULIA_DOZE_ENABLE 0
#endif
#ifndef JULIA_DOZE_BREATH_ENABLE
#define JULIA_DOZE_BREATH_ENABLE 1
#endif
#ifndef JULIA_DISPLAY_LOG
#define JULIA_DISPLAY_LOG 1
#endif
#ifndef DOZE_BREATHE_MIN_PERCENT
#define DOZE_BREATHE_MIN_PERCENT 0U
#endif
#ifndef DOZE_BREATHE_MAX_PERCENT
#define DOZE_BREATHE_MAX_PERCENT 30U
#endif
#ifndef DOZE_BREATHE_PERIOD_MS
#define DOZE_BREATHE_PERIOD_MS 8000U
#endif
#define DOZE_TRANSITION_DEBOUNCE_MS 800U
#ifndef DOZE_NIGHT_BACKLIGHT_PERCENT
#define DOZE_NIGHT_BACKLIGHT_PERCENT 3U
#endif
#ifndef DOZE_FADE_MS
#define DOZE_FADE_MS 800U
#endif
#ifndef DOZE_WAKE_FADE_MS
#define DOZE_WAKE_FADE_MS 300U
#endif
#define DEFAULT_IDLE_MS (SCREEN_OFF_TIMEOUT_S * 1000U)
#define BACKLIGHT_FADE_MS 800U
#define WAKE_FADE_MS DOZE_WAKE_FADE_MS

/* 显示功率状态机：ON 完整亮度 → FADE_IN/DIM/FADE_OUT → OFF（doze 低亮度呼吸），
 * 以及 WAKE_WAIT_FLUSH（等首帧刷完再提升亮度，避免唤醒闪黑）。 */
typedef enum {
    DISPLAY_POWER_ON = 0,       ///< 全亮。
    DISPLAY_POWER_FADE_IN,      ///< 正在回升到全亮。
    DISPLAY_POWER_DIM,          ///< 已调暗（半亮度）。
    DISPLAY_POWER_FADE_OUT,     ///< 正在降到熄灭（进入 doze）。
    DISPLAY_POWER_OFF,          ///< 已熄灭（doze 低亮度呼吸中）。
    DISPLAY_POWER_WAKE_WAIT_FLUSH, ///< 等待唤醒后首帧刷出，再回升亮度。
} display_power_state_t;

/* 主题：内建 3 个，SD 卡上可扩展（s_themes 数组，最多 THEME_MAX）。 */
typedef struct { char name[THEME_NAME]; uint32_t background; uint32_t led; bool sd; } theme_t;
static theme_t s_themes[THEME_MAX] = {
    {"default", 0xf3f5f7, 0xffd7b0, false},
    {"night",   0x101722, 0x7898c8, false},
    {"fresh",   0xe9f5ee, 0x86d3ac, false},
};
static size_t s_count = 3, s_current;
static julia_backlight_setter_t s_setter;
static uint32_t s_last_interaction, s_idle_timeout = DEFAULT_IDLE_MS;
static bool s_rendering = true;                 /* 是否允许正常渲染（doze 时置 false）。 */
static bool s_doze_asset_loaded;
static display_power_state_t s_power_state = DISPLAY_POWER_ON;
static uint8_t s_backlight_current = 10;        /* 当前亮度（从硬件读回）。 */
static uint8_t s_backlight_from = 10;           /* 本次渐变的起点亮度。 */
static uint8_t s_backlight_target = 10;         /* 本次渐变的终点亮度。 */
static uint32_t s_fade_started_ms;
static uint32_t s_fade_duration_ms = BACKLIGHT_FADE_MS;
static bool s_fading;
static bool s_boot_brightness_hold;             /* 开机升亮期间的保持（避免被调暗抢占）。 */
static uint64_t s_wake_flush_baseline;          /* 唤醒前 flush 计数基线。 */
static uint32_t s_wake_started_ms;
static uint8_t s_restore_brightness = 100;      /* doze 退出后要恢复到的亮度。 */
static julia_sub_state_t s_state = JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
static bool s_doze_transitioning;               /* doze 进入/退出进行中（自旋锁原子量）。 */
static uint32_t s_doze_transition_finished_ms;  /* doze 切换完成时刻（用于防抖）。 */
static const char *TAG = "DISPLAY_THEME";

/* 由当前 FSM 子状态推导目标亮度：待机/主动 100%，陪伴 40%，其余（睡眠/静默）10%。 */
static uint8_t state_brightness(void)
{
    if (s_state >= JULIA_SUB_STATE_S1_1_NEAR_STANDBY &&
        s_state <= JULIA_SUB_STATE_S1_3_CHARGING_STANDBY) return 100;
    if (s_state >= JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER &&
        s_state <= JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE) return 100;
    if (s_state >= JULIA_SUB_STATE_S2_1_OBSERVE &&
        s_state <= JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION) return 40;
    return 10;
}

/* 启动一次亮度渐变：每次从硬件当前值出发（不是上一次目标值），
 * 记录起点/终点/起算时刻/时长，并判断是否确实需要渐变。 */
static void start_fade_for(uint8_t target, uint32_t now_ms, uint32_t duration_ms)
{
    /* 每次折向都从硬件当前值出发，不能从上一次目标值起跳。 */
    s_backlight_current = julia_backlight_get_percent();
    s_backlight_from = s_backlight_current;
    s_backlight_target = target;
    s_fade_started_ms = now_ms;
    s_fade_duration_ms = duration_ms ? duration_ms : 1U;
    s_fading = s_backlight_from != target;
}

/* 逐帧推进亮度渐变：用 ease-out 二次缓动插值，调用 s_setter 写入背光；
 * 结束后夹到目标值并清 fading，返回是否已完成。 */
static bool update_fade(uint32_t now_ms)
{
    if (!s_fading) return true;
    uint32_t elapsed = now_ms - s_fade_started_ms;
    if (elapsed > s_fade_duration_ms) elapsed = s_fade_duration_ms;
    uint32_t t = elapsed * 1024U / s_fade_duration_ms;
    uint32_t eased = 1024U - ((1024U - t) * (1024U - t) / 1024U);
    int delta = (int)s_backlight_target - (int)s_backlight_from;
    s_backlight_current = (uint8_t)((int)s_backlight_from +
                                    delta * (int)eased / 1024);
    s_setter(s_backlight_current);
    if (elapsed == s_fade_duration_ms) {
        s_backlight_current = s_backlight_target;
        s_setter(s_backlight_target);
        s_fading = false;
    }
    return !s_fading;
}

static void start_fade(uint8_t target, uint32_t now_ms)
{
    start_fade_for(target, now_ms, BACKLIGHT_FADE_MS);
}

/* 是否处于夜间时段（跨 23:00..次日 07:00）。时间未同步（epoch 过小）视为非夜间。 */
static bool night_time(void)
{
    time_t now = time(NULL);
    if (now < 1704067200) return false;
    struct tm local;
    localtime_r(&now, &local);
    return local.tm_hour >= SCREEN_NIGHT_START_HOUR || local.tm_hour < SCREEN_NIGHT_END_HOUR;
}

/* 从 theme.json 读取一个 24bit 颜色字段：支持数字(0xRRGGBB)或 "#RRGGBB" 字符串，
 * 缺省回退 fallback。 */
static uint32_t json_color(const cJSON *root, const char *key, uint32_t fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(value)) return (uint32_t)value->valuedouble & 0xffffffU;
    if (cJSON_IsString(value) && value->valuestring) {
        const char *s = value->valuestring;
        if (*s == '#') ++s;
        return (uint32_t)strtoul(s, NULL, 16) & 0xffffffU;
    }
    return fallback;
}

/* 扫描 SD 卡 /sdcard/julia/themes 下的每个主题目录，读取 theme.json 并注册到
 * s_themes（最多 THEME_MAX 个）。名称过长/JSON 非法则跳过。整个读取在 SD 锁内。 */
static void scan_sd_themes(void)
{
    if (!julia_sd_is_mounted() || !julia_sd_lock(pdMS_TO_TICKS(1500))) return;
    DIR *dir = opendir("/sdcard/julia/themes");
    struct dirent *entry;
    while (dir && s_count < THEME_MAX && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        if (strlen(entry->d_name) >= THEME_NAME) {
            ESP_LOGW(TAG, "skip theme with overlong name");
            continue;
        }
        char path[128];
        snprintf(path, sizeof(path), "/sdcard/julia/themes/%.23s/theme.json", entry->d_name);
        FILE *file = fopen(path, "rb");
        if (!file) continue;
        char json[512]; size_t got = fread(json, 1, sizeof(json) - 1, file); fclose(file); json[got] = 0;
        cJSON *root = cJSON_Parse(json);
        if (!root) { ESP_LOGW(TAG, "skip invalid theme=%s", entry->d_name); continue; }
        theme_t *theme = &s_themes[s_count++];
        snprintf(theme->name, sizeof(theme->name), "%.23s", entry->d_name);
        theme->background = json_color(root, "background_color", 0xf3f5f7);
        theme->led = json_color(root, "breathing_led_color", 0xffd7b0);
        theme->sd = true;
        cJSON_Delete(root);
        ESP_LOGI(TAG, "registered SD theme=%s", theme->name);
    }
    if (dir) closedir(dir);
    julia_sd_unlock();
}

/* 应用当前主题：设背景色 + 呼吸灯颜色；remember=true 时把主题名写入 NVS 并记一条
 * 记忆事件（下次开机恢复）。 */
static void apply_current(bool remember)
{
    theme_t *theme = &s_themes[s_current];
    julia_ui_apply_theme(theme->background, 250);
    julia_led_set_breathing(3, 12, 4000, theme->led);
    if (remember) {
        nvs_handle_t nvs;
        if (nvs_open("julia_theme", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, "current", theme->name); nvs_commit(nvs); nvs_close(nvs);
        }
        char event[64]; snprintf(event, sizeof(event), "theme:%s", theme->name);
        julia_memory_append(3, JULIA_MEMORY_EMOTION_NONE, event);
    }
    ESP_LOGI(TAG, "theme=%s source=%s", theme->name, theme->sd ? "sd" : "builtin");
}

/* 初始化：登记 setter、扫描 SD 主题、从 NVS 恢复上次主题，并把背光/渐变状态清零。
 * 注意把亮度设为 0（背光保持灭），由调用方后续 display_fade_in 点亮。 */
esp_err_t julia_display_theme_init(julia_backlight_setter_t setter)
{
    if (!setter) return ESP_ERR_INVALID_ARG;
    s_setter = setter; s_last_interaction = (uint32_t)(esp_timer_get_time() / 1000);
    scan_sd_themes();
    char saved[THEME_NAME] = {0}; size_t length = sizeof(saved); nvs_handle_t nvs;
    if (nvs_open("julia_theme", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_str(nvs, "current", saved, &length); nvs_close(nvs);
    }
    if (saved[0]) for (size_t i = 0; i < s_count; ++i) if (!strcmp(saved, s_themes[i].name)) s_current = i;
    s_setter(0);
    s_backlight_current = s_backlight_from = s_backlight_target = 0;
    s_fading = false;
    return ESP_OK;
}

/* 开机淡入：从 0% 升到 100%。期间置 boot_brightness_hold，防止空闲调暗抢占。 */
void display_fade_in(uint16_t duration_ms)
{
    if (!s_setter) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_setter(0);
    s_backlight_current = 0;
    start_fade_for(100, now_ms, duration_ms);
    s_power_state = DISPLAY_POWER_FADE_IN;
    s_boot_brightness_hold = true;
    s_last_interaction = now_ms;
    ESP_LOGI(TAG, "boot fade-in start from=0 target=100 duration_ms=%u", duration_ms);
}

/* 状态迁移入口的快捷包装：默认用 BACKLIGHT_FADE_MS 做渐变。 */
void julia_display_theme_on_state(julia_sub_state_t state)
{
    julia_display_theme_on_state_transition(state, BACKLIGHT_FADE_MS);
}

/* 状态迁移：更新当前状态并依据目标子状态决定亮暗策略——
 *   睡眠(S0)      → 进入呼吸（doze）；
 *   已在呼吸/doze → 复位空闲计时（表示要醒了）；
 *   主动/对话     → 复位空闲计时；
 *   其余且全亮    → 按 state_brightness() 渐变。
 * 开机淡入期间不打断较亮动作。 */
void julia_display_theme_on_state_transition(julia_sub_state_t state, uint16_t duration_ms)
{
    s_state = state;
    if (s_power_state != DISPLAY_POWER_FADE_IN) s_boot_brightness_hold = false;
    if (state <= JULIA_SUB_STATE_S0_3_MANUAL_SLEEP) {
        display_breathing_start();
    } else if (display_breathing_active()) {
        reset_idle_timer();
    } else if (state >= JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER &&
               state <= JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE) {
        reset_idle_timer();
    } else if (s_power_state == DISPLAY_POWER_ON) {
        start_fade_for(state_brightness(), (uint32_t)(esp_timer_get_time() / 1000ULL),
                       duration_ms);
    }
}

/* 进入 doze：先通过原子标志防止重入（s_doze_transitioning），带 800ms 防抖忽略刚
 * 切换完的请求；暂停微动、切睡眠立绘、起低亮度呼吸，关闭渲染与呼吸灯进入睡眠态。
 * avatar_face_set_doze 失败则回滚并告警。 */
void display_breathing_start(void)
{
    if (!JULIA_DOZE_BREATH_ENABLE || !s_setter || s_power_state == DISPLAY_POWER_OFF ||
        s_power_state == DISPLAY_POWER_FADE_OUT ||
        s_state < JULIA_SUB_STATE_S1_1_NEAR_STANDBY ||
        s_state > JULIA_SUB_STATE_S1_3_CHARGING_STANDBY ||
        __atomic_exchange_n(&s_doze_transitioning, true, __ATOMIC_ACQ_REL)) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s_doze_transition_finished_ms < DOZE_TRANSITION_DEBOUNCE_MS) {
        __atomic_store_n(&s_doze_transitioning, false, __ATOMIC_RELEASE);
        return;
    }
    s_restore_brightness = state_brightness();
    avatar_motion_pause_all();
    int64_t image_started_us = esp_timer_get_time();
    esp_err_t image_err = avatar_face_set_doze(true);
    uint32_t image_done_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (image_err != ESP_OK) {
        avatar_motion_resume_all();
        __atomic_store_n(&s_doze_transitioning, false, __ATOMIC_RELEASE);
        ESP_LOGE(TAG, "doze enter aborted frame_sync=%s", esp_err_to_name(image_err));
        return;
    }
    s_doze_asset_loaded = true;
    esp_err_t breathe_err = julia_backlight_breathe_start(
        DOZE_BREATHE_MIN_PERCENT, DOZE_BREATHE_MAX_PERCENT, DOZE_BREATHE_PERIOD_MS);
    s_power_state = DISPLAY_POWER_OFF;
    s_rendering = false;
    breathing_led_set_display_sleep(true, s_state <= JULIA_SUB_STATE_S0_3_MANUAL_SLEEP);
    s_doze_transition_finished_ms = (uint32_t)(esp_timer_get_time() / 1000);
    __atomic_store_n(&s_doze_transitioning, false, __ATOMIC_RELEASE);
#if JULIA_DISPLAY_LOG
    ESP_LOGI(TAG, "doze enter image_done_ms=%lu image_elapsed_ms=%.1f fade_start_ms=%lu breathe=%s min=%u max=%u period_ms=%u zero_hold=%u%%",
             (unsigned long)image_done_ms,
             (double)(esp_timer_get_time() - image_started_us) / 1000.0,
             (unsigned long)s_doze_transition_finished_ms, esp_err_to_name(breathe_err),
             DOZE_BREATHE_MIN_PERCENT, DOZE_BREATHE_MAX_PERCENT,
             DOZE_BREATHE_PERIOD_MS, 15U);
#endif
}

/* 停止呼吸 = 复位空闲计时（等价于一次"唤醒"）。 */
void display_breathing_stop(void)
{
    reset_idle_timer();
}

/* 是否处于 doze 相关状态（含切换进行中、退出中、等待首帧）。 */
bool display_breathing_active(void)
{
    return __atomic_load_n(&s_doze_transitioning, __ATOMIC_ACQUIRE) ||
           s_power_state == DISPLAY_POWER_FADE_OUT ||
           s_power_state == DISPLAY_POWER_OFF ||
           s_power_state == DISPLAY_POWER_WAKE_WAIT_FLUSH;
}

/* 空闲计时复位（"有用户交互/要醒来"）。
 *   - 若在 doze(OFF)：执行唤醒——先淡入到 100% 并等待完成，再切回非睡眠立绘、
 *     恢复微动，回到 ON。用原子标志 + 800ms 防抖避免重复。
 *   - 若在 FADE_OUT（正在降下去）：撤销：记录 flush 基线、恢复渲染、显示立绘、
 *     等首帧刷出后进入 WAKE_WAIT_FLUSH。
 *   - 其余：按 state_brightness() 回升到全亮（FADE_IN）。
 */
void reset_idle_timer(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_interaction = now_ms;
    s_boot_brightness_hold = false;
    if (s_power_state == DISPLAY_POWER_OFF) {
        if (__atomic_exchange_n(&s_doze_transitioning, true, __ATOMIC_ACQ_REL)) return;
        if (now_ms - s_doze_transition_finished_ms < DOZE_TRANSITION_DEBOUNCE_MS) {
            __atomic_store_n(&s_doze_transitioning, false, __ATOMIC_RELEASE);
            return;
        }
        breathing_led_set_display_sleep(false, false);
        uint32_t fade_started_ms = (uint32_t)(esp_timer_get_time() / 1000);
        esp_err_t fade_err = julia_backlight_fade_to(100, DOZE_WAKE_FADE_MS);
        esp_err_t wait_err = fade_err == ESP_OK
                                 ? julia_backlight_wait_fade(DOZE_WAKE_FADE_MS + 300U)
                                 : fade_err;
        uint32_t fade_done_ms = (uint32_t)(esp_timer_get_time() / 1000);
        esp_err_t image_err = wait_err == ESP_OK ? avatar_face_set_doze(false) : wait_err;
        avatar_motion_resume_all();
        s_power_state = DISPLAY_POWER_ON;
        s_rendering = true;
        s_doze_transition_finished_ms = (uint32_t)(esp_timer_get_time() / 1000);
        __atomic_store_n(&s_doze_transitioning, false, __ATOMIC_RELEASE);
#if JULIA_DISPLAY_LOG
        ESP_LOGI(TAG, "doze exit fade_start_ms=%lu fade_done_ms=%lu wait=%s image_done_ms=%lu image_sync=%s",
                 (unsigned long)fade_started_ms, (unsigned long)fade_done_ms,
                 esp_err_to_name(wait_err), (unsigned long)s_doze_transition_finished_ms,
                 esp_err_to_name(image_err));
#endif
        return;
    }
    if (s_power_state == DISPLAY_POWER_FADE_OUT) {
        lvgl_port_get_flush_metrics(&s_wake_flush_baseline, NULL, NULL);
        breathing_led_set_display_sleep(false, false);
        lvgl_port_set_refresh_paused(false);
        avatar_show_all();
        if (lvgl_port_lock(pdMS_TO_TICKS(20))) {
            lv_obj_invalidate(lv_scr_act());
            lvgl_port_unlock();
        }
        s_wake_started_ms = now_ms;
        s_power_state = DISPLAY_POWER_WAKE_WAIT_FLUSH;
        return;
    }
    start_fade_for(state_brightness(), now_ms, WAKE_FADE_MS);
    s_power_state = DISPLAY_POWER_FADE_IN;
}

/* 交互回调：等价于一次"空闲计时复位"。 */
void julia_display_theme_on_interaction(void) { reset_idle_timer(); }

/* 周期驱动（调用方按固定节奏/帧率调用）：根据空闲时长与状态机决定——
 *   OFF → 不动；WAKE_WAIT_FLUSH → 等首帧刷完再回升；FADE_OUT → 推进渐变到完全熄灭；
 *   空闲超时且处于待机/全亮 → 进入 doze；FADE_IN → 推进到全亮；
 *   否则按 state_brightness() 或 dim（空闲过半）决定目标，必要时启动渐变。
 * 夜间把超时折半、调暗亮度更暗。 */
void julia_display_theme_update(uint32_t now_ms)
{
    if (!s_setter) return;
    uint32_t idle = now_ms - s_last_interaction;
    uint32_t timeout = night_time() ? s_idle_timeout / 2U : s_idle_timeout;
    uint8_t dim = night_time() ? SCREEN_NIGHT_DIM_PERCENT : SCREEN_DIM_PERCENT;
    if (s_power_state == DISPLAY_POWER_OFF) return;
    if (s_power_state == DISPLAY_POWER_WAKE_WAIT_FLUSH) {
        uint64_t flush_count = 0;
        lvgl_port_get_flush_metrics(&flush_count, NULL, NULL);
        if (flush_count > s_wake_flush_baseline) {
            start_fade_for(s_restore_brightness, now_ms, WAKE_FADE_MS);
            s_power_state = DISPLAY_POWER_FADE_IN;
            ESP_LOGI(TAG, "t=%lu wake first-flush complete latency_ms=%lu fade_target=%u duty=%u",
                     (unsigned long)now_ms, (unsigned long)(now_ms - s_wake_started_ms),
                     s_restore_brightness, (unsigned)julia_backlight_get_duty());
        }
        return;
    }
    if (s_power_state == DISPLAY_POWER_FADE_OUT) {
        if (update_fade(now_ms)) {
            s_rendering = false;
            s_power_state = DISPLAY_POWER_OFF;
            ESP_LOGI(TAG, "t=%lu doze complete percent=%u duty=%u lvgl_flush=0 asset=%u",
                     (unsigned long)now_ms, s_backlight_current,
                     (unsigned)julia_backlight_get_duty(), s_doze_asset_loaded ? 1U : 0U);
        }
        return;
    }
    if (idle >= timeout && s_state >= JULIA_SUB_STATE_S1_1_NEAR_STANDBY &&
        s_state <= JULIA_SUB_STATE_S1_3_CHARGING_STANDBY &&
        (s_power_state == DISPLAY_POWER_ON ||
                            s_power_state == DISPLAY_POWER_DIM)) {
        display_breathing_start();
        return;
    }
    if (s_power_state == DISPLAY_POWER_FADE_IN) {
        if (update_fade(now_ms)) {
            s_power_state = DISPLAY_POWER_ON;
            s_rendering = true;
            s_boot_brightness_hold = false;
            ESP_LOGI(TAG, "t=%lu wake/full-bright complete percent=%u duty=%u",
                     (unsigned long)now_ms, s_backlight_current,
                     (unsigned)julia_backlight_get_duty());
        }
        return;
    }
    if (s_boot_brightness_hold) return;
    uint8_t desired = idle >= timeout / 2U ? dim : state_brightness();
    if (desired != s_backlight_target) {
        start_fade_for(desired, now_ms, BACKLIGHT_FADE_MS);
        if (desired == dim) {
            s_power_state = DISPLAY_POWER_DIM;
            ESP_LOGI(TAG, "t=%lu dim target=%u duty=%u timeout_ms=%lu night=%u",
                     (unsigned long)now_ms, dim, (unsigned)julia_backlight_get_duty(),
                     (unsigned long)timeout, night_time() ? 1U : 0U);
        }
    }
    if (s_fading) update_fade(now_ms);
}

#ifdef DISPLAY_DEBUG
void display_debug_force_breathing(void) { display_breathing_start(); }
void display_debug_stop_breathing(void) { display_breathing_stop(); }
#endif

void julia_display_theme_set_idle_timeout(uint32_t milliseconds) { s_idle_timeout = milliseconds; }
bool julia_display_theme_rendering(void) { return s_rendering; }
const char *julia_theme_current(void) { return s_themes[s_current].name; }
size_t julia_theme_count(void) { return s_count; }

/* 按名称切换主题；找不到返回 NOT_FOUND。 */
esp_err_t julia_theme_select(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < s_count; ++i) if (!strcmp(name, s_themes[i].name)) {
        s_current = i; apply_current(true); return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* 循环切换到下一个主题。 */
esp_err_t julia_theme_next(void)
{
    s_current = (s_current + 1U) % s_count; apply_current(true); return ESP_OK;
}
