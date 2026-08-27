#include "julia_avatar.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "avatar_chroma_assets.h"
#include "avatar_face_base.h"
#include "avatar_eyes.h"
#include "avatar_mouth.h"
#include "lvgl_port.h"

#define AVATAR_UPDATE_MS          40U
#define AVATAR_PCM_HOLD_MS        180U
#define AVATAR_BREATH_PERIOD_MS   4200U
#define AVATAR_NOD_PERIOD_MS      11000U
#define AVATAR_NOD_DURATION_MS    720U

/* Transforming the 360x360 root invalidates the complete display on every
 * animation tick.  On the QSPI panel that frame is committed in ten strips,
 * without a TE signal to keep the writes outside the LCD scanout window.  The
 * result is continuous visible tearing/flicker.  Keep animation updates local
 * to the eyes and mouth until panel-synchronised full-frame rendering exists. */
#define AVATAR_ENABLE_FULL_FRAME_MOTION 0

static const char *TAG = "julia_avatar";
static lv_obj_t *s_motion_root;
static volatile bool s_ready;
static bool s_talking;
static uint32_t s_smoothed_rms;
static uint8_t s_mouth_level;
static uint8_t s_target_mouth_level;
static uint32_t s_last_pcm_ms;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t integer_sqrt_u64(uint64_t value)
{
    uint64_t bit = 1ULL << 62;
    uint64_t result = 0;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

static uint8_t mouth_level_for_frame(const int16_t *samples, size_t count)
{
    uint64_t energy = 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t sample = samples[i];
        energy += (uint64_t)(sample * sample);
    }
    uint32_t rms = count ? integer_sqrt_u64(energy / count) : 0;

    /* Same asymmetric thresholds as the fused lipsync module: fast attack,
     * slower release, and one-level steps avoid chatter around a boundary. */
    s_smoothed_rms = (s_smoothed_rms * 5U + rms * 3U) / 8U;
    static const uint16_t rise[] = {300, 950, 2300};
    static const uint16_t fall[] = {180, 650, 1650};
    uint8_t level = s_mouth_level;
    if (level < 3U && s_smoothed_rms >= rise[level]) {
        ++level;
    } else if (level > 0U && s_smoothed_rms < fall[level - 1U]) {
        --level;
    }
    s_mouth_level = level;
    return level;
}

void julia_avatar_feed_pcm(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0U) {
        return;
    }
    uint8_t level = mouth_level_for_frame(samples, sample_count);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    portENTER_CRITICAL(&s_state_lock);
    s_target_mouth_level = level;
    s_last_pcm_ms = now_ms;
    portEXIT_CRITICAL(&s_state_lock);
}

void julia_avatar_talking_start(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_talking = true;
    s_smoothed_rms = 0;
    s_mouth_level = 0;
    s_target_mouth_level = 0;
    s_last_pcm_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    portEXIT_CRITICAL(&s_state_lock);
}

void julia_avatar_talking_stop(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_talking = false;
    s_smoothed_rms = 0;
    s_mouth_level = 0;
    s_target_mouth_level = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

static void update_micro_motion(uint32_t now_ms)
{
#if AVATAR_ENABLE_FULL_FRAME_MOTION
    if (!s_motion_root) {
        return;
    }

    /* A tiny zoom pulse reads as breathing without exposing the screen edge. */
    uint32_t breath_phase = now_ms % AVATAR_BREATH_PERIOD_MS;
    uint32_t half = AVATAR_BREATH_PERIOD_MS / 2U;
    uint32_t triangle = breath_phase <= half ? breath_phase : AVATAR_BREATH_PERIOD_MS - breath_phase;
    lv_coord_t zoom = (lv_coord_t)(256U + (triangle * 2U + half / 2U) / half);

    /* Periodic sub-degree forward/back motion gives a restrained idle nod. */
    uint32_t nod_phase = now_ms % AVATAR_NOD_PERIOD_MS;
    int16_t angle = 0;
    if (nod_phase < AVATAR_NOD_DURATION_MS) {
        uint32_t quarter = AVATAR_NOD_DURATION_MS / 4U;
        if (nod_phase < quarter) {
            angle = (int16_t)(-(int32_t)nod_phase * 6 / (int32_t)quarter);
        } else if (nod_phase < quarter * 2U) {
            angle = (int16_t)(-6 + (int32_t)(nod_phase - quarter) * 6 / (int32_t)quarter);
        } else if (nod_phase < quarter * 3U) {
            angle = (int16_t)((int32_t)(nod_phase - quarter * 2U) * 4 / (int32_t)quarter);
        } else {
            angle = (int16_t)(4 - (int32_t)(nod_phase - quarter * 3U) * 4 / (int32_t)quarter);
        }
    }

    lv_obj_set_style_transform_zoom(s_motion_root, zoom, LV_PART_MAIN);
    lv_obj_set_style_transform_angle(s_motion_root, angle, LV_PART_MAIN);
#else
    (void)now_ms;
#endif
}

static void avatar_task(void *argument)
{
    (void)argument;
    uint8_t displayed_level = UINT8_MAX;
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        bool talking;
        uint8_t target;
        uint32_t last_pcm;
        portENTER_CRITICAL(&s_state_lock);
        talking = s_talking;
        target = s_target_mouth_level;
        last_pcm = s_last_pcm_ms;
        portEXIT_CRITICAL(&s_state_lock);

        if (!talking || (uint32_t)(now_ms - last_pcm) > AVATAR_PCM_HOLD_MS) {
            target = 0;
        }

        if (lvgl_port_lock(pdMS_TO_TICKS(20))) {
            update_micro_motion(now_ms);
            if (target != displayed_level) {
                avatar_mouth_set_shape((avatar_mouth_shape_t)target, s_smoothed_rms);
                displayed_level = target;
            }
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(AVATAR_UPDATE_MS));
    }
}

esp_err_t julia_avatar_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_motion_root = lv_obj_create(screen);
    lv_obj_set_size(s_motion_root, 360, 360);
    lv_obj_set_pos(s_motion_root, 0, 0);
    lv_obj_set_style_pad_all(s_motion_root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_motion_root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_motion_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(s_motion_root, 180, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(s_motion_root, 240, LV_PART_MAIN);
    lv_obj_clear_flag(s_motion_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *base = lv_img_create(s_motion_root);
    lv_img_set_src(base, &avatar_asset_julia_s1_1_near_standby);
    lv_obj_set_pos(base, 0, 0);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);

    avatar_eyes_init(s_motion_root);
    avatar_mouth_init(s_motion_root);
    lv_obj_invalidate(screen);
    lvgl_port_unlock();

    esp_err_t refresh_err = lvgl_port_refr_now_sync(pdMS_TO_TICKS(1000));
    if (refresh_err != ESP_OK) {
        ESP_LOGW(TAG, "first portrait refresh reported: %s", esp_err_to_name(refresh_err));
    }

    if (xTaskCreateWithCaps(avatar_task, "avatar_l1", 4096, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "Julia portrait ready: stable frame + blink + 4-level RMS mouth");
    return ESP_OK;
}

bool julia_avatar_is_ready(void)
{
    return s_ready;
}
