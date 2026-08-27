#include "avatar_mouth.h"

#include "avatar_chroma_assets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl_port.h"

#ifndef JULIA_AVATAR_LOG
#define JULIA_AVATAR_LOG 1
#endif

static lv_obj_t *s_mouth;
static avatar_mouth_shape_t s_shape = AVATAR_MOUTH_IDLE;
static bool s_transition_active;

static const lv_img_dsc_t *source_for(avatar_mouth_shape_t shape)
{
    if (shape == AVATAR_MOUTH_IDLE) return &avatar_asset_mouth_closed;
    if (shape == AVATAR_MOUTH_SPEAK1) return &avatar_asset_mouth_half;
    if (shape == AVATAR_MOUTH_SPEAK2) return &avatar_asset_mouth_open;
    return &avatar_asset_mouth_speak3;
}

void avatar_mouth_init(lv_obj_t *parent)
{
    if (!parent || s_mouth) return;
    s_mouth = lv_img_create(parent);
    lv_img_set_src(s_mouth, source_for(AVATAR_MOUTH_IDLE));
    lv_obj_set_pos(s_mouth, 153, 177);
    lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_SCROLLABLE);
}

void avatar_mouth_set_shape(avatar_mouth_shape_t shape, uint16_t rms)
{
    if (shape > AVATAR_MOUTH_SPEAK3) shape = AVATAR_MOUTH_SPEAK3;
    if (!s_mouth || s_transition_active || shape == s_shape) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    int64_t started = esp_timer_get_time();
    lv_img_set_src(s_mouth, source_for(shape));
    s_shape = shape;
    lvgl_port_unlock();
    int64_t elapsed = esp_timer_get_time() - started;
#if JULIA_AVATAR_LOG
    ESP_LOGI("JULIA_AVATAR", "mouth shape=%u rms=%u refresh_us=%lld", shape, rms, elapsed);
#endif
    if (elapsed > 12000)
        ESP_LOGW("JULIA_AVATAR", "mouth refresh slow elapsed_us=%lld", elapsed);
}

void avatar_mouth_set_rms(uint16_t rms)
{
    avatar_mouth_shape_t shape = rms <= 15 ? AVATAR_MOUTH_IDLE :
                                 rms <= 50 ? AVATAR_MOUTH_SPEAK1 :
                                 rms <= 80 ? AVATAR_MOUTH_SPEAK2 : AVATAR_MOUTH_SPEAK3;
    avatar_mouth_set_shape(shape, rms);
}

void avatar_mouth_set_transition_active(bool active)
{
    s_transition_active = active;
    avatar_mouth_set_visible(!active);
    if (!active) {
        s_shape = AVATAR_MOUTH_SPEAK3;
        avatar_mouth_set_shape(AVATAR_MOUTH_IDLE, 0);
    }
}

void avatar_mouth_set_visible(bool visible)
{
    if (!s_mouth || !lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    if (visible) lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

lv_obj_t *avatar_mouth_object(void) { return s_mouth; }
