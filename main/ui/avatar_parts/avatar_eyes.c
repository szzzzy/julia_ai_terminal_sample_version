#include "avatar_eyes.h"

#include "avatar_chroma_assets.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "lvgl_port.h"

#ifndef JULIA_AVATAR_LOG
#define JULIA_AVATAR_LOG 1
#endif

static lv_obj_t *s_left;
static lv_obj_t *s_right;
static volatile uint8_t s_main_state = 1;
static volatile bool s_transition_active;
static volatile uint32_t s_generation;

#define PUPIL_GREEN_RGB565 0x2645U

static bool in_eye_region(unsigned x, unsigned y)
{
    return x >= 45U && x < 315U && y >= 55U && y < 235U;
}

void avatar_eyes_correct_pupils_rgb565(uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (!pixels || width != 360 || height != 360) return;
    for (unsigned y = 55; y < 235; ++y) {
        for (unsigned x = 45; x < 315; ++x) {
            if (!in_eye_region(x, y)) continue;
            uint16_t value = pixels[y * width + x];
            unsigned red = (value >> 11) & 0x1fU;
            unsigned green = (value >> 5) & 0x3fU;
            unsigned blue = value & 0x1fU;
            unsigned red6 = red * 2U;
            unsigned blue6 = blue * 2U;
            if (green >= 18U && green * 4U > red6 * 5U &&
                green * 4U > blue6 * 5U) {
                unsigned gold_red = 18U + green / 5U;
                unsigned gold_green = 16U + green / 2U;
                unsigned gold_blue = 2U + green / 16U;
                if (gold_red > 31U) gold_red = 31U;
                if (gold_green > 63U) gold_green = 63U;
                if (gold_blue > 31U) gold_blue = 31U;
                pixels[y * width + x] = (uint16_t)((gold_red << 11) |
                                                   (gold_green << 5) | gold_blue);
            }
        }
    }
}

bool avatar_eyes_has_green_blob_rgb565(const uint16_t *pixels, uint16_t width,
                                       uint16_t height)
{
    if (!pixels || width != 360 || height != 360) return true;
    unsigned outside = 0;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            if (!in_eye_region(x, y) && pixels[y * width + x] == PUPIL_GREEN_RGB565 &&
                ++outside > 256U) return true;
    return false;
}

static const lv_img_dsc_t *left_source(avatar_eyes_frame_t frame)
{
    static const lv_img_dsc_t *sources[] = {
        &avatar_asset_eye_left_open,
        &avatar_asset_eye_left_half,
        &avatar_asset_eye_left_closed,
    };
    return sources[frame <= AVATAR_EYES_CLOSED ? frame : AVATAR_EYES_OPEN];
}

static const lv_img_dsc_t *right_source(avatar_eyes_frame_t frame)
{
    static const lv_img_dsc_t *sources[] = {
        &avatar_asset_eye_right_open,
        &avatar_asset_eye_right_half,
        &avatar_asset_eye_right_closed,
    };
    return sources[frame <= AVATAR_EYES_CLOSED ? frame : AVATAR_EYES_OPEN];
}

void avatar_eyes_show(avatar_eyes_frame_t frame)
{
    if (!s_left || !s_right || !lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    int64_t started = esp_timer_get_time();
    lv_img_set_src(s_left, left_source(frame));
    lv_img_set_src(s_right, right_source(frame));
    lvgl_port_unlock();
    int64_t elapsed = esp_timer_get_time() - started;
    if (elapsed > 12000)
        ESP_LOGW("JULIA_AVATAR", "eye refresh slow frame=%u elapsed_us=%lld", frame, elapsed);
}

static void blink_task(void *argument)
{
    (void)argument;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(3000U + esp_random() % 5001U));
        if (s_transition_active || (s_main_state != 1 && s_main_state != 3)) continue;
        uint32_t generation = s_generation;
        int64_t started = esp_timer_get_time();
#if JULIA_AVATAR_LOG
        ESP_LOGI("JULIA_AVATAR", "blink trigger state=S%u", s_main_state);
#endif
        avatar_eyes_show(AVATAR_EYES_CLOSED);
        vTaskDelay(pdMS_TO_TICKS(90));
        if (generation != s_generation || s_transition_active) continue;
        avatar_eyes_show(AVATAR_EYES_OPEN);
        vTaskDelay(pdMS_TO_TICKS(90));
#if JULIA_AVATAR_LOG
        ESP_LOGI("JULIA_AVATAR", "blink complete duration_ms=%lld",
                 (esp_timer_get_time() - started) / 1000);
#endif
    }
}

void avatar_eyes_init(lv_obj_t *parent)
{
    if (!parent || s_left || s_right) return;
    s_left = lv_img_create(parent);
    s_right = lv_img_create(parent);
    lv_img_set_src(s_left, &avatar_asset_eye_left_open);
    lv_img_set_src(s_right, &avatar_asset_eye_right_open);
    lv_obj_set_pos(s_left, 112, 108);
    lv_obj_set_pos(s_right, 194, 108);
    lv_obj_clear_flag(s_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_right, LV_OBJ_FLAG_SCROLLABLE);
    if (xTaskCreateWithCaps(blink_task, "avatar_eyes", 3072, NULL, 2, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE("JULIA_AVATAR", "failed to create PSRAM blink task");
}

void avatar_eyes_set_state(uint8_t main_state)
{
    s_main_state = main_state;
    ++s_generation;
    if (!s_transition_active) avatar_eyes_show(AVATAR_EYES_OPEN);
}

void avatar_eyes_set_transition_active(bool active)
{
    s_transition_active = active;
    ++s_generation;
    avatar_eyes_set_visible(!active);
    if (!active) avatar_eyes_show(AVATAR_EYES_OPEN);
}

void avatar_eyes_set_visible(bool visible)
{
    if (!s_left || !s_right || !lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    if (visible) {
        lv_obj_clear_flag(s_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_right, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_right, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

lv_obj_t *avatar_eyes_left_object(void) { return s_left; }
lv_obj_t *avatar_eyes_right_object(void) { return s_right; }
