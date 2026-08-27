#include "breathing_led.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "julia_led.h"

typedef struct {
    uint8_t lo;
    uint8_t hi;
    uint16_t period_ms;
    uint32_t color;
    bool solid;
} led_profile_t;

static const led_profile_t s_profiles[LED_STATE_COUNT] = {
    [LED_S0_OFF]       = {0, 0, 0, 0x000000, true},
    [LED_S1_DIM_WARM]  = {5, 15, 4000, 0xFFF1D6, false},
    [LED_S2_SOFT_WARM] = {18, 22, 4000, 0xFFD27A, false},
    [LED_S3_ALERT]     = {35, 50, 700, 0xFF8A35, false},
    [LED_S4_EMOTION]   = {70, 70, 0, 0xFFF1D6, true},
    [LED_S5_FADE_COLD] = {5, 10, 6000, 0x69849E, false},
};

static led_profile_t s_current;
static led_profile_t s_from;
static led_profile_t s_target;
static uint32_t s_started_ms;
static uint16_t s_duration_ms;
static uint32_t s_emotion_color = 0xFFF1D6;
static bool s_transitioning;
static bool s_display_sleep;
static bool s_deep_sleep;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "STATE_LED";

static uint8_t lerp_u8(uint8_t from, uint8_t to, uint32_t q10)
{
    return (uint8_t)((int32_t)from + ((int32_t)to - from) * (int32_t)q10 / 1024);
}

static uint32_t lerp_color(uint32_t from, uint32_t to, uint32_t q10)
{
    uint8_t r = lerp_u8((from >> 16) & 0xff, (to >> 16) & 0xff, q10);
    uint8_t g = lerp_u8((from >> 8) & 0xff, (to >> 8) & 0xff, q10);
    uint8_t b = lerp_u8(from & 0xff, to & 0xff, q10);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

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

void led_set_emotion_color(uint32_t rgb)
{
    s_emotion_color = rgb & 0xffffffU;
}

bool breathing_led_transition_active(void) { return s_transitioning; }

void breathing_led_set_display_sleep(bool sleeping, bool deep_sleep)
{
    taskENTER_CRITICAL(&s_lock);
    s_display_sleep = sleeping;
    s_deep_sleep = deep_sleep;
    led_profile_t current = s_current;
    taskEXIT_CRITICAL(&s_lock);
    apply_profile(&current);
}
