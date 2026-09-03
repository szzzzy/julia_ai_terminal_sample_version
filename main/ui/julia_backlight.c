/**
 * @file    julia_backlight.c
 * @brief   控制用户看到的屏幕亮度，并在待机时生成平滑呼吸效果。
 *
 * 开机时先保持全黑，完整立绘刷新后才点亮，避免用户看到白屏或未初始化像素。
 * 设备状态决定目标亮度或是否呼吸；本模块只负责把变化平滑地输出到背光引脚，
 * 不自行判断用户是否离开或设备是否应该睡眠。
 *
 * 手动设置亮度会先停止呼吸，保证两种控制不会互相争抢。呼吸任务只安排下一段
 * 渐变，实际亮度过渡由硬件完成，因此不会持续占用 CPU。
 *
 * 呼吸曲线预先计算，低亮度变化经过视觉校正，避免人眼看到突跳。最小亮度配置为零时，
 * 每个周期会短暂停留在全黑，降低平均功耗但不改变渐亮和渐暗速度。
 */
#include "julia_backlight.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef JULIA_BACKLIGHT_GPIO
#define JULIA_BACKLIGHT_GPIO 38
#warning "JULIA_BACKLIGHT_GPIO undefined; falling back to GPIO 38"
#endif
#ifndef JULIA_DISPLAY_LOG
#define JULIA_DISPLAY_LOG 0
#endif

#define BL_MODE LEDC_LOW_SPEED_MODE
#define BL_TIMER LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_MAX_DUTY 1023U
#define BREATHE_DEFAULT_SEGMENTS 120U
#define BREATHE_LUT_SEGMENTS 120U
#define BREATHE_MIN_SEGMENT_MS 5U
/* 最小亮度为零时，每个周期有 15% 时间保持全黑，以降低平均功耗。 */
#ifndef BREATHE_ZERO_HOLD_PERCENT
#define BREATHE_ZERO_HOLD_PERCENT 15U
#endif

/* 预先计算的呼吸曲线保存在 Flash，运行时不做浮点运算；视觉校正不会改变配置的端点。 */
static const uint16_t s_sine_q10[BREATHE_LUT_SEGMENTS + 1] = {
    0,1,3,6,11,17,25,34,44,56,69,83,98,114,131,150,169,190,211,233,256,
    279,303,328,353,379,405,431,458,485,511,538,565,592,618,644,670,695,
    720,744,767,790,812,833,854,873,892,909,925,940,954,967,979,989,998,
    1006,1012,1017,1020,1022,1023,1022,1020,1017,1012,1006,998,989,979,
    967,954,940,925,909,892,873,854,833,812,790,767,744,720,695,670,644,
    618,592,565,538,512,485,458,431,405,379,353,328,303,279,256,233,211,
    190,169,150,131,114,98,83,69,56,44,34,25,17,11,6,3,1,0
};
static const uint16_t s_sine_gamma22_q10[BREATHE_LUT_SEGMENTS + 1] = {
    0,0,0,0,0,0,0,1,1,2,3,4,6,8,11,15,20,25,32,39,48,59,71,84,99,115,
    133,153,175,198,223,249,277,307,337,369,403,437,472,507,543,579,616,
    652,687,722,756,789,820,850,878,904,928,950,969,985,999,1009,1017,
    1021,1023,1021,1017,1009,999,985,969,950,928,904,878,850,820,789,756,
    722,687,652,616,579,543,507,472,437,403,369,337,307,277,249,223,198,
    175,153,133,115,99,84,71,59,48,39,32,25,20,15,11,8,6,4,3,2,1,1,0,
    0,0,0,0,0,0
};
static volatile uint8_t s_percent;             /* 当前亮度百分比（尽力一致）。 */
static volatile bool s_breathing;              /* 呼吸模式是否运行。 */
static volatile bool s_gamma_enabled = true;   /* 是否启用 gamma 曲线。 */
static uint8_t s_min_percent, s_max_percent;   /* 呼吸亮度上下限。 */
static uint16_t s_curve_index, s_segments;     /* 当前曲线段/总段数。 */
static uint32_t s_period_ms;                   /* 呼吸周期。 */
static uint32_t s_segment_ms;                  /* 每段时长 = period/segments。 */
static volatile uint32_t s_generation;         /* 参数版本号，避免旧任务用已更新的配置。 */
static volatile TickType_t s_segment_started_tick; /* 当前段的起始 tick（用于 vTaskDelayUntil）。 */
static TaskHandle_t s_breathe_task;
static SemaphoreHandle_t s_fade_done;          /* 硬件 fade 完成信号（irq 给）。 */


/* 百分比 → LEDC 占空比（10-bit），并夹到 [0,100]。 */
static uint32_t duty_for(uint8_t percent)
{
    return BL_MAX_DUTY * (percent > 100U ? 100U : percent) / 100U;
}

/* 计算曲线第 index 段的占空比：
 *   - 最小亮度 0 且启用 zero-hold 时，在首尾各留一段全灭（降功耗）；
 *   - 用 LUT（gamma 或线性）取该段的归一化幅度；
 *   - 按 min/max duty 线性缩放到实际 PWM 占空比（避免端点被 gamma 改变）。
 */
static uint32_t curve_duty(uint16_t index)
{
    if (s_min_percent == 0U && BREATHE_ZERO_HOLD_PERCENT > 0U) {
        uint16_t hold = (uint16_t)(((uint32_t)s_segments * BREATHE_ZERO_HOLD_PERCENT) / 100U);
        if (index <= hold || index >= (uint16_t)(s_segments - hold)) return 0;
    }
    uint16_t lut_index = (uint16_t)(((uint32_t)index * BREATHE_LUT_SEGMENTS +
                                     s_segments / 2U) / s_segments);
    if (lut_index > BREATHE_LUT_SEGMENTS) lut_index = BREATHE_LUT_SEGMENTS;
    const uint16_t *lut = s_gamma_enabled ? s_sine_gamma22_q10 : s_sine_q10;
    uint32_t minimum = duty_for(s_min_percent);
    uint32_t maximum = duty_for(s_max_percent);
    return minimum + ((maximum - minimum) * lut[lut_index] + 511U) / 1023U;
}

/* LEDC fade 完成 ISR（IRAM）：唤醒 breathe_task，让它推进到下一段。
 * 返回是否需要上下文切换。 */
static bool IRAM_ATTR fade_done(const ledc_cb_param_t *param, void *arg)
{
    (void)param;
    (void)arg;
    BaseType_t wake = pdFALSE;
    if (s_breathe_task) vTaskNotifyGiveFromISR(s_breathe_task, &wake);
    return wake == pdTRUE;
}

/* 启动某一段：让 LEDC 硬件在 s_segment_ms 内渐到该段目标占空比（不等待）。 */
static void start_segment(uint16_t index)
{
    ledc_set_fade_with_time(BL_MODE, BL_CHANNEL, curve_duty(index), s_segment_ms);
    ledc_fade_start(BL_MODE, BL_CHANNEL, LEDC_FADE_NO_WAIT);
}

/* 呼吸任务：被 IRAM fade_done ISR 唤醒后推进到下一段。若收到新版参数
 * （generation 变化）则重置相位；否则按段边界对齐，发新一轮硬件 fade。
 * 说明：当相邻 gamma 样本量化到相同 duty 时 LEDC 会立即上报完成，这里不逐点做软件
 * PWM，而是等段边界再启动下一段，保持相位均匀。 */
static void breathe_task(void *arg)
{
    (void)arg;
    uint32_t generation = 0;
    TickType_t segment_deadline = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_breathing) {
            if (s_fade_done) xSemaphoreGive(s_fade_done);
            continue;
        }
        uint32_t current_generation = s_generation;
        if (current_generation != generation) {
            generation = current_generation;
            segment_deadline = s_segment_started_tick;
        }
        /* LEDC reports an immediate completion when adjacent gamma samples
         * quantize to the same duty. Keep phase time uniform without doing
         * software PWM updates; the dedicated task only waits for the next
         * segment boundary, then launches another hardware fade. */
        vTaskDelayUntil(&segment_deadline, pdMS_TO_TICKS(s_segment_ms));
        if (!s_breathing || generation != s_generation) continue;
        uint32_t duty = curve_duty(s_curve_index);
        s_percent = (uint8_t)((duty * 100U + BL_MAX_DUTY / 2U) / BL_MAX_DUTY);
#if JULIA_DISPLAY_LOG
        uint16_t log_interval = s_segments / 12U;
        if (!log_interval) log_interval = 1U;
        if (s_curve_index % log_interval == 0U)
            ESP_LOGI("BACKLIGHT", "breathe sample percent=%u duty=%lu segment=%u/%u gamma=%u",
                     s_percent, (unsigned long)duty, s_curve_index, s_segments,
                     s_gamma_enabled ? 1U : 0U);
#endif
        s_curve_index = (s_curve_index + 1U) % (s_segments + 1U);
        /* Index zero is the duplicated cycle endpoint; proceed to one. */
        if (s_curve_index == 0U) s_curve_index = 1U;
        if (s_breathing) start_segment(s_curve_index);
    }
}

/* 初始化背光：先把 GPIO 拉低（保证开机背光灭），配置 GPIO/LEDC 定时器与通道，
 * 安装 fade 中断并注册捕获回调，创建呼吸任务。注意 init 时 duty 为 0，因此直到
 * 调用方显式点亮（如首帧渲染完成后 julia_backlight_set/fade_to）背光都保持关闭。 */
esp_err_t julia_backlight_init(void)
{
    gpio_set_level(JULIA_BACKLIGHT_GPIO, 0);
    gpio_config_t gpio = {.pin_bit_mask = 1ULL << JULIA_BACKLIGHT_GPIO, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&gpio), "BACKLIGHT", "gpio");
    ledc_timer_config_t timer = {.speed_mode = BL_MODE, .timer_num = BL_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK};
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), "BACKLIGHT", "timer");
    ledc_channel_config_t channel = {.gpio_num = JULIA_BACKLIGHT_GPIO, .speed_mode = BL_MODE,
        .channel = BL_CHANNEL, .intr_type = LEDC_INTR_DISABLE, .timer_sel = BL_TIMER,
        .duty = 0, .hpoint = 0};
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), "BACKLIGHT", "channel");
    ESP_RETURN_ON_ERROR(ledc_fade_func_install(0), "BACKLIGHT", "fade install");
    ledc_cbs_t callbacks = {.fade_cb = fade_done};
    ESP_RETURN_ON_ERROR(ledc_cb_register(BL_MODE, BL_CHANNEL, &callbacks, NULL),
                        "BACKLIGHT", "callback");
    s_fade_done = xSemaphoreCreateBinary();
    if (!s_fade_done || xTaskCreateWithCaps(breathe_task, "bl_breathe", 4096, NULL, 4,
                                            &s_breathe_task,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI("BACKLIGHT", "LEDC gpio=%d freq=20000Hz bits=10", JULIA_BACKLIGHT_GPIO);
    return ESP_OK;
}

/* 停止呼吸：置位 s_breathing=false 并停止 LEDC fade。 */
void julia_backlight_breathe_stop(void)
{
    s_breathing = false;
    ledc_fade_stop(BL_MODE, BL_CHANNEL);
}

/* 立即设置亮度（无渐变）。先停呼吸，避免二者抢占同一通道。 */
void julia_backlight_set(uint8_t percent)
{
    julia_backlight_breathe_stop();
    if (percent > 100U) percent = 100U;
    s_percent = percent;
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty_for(percent));
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

/* 渐变到目标亮度（duration_ms 内）。先停呼吸并清掉旧完成信号，再发起硬件 fade。 */
esp_err_t julia_backlight_fade_to(uint8_t percent, uint32_t duration_ms)
{
    julia_backlight_breathe_stop();
    if (percent > 100U) percent = 100U;
    if (s_fade_done) xSemaphoreTake(s_fade_done, 0);
    ESP_RETURN_ON_ERROR(ledc_set_fade_with_time(BL_MODE, BL_CHANNEL,
                                                duty_for(percent), duration_ms),
                        "BACKLIGHT", "fade");
    s_percent = percent;
    return ledc_fade_start(BL_MODE, BL_CHANNEL, LEDC_FADE_NO_WAIT);
}

/* 等待最近一次 fade 完成（由 ISR 给 s_fade_done）。 */
esp_err_t julia_backlight_wait_fade(uint32_t timeout_ms)
{
    if (!s_fade_done) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(s_fade_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* 以默认段数启动呼吸。 */
esp_err_t julia_backlight_breathe_start(uint8_t min_percent, uint8_t max_percent,
                                        uint32_t period_ms)
{
    return julia_backlight_breathe_start_ex(min_percent, max_percent, period_ms,
                                             BREATHE_DEFAULT_SEGMENTS);
}

/* 以指定段数启动呼吸。参数非法（min>=max/max>100/段数越界/每段过短）返回 INVALID_ARG。
 * 启动时把通道设为 min 亮度并从第 1 段开始；每次配置都会递增 s_generation，
 * 让任务丢弃旧配置的剩余段。 */
esp_err_t julia_backlight_breathe_start_ex(uint8_t min_percent, uint8_t max_percent,
                                           uint32_t period_ms, uint16_t segments)
{
    if (min_percent >= max_percent || max_percent > 100U || !segments ||
        segments > BREATHE_LUT_SEGMENTS || period_ms / segments < BREATHE_MIN_SEGMENT_MS)
        return ESP_ERR_INVALID_ARG;
    julia_backlight_breathe_stop();
    s_min_percent = min_percent;
    s_max_percent = max_percent;
    s_period_ms = period_ms;
    s_segments = segments;
    s_segment_ms = period_ms / segments;
    s_curve_index = 1U;
    ++s_generation;
    s_breathing = true;
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty_for(min_percent));
    ledc_update_duty(BL_MODE, BL_CHANNEL);
    s_percent = min_percent;
    s_segment_started_tick = xTaskGetTickCount();
    start_segment(s_curve_index);
    ESP_LOGI("BACKLIGHT", "breathe min=%u max=%u period_ms=%lu segments=%u segment_ms=%lu gamma=%u",
             min_percent, max_percent, (unsigned long)period_ms,
             segments, (unsigned long)s_segment_ms, s_gamma_enabled ? 1U : 0U);
    return ESP_OK;
}

/* 切换 gamma。若正在呼吸，用当前参数重启以立即生效。 */
esp_err_t julia_backlight_set_gamma(bool enabled)
{
    s_gamma_enabled = enabled;
    if (!s_breathing) return ESP_OK;
    return julia_backlight_breathe_start_ex(s_min_percent, s_max_percent,
                                             s_period_ms, s_segments);
}

bool julia_backlight_gamma_enabled(void) { return s_gamma_enabled; }

bool julia_backlight_breathing(void) { return s_breathing; }
uint8_t julia_backlight_get_percent(void) { return s_percent; }
uint32_t julia_backlight_get_duty(void) { return ledc_get_duty(BL_MODE, BL_CHANNEL); }
int julia_backlight_get_gpio_level(void) { return gpio_get_level(JULIA_BACKLIGHT_GPIO); }
/* 强制熄灭：停呼吸、停 LEDC、拉低 GPIO、清百分比。 */
void julia_backlight_force_off(void)
{
    julia_backlight_breathe_stop();
    ledc_stop(BL_MODE, BL_CHANNEL, 0);
    gpio_set_level(JULIA_BACKLIGHT_GPIO, 0);
    s_percent = 0;
}
