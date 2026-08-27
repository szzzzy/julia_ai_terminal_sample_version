#include "avatar_face.h"

#include "avatar_eyes.h"
#include "avatar_mouth.h"
#include "avatar_face_base.h"
#include "avatar_face_doze.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "julia_ui.h"
#include "lvgl_port.h"

static volatile int64_t s_last_activity_us;
static volatile bool s_demo_allowed = true;
static volatile bool s_demo_active;
static volatile bool s_button_pressed;
static volatile int64_t s_button_started_us;
static lv_obj_t *s_base;
static volatile bool s_dozing;
static volatile uint8_t s_main_state = 1;

static void simulation_task(void *argument)
{
    (void)argument;
    static const julia_sub_state_t demo_states[] = {
        JULIA_SUB_STATE_S1_1_NEAR_STANDBY,
        JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER,
        JULIA_SUB_STATE_S4_1_LIGHT_DIALOG,
    };
    unsigned demo_step = 0;
    unsigned mouth_tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();
        bool long_press = s_button_pressed && now - s_button_started_us >= 600000;
        if (long_press) {
            static const uint16_t levels[] = {0, 30, 80};
            avatar_mouth_set_rms(levels[esp_random() % 3U]);
            continue;
        }
        if (!s_demo_active && s_demo_allowed && now - s_last_activity_us >= 30000000LL)
            s_demo_active = true;
        if (!s_demo_active) continue;
        static const uint16_t demo_rms[] = {0, 30, 80, 95, 30, 0};
        avatar_mouth_set_rms(demo_rms[mouth_tick++ % 6U]);
        if ((mouth_tick % 50U) == 0U)
            julia_ui_set_state(demo_states[demo_step++ % 3U]);
    }
}

void avatar_face_init(lv_obj_t *parent)
{
    s_base = lv_img_create(parent);
    lv_img_set_src(s_base, &avatar_asset_julia_s1_1_near_standby);
    lv_obj_set_pos(s_base, 0, 0);
    lv_obj_clear_flag(s_base, LV_OBJ_FLAG_SCROLLABLE);
    avatar_eyes_init(parent);
    avatar_mouth_init(parent);
    s_last_activity_us = esp_timer_get_time();
    if (xTaskCreateWithCaps(simulation_task, "avatar_demo", 3072, NULL, 2, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE("JULIA_AVATAR", "failed to create PSRAM demo task");
}

void avatar_face_set_state(uint8_t main_state)
{
    s_main_state = main_state;
    avatar_eyes_set_state(main_state);
    if (s_base && !s_dozing) {
        lv_obj_clear_flag(s_base, LV_OBJ_FLAG_HIDDEN);
    }
}

void avatar_face_set_transition_active(bool active)
{
    avatar_eyes_set_transition_active(active);
    avatar_mouth_set_transition_active(active);
    if (s_base) {
        if (active) lv_obj_add_flag(s_base, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_base, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI("JULIA_AVATAR", "transition layers=%s", active ? "hidden" : "restored");
}

esp_err_t avatar_face_set_doze(bool active)
{
    if (!s_base || !lvgl_port_lock(pdMS_TO_TICKS(250))) return ESP_ERR_TIMEOUT;
    s_dozing = active;
    lv_img_set_src(s_base, active ? &avatar_asset_julia_s0_1_night_sleep
                                  : &avatar_asset_julia_s1_1_near_standby);
    lv_obj_clear_flag(s_base, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *layers[] = {avatar_eyes_left_object(), avatar_eyes_right_object(),
                          avatar_mouth_object()};
    for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); ++i) {
        if (!layers[i]) continue;
        if (active) lv_obj_add_flag(layers[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(layers[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(layers[i]);
    }
    lv_obj_invalidate(s_base);
    int64_t started_us = esp_timer_get_time();
    esp_err_t refresh_err = lvgl_port_refr_now_sync(pdMS_TO_TICKS(1000));
    lvgl_port_unlock();
    ESP_LOGI("JULIA_AVATAR", "doze image=%s bytes=259200 cf=RGB565 frame_sync=%s elapsed_ms=%.1f",
             active ? "rest" : "standby", esp_err_to_name(refresh_err),
             (double)(esp_timer_get_time() - started_us) / 1000.0);
    return refresh_err;
}

bool avatar_face_is_dozing(void) { return s_dozing; }

void avatar_face_set_rms(uint16_t rms) { avatar_mouth_set_rms(rms); }

void avatar_face_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    s_demo_active = false;
}

void avatar_face_demo_set_enabled(bool enabled)
{
    s_demo_allowed = enabled;
    s_demo_active = enabled;
    if (!enabled) avatar_mouth_set_rms(0);
}

bool avatar_face_demo_enabled(void) { return s_demo_active; }

void avatar_face_button_set_pressed(bool pressed)
{
    s_button_pressed = pressed;
    if (pressed) {
        s_button_started_us = esp_timer_get_time();
        avatar_face_note_activity();
    } else {
        avatar_mouth_set_rms(0);
    }
}

lv_obj_t *avatar_face_left_eye(void) { return avatar_eyes_left_object(); }
lv_obj_t *avatar_face_right_eye(void) { return avatar_eyes_right_object(); }
lv_obj_t *avatar_face_mouth(void) { return avatar_mouth_object(); }
lv_obj_t *avatar_face_base_object(void) { return s_base; }
