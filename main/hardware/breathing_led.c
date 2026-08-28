/**
 * @file    breathing_led.c
 * @brief   LED 状态机：把“当前 LED 状态”映射到亮度/颜色轮廓，并做状态间平滑过渡。
 *
 * 职责边界（与 julia_led.c 的分工）：
 * - 本模块是“上层状态机/策略层”：定义了一组按 LED 状态（led_state_t）索引的
 *   led_profile_t 轮廓（off/柔焦暖光/提示/情感色/冷光渐隐等），并在状态切换时
 *   在 从→到 两个轮廓之间做时间插值，最终调用 julia_led_set_* 输出。
 * - julia_led.c 是“原语层”：只负责把最终亮度/颜色点亮到 WS2812，不关心状态语义。
 *
 * 数据流：UI 状态机（julia_ui.c 的 led_transition_to / led_set_state）
 *   → 本模块设置 s_from/s_target/s_started_ms 并进入过渡
 *   → 外部定时循环调用 breathing_led_update(now) 
 *   → 按缓动曲线插值出当前帧 → apply_profile() → julia_led_set_* → WS2812。
 * 因此本模块依赖一个“节拍器”：外层需要周期（如每帧 80ms 左右）调用
 * breathing_led_update()。目前工程内未见该调用点，若未连接节拍器则只会在
 * 第一次 apply_profile 时静止输出一个轮廓。
 *
 * 与显示的关系：breathing_led_set_display_sleep(bool, bool) 用于“屏幕睡眠时压暗灯”、
 * “深度睡眠时切到固定冷色”，由 julia_display_theme.c 在 UI 状态变化时调用。
 */

#include "breathing_led.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "julia_led.h"

/**
 * @brief 单个 LED 轮廓：一组决定“灯该是什么样”参数的打包。
 *
 *   solid == true  → 用 julia_led_set_solid(hi, color)；否则用 julia_led_set_breathing(lo, hi, period, color)。
 */
typedef struct {
    uint8_t lo;          /* 呼吸亮度下限（0~100）。 */
    uint8_t hi;          /* 呼吸亮度上限 / 常亮亮度（0~100）。 */
    uint16_t period_ms;  /* 呼吸周期（ms）；0 表示非常亮。 */
    uint32_t color;      /* 颜色 0xRRGGBB。 */
    bool solid;          /* true：常亮（不呼吸）；false：呼吸。 */
} led_profile_t;

/** 各 LED 状态的默认轮廓表：按 led_state_t 索引。注意 S4 情感态 hi=70 且 solid=true。 */
static const led_profile_t s_profiles[LED_STATE_COUNT] = {
    [LED_S0_OFF]       = {0, 0, 0, 0x000000, true},
    [LED_S1_DIM_WARM]  = {5, 15, 4000, 0xFFF1D6, false},
    [LED_S2_SOFT_WARM] = {18, 22, 4000, 0xFFD27A, false},
    [LED_S3_ALERT]     = {35, 50, 700, 0xFF8A35, false},
    [LED_S4_EMOTION]   = {70, 70, 0, 0xFFF1D6, true},
    [LED_S5_FADE_COLD] = {5, 10, 6000, 0x69849E, false},
};

/* 当前/过渡源/目标轮廓，以及过渡计时。均受 s_lock 保护。 */
static led_profile_t s_current;       /* 当前生效的轮廓（过渡起始帧）。 */
static led_profile_t s_from;          /* 过渡起点轮廓。 */
static led_profile_t s_target;        /* 过渡终点轮廓。 */
static uint32_t s_started_ms;         /* 过渡开始时刻（esp_timer 毫秒）。 */
static uint16_t s_duration_ms;        /* 过渡时长（ms，至少 1）。 */
static uint32_t s_emotion_color = 0xFFF1D6; /* S4 情感态覆盖色（默认暖白）。 */
static bool s_transitioning;          /* 是否正处在过渡中。 */
static bool s_display_sleep;          /* 屏幕是否处于“睡眠压暗”模式。 */
static bool s_deep_sleep;             /* 是否深度睡眠（睡眠模式下用固定冷色）。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "STATE_LED";

/**
 * @brief 单个通道的线性插值（用 10 位分数 q10 表示进度）。
 * @param[in] from 起点值。
 * @param[in] to   终点值。
 * @param[in] q10  0~1024 的过渡分数（1024 表示完全到达终点）。
 * @return 插值结果。
 */
static uint8_t lerp_u8(uint8_t from, uint8_t to, uint32_t q10)
{
    return (uint8_t)((int32_t)from + ((int32_t)to - from) * (int32_t)q10 / 1024);
}

/** 对 RGB 三通道分别 lerp，得到过渡颜色。 */
static uint32_t lerp_color(uint32_t from, uint32_t to, uint32_t q10)
{
    uint8_t r = lerp_u8((from >> 16) & 0xff, (to >> 16) & 0xff, q10);
    uint8_t g = lerp_u8((from >> 8) & 0xff, (to >> 8) & 0xff, q10);
    uint8_t b = lerp_u8(from & 0xff, to & 0xff, q10);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/**
 * @brief 把“目标轮廓”落到 LED 上（唯一输出点）。
 *
 * 先按屏幕睡眠状态对轮廓做修正：
 *   - 深度睡眠：丢弃原轮廓，直接用固定冷色 {10,15,6000, 0x4477AA}；
 *   - 普通睡眠：把呼吸上/下限压暗到 30%（×30/100）。
 * 然后按 lo/hi/solid 分派到 julia_led 的三条原语：
 *   hi==0 → 关闭；solid → 常亮；否则 → 呼吸。
 * 注意 hi==0 时优先走“关闭”，避免出现“呼吸到 0 亮度但 period 异常”的情况。
 */
static void apply_profile(const led_profile_t *profile)
{
    led_profile_t output = *profile;
    if (s_display_sleep) {
        if (s_deep_sleep) {
            output = (led_profile_t){10, 15, 6000, 0x4477AA, false};
        } else {
            output.lo = (uint8_t)((output.lo * 30U + 99U) / 100U);
            output.hi = (uint8_t)((output.hi * 30U + 99U) / 100U);
        }
    }
    if (output.hi == 0) julia_led_set_off();
    else if (output.solid) julia_led_set_solid(output.hi, output.color);
    else julia_led_set_breathing(output.lo, output.hi, output.period_ms, output.color);
}

/**
 * @brief 过渡节拍器：每个刷新周期调用一次，推进从 s_from→s_target 的插值并输出。
 *
 * @param[in] now 当前时刻（毫秒）；调用方应使用与 s_started_ms 相同的时间基准
 *                （本模块用 esp_timer_get_time()/1000 记录 s_started_ms）。
 *
 * 行为：
 *  - 无进行中过渡直接返回；否则用 q10 = min(1, elapsed/duration) 作为进度，
 *    再做 ease-out（1-(1-q)^2）缓动，让开头快、结尾慢。
 *  - 用 lerp 得到当前帧的 lo/hi/color（solid 恒为 true，过渡期间按常亮处理），
 *    调用 apply_profile()。
 *  - 过渡结束时应用一次精确的 target 轮廓，并更新 s_current、清除 s_transitioning。
 * 函数可重入且可从任意任务上下文调用（内部用 spinlock 快照，外部再插值）。
 */
void breathing_led_update(uint32_t now)
{
    led_profile_t from, target;
    uint32_t started;
    uint16_t duration;
    bool active;
    taskENTER_CRITICAL(&s_lock);
    from = s_from; target = s_target; started = s_started_ms;
    duration = s_duration_ms; active = s_transitioning;
    taskEXIT_CRITICAL(&s_lock);
    if (!active) return;
    uint32_t elapsed = now - started;
    uint32_t q10 = elapsed >= duration ? 1024U : elapsed * 1024U / duration;
    q10 = 1024U - (1024U - q10) * (1024U - q10) / 1024U;
    led_profile_t frame = target;
    frame.lo = lerp_u8(from.lo, target.lo, q10);
    frame.hi = lerp_u8(from.hi, target.hi, q10);
    frame.color = lerp_color(from.color, target.color, q10);
    frame.solid = true;
    apply_profile(&frame);
    if (elapsed >= duration) {
        apply_profile(&target);
        taskENTER_CRITICAL(&s_lock);
        s_current = target; s_transitioning = false;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "transition complete brightness=%u color=%06lx period=%u",
                 target.hi, (unsigned long)target.color, target.period_ms);
    }
}

/**
 * @brief 立即切换到某一 LED 状态（无过渡动画），直接输出该状态的轮廓。
 *
 * 若传入 LED_S4_EMOTION，会以 s_emotion_color 作为颜色（情感态颜色可被
 * led_set_emotion_color() 动态覆盖）。整体在一把 spinlock 里更新 s_current、
 * 清除过渡标志，随后的 apply_profile() 需要处于任务上下文（内部会调用会阻塞的
 * julia_led_set_*）。
 *
 * @param[in] state 目标 led_state_t，越界直接忽略。
 */
void led_set_state(led_state_t state)
{
    if (state >= LED_STATE_COUNT) return;
    led_profile_t profile = s_profiles[state];
    if (state == LED_S4_EMOTION) profile.color = s_emotion_color;
    taskENTER_CRITICAL(&s_lock);
    s_current = profile; s_transitioning = false;
    taskEXIT_CRITICAL(&s_lock);
    apply_profile(&profile);
}

/**
 * @brief 启动一次状态过渡（从当前轮廓平滑变到 target 轮廓）。
 *
 * 记录 s_from=当前轮廓、s_target=目标轮廓、s_started_ms（经 esp_timer，单位 ms）
 * 和时长；若传入 0 时长则按 1ms 处理，避免除零。真正输出仍由外层周期性调用
 * breathing_led_update() 推进。只有更新了过渡三元组，不会立刻改灯。
 *
 * @param[in] target      目标 led_state_t，越界忽略。
 * @param[in] duration_ms 过渡时长（ms）。
 */
void led_transition_to(led_state_t target, uint16_t duration_ms)
{
    if (target >= LED_STATE_COUNT) return;
    led_profile_t profile = s_profiles[target];
    if (target == LED_S4_EMOTION) profile.color = s_emotion_color;
    taskENTER_CRITICAL(&s_lock);
    s_from = s_current; s_target = profile;
    s_started_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_duration_ms = duration_ms ? duration_ms : 1U;
    s_transitioning = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "transition target=%u duration=%u brightness=%u color=%06lx",
             target, duration_ms, profile.hi, (unsigned long)profile.color);
}

/** 设置情感态颜色（仅影响 LED_S4_EMOTION 状态的颜色）。取 RGB 低 24 位。 */
void led_set_emotion_color(uint32_t rgb)
{
    s_emotion_color = rgb & 0xffffffU;
}

/** 查询是否正处于过渡中（供 UI 判断是否已经稳定）。 */
bool breathing_led_transition_active(void) { return s_transitioning; }

/**
 * @brief 设置“显示睡眠”模式并立刻按新模式重刷当前轮廓。
 *
 * @param[in] sleeping   true → 进入压暗模式；false → 恢复。
 * @param[in] deep_sleep true → 深度睡眠（固定冷色）；仅在 sleeping=true 时有意义。
 * 先更新标志，再取当前轮廓按新标志重放 apply_profile()，让灯立即可感知。
 */
void breathing_led_set_display_sleep(bool sleeping, bool deep_sleep)
{
    taskENTER_CRITICAL(&s_lock);
    s_display_sleep = sleeping;
    s_deep_sleep = deep_sleep;
    led_profile_t current = s_current;
    taskEXIT_CRITICAL(&s_lock);
    apply_profile(&current);
}
