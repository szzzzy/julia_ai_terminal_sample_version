#include "julia_ui.h"
#include "julia_ui_showcase.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl_port.h"
#include "julia_ui_assets.h"
#include "julia_blink_assets.h" /* Legacy declarations; dead full-frame helpers are link-GC'd. */
#include "avatar_micro_motion.h"
#include "avatar_anim_engine.h"
#include "avatar_clip_map.h"
#include "transition_director.h"
#include "transition_player.h"
#include "idle_player.h"
#include "julia_display_theme.h"
#include "breathing_led.h"
#include "julia_rig_assets.h"
#include "avatar_layer_assets.h"
#include "avatar_face.h"
#include "avatar_mouth.h"
#include "julia_sd.h"
#include "extra/libs/png/lodepng.h"

extern const uint8_t reference_png_start[] asm("_binary_julia_reference_ui_png_start");
extern const uint8_t reference_png_end[] asm("_binary_julia_reference_ui_png_end");

typedef struct {
    lv_obj_t *avatar_slot;
    lv_obj_t *avatar_container;
    lv_obj_t *neck_container;
    lv_obj_t *head_container;
    lv_obj_t *avatar_image;
    lv_obj_t *stream_canvas;
    lv_obj_t *stream_canvas_alt;
    lv_obj_t *face;
    lv_obj_t *eyes;
    lv_obj_t *mouth;
    lv_obj_t *expression_label;
    lv_obj_t *bubble_label;
    lv_obj_t *sleep_blackout;
    lv_obj_t *rig_root;
    lv_obj_t *rig_body;
    lv_obj_t *rig_head;
    lv_obj_t *rig_hair_front;
    lv_obj_t *rig_hair_back;
    lv_obj_t *rig_eye_left;
    lv_obj_t *rig_eye_right;
    lv_obj_t *rig_eyelid_left;
    lv_obj_t *rig_eyelid_right;
    lv_obj_t *rig_pupil_left;
    lv_obj_t *rig_pupil_right;
    lv_obj_t *rig_mouth;
    lv_anim_t mouth_anim;
    bool initialized;
} julia_ui_ctx_t;

static julia_ui_ctx_t s_ui;
static const char *TAG = "JULIA_UI";
static void transition_target_commit(julia_sub_state_t target);
#ifndef AVATAR_LAYER_DEBUG
#define AVATAR_LAYER_DEBUG 0
#endif

#ifndef JULIA_ANIM_LOG
#define JULIA_ANIM_LOG 1
#endif
static lv_color_t *s_avatar_pixels;
static lv_color_t *s_state_pixels;
#define DOZE_DMA_ROWS 12
static lv_color_t *s_doze_dma_rows;
static lv_color_t *s_doze_frame;
static bool s_doze_frame_loaded;
static uint8_t *s_png_data;
static lv_img_dsc_t s_png_image;
static esp_lcd_panel_handle_t s_panel;
static volatile julia_sub_state_t s_current_state = JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
static volatile bool s_transitioning;
static volatile bool s_blinking;
static volatile bool s_talking;
static volatile bool s_program_blink_enabled = true;
static volatile bool s_program_motion_mode = true;
static volatile bool s_idle_frame_mode;
static volatile julia_sub_state_t s_idle_frame_state = JULIA_SUB_STATE_COUNT;
static volatile bool s_transition_frame_mode;
static volatile julia_dialog_phase_t s_dialog_phase;
static TickType_t s_state_entered_at;
static QueueHandle_t s_state_queue;
static void state_transition_apply(julia_sub_state_t state);
static void state_worker_task(void *argument);
static esp_err_t draw_avatar_rows(esp_lcd_panel_handle_t panel, int y_start, int y_end);
static esp_err_t draw_avatar_region(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                    int x_end, int y_end);
static __attribute__((unused)) void dialog_phase_set_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)x);
}

static void stream_canvas_draw_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DRAW_POST_END)
        avatar_anim_engine_on_canvas_draw_complete();
}

#define BLINK_X0 82
#define BLINK_Y0 108
#define BLINK_X1 278
#define BLINK_Y1 196

static lv_obj_t *create_rig_image(lv_obj_t *parent, const lv_img_dsc_t *source, int x, int y)
{
    if (!parent || !source) return NULL;
    lv_obj_t *image = lv_img_create(parent);
    lv_img_set_src(image, source);
    lv_obj_set_pos(image, x, y);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_SCROLLABLE);
    return image;
}

static bool layer_source_valid(const char *name, const lv_img_dsc_t *source)
{
    bool valid = avatar_layer_asset_valid(source);
    if (!valid) {
        ESP_LOGW("AVATAR_LAYER", "%s rejected: descriptor/data range invalid data=%p",
                 name, source ? source->data : NULL);
    }
    return valid;
}

static void audit_layer(const char *name, const lv_img_dsc_t *source)
{
    const avatar_layer_asset_info_t *theory = avatar_layer_asset_info(source);
    if (!source || !theory) {
        ESP_LOGE("AVATAR_LAYER", "%s descriptor/theory missing", name);
        return;
    }
    uint32_t actual_stride = source->header.h ? source->data_size / source->header.h : 0;
    bool match = avatar_layer_asset_valid(source) && actual_stride == theory->stride;
    const char *residency = esp_ptr_external_ram(source->data) ? "PSRAM" :
                            (esp_ptr_in_drom(source->data) ? "FLASH" : "INVALID");
    ESP_LOGI("AVATAR_LAYER",
             "%s dsc cf=%u w=%u h=%u stride=%lu size=%lu data=%p residency=%s",
             name, source->header.cf, source->header.w, source->header.h,
             (unsigned long)actual_stride, (unsigned long)source->data_size,
             source->data, residency);
    ESP_LOG_BUFFER_HEX_LEVEL("AVATAR_LAYER", source->data,
                             source->data_size < 16 ? source->data_size : 16, ESP_LOG_INFO);
    ESP_LOGI("AVATAR_LAYER",
             "%s theory cf=%u w=%u h=%u stride=%u size=%lu linker=%lu compare=%s",
             theory->name, theory->color_format, theory->width, theory->height,
             theory->stride, (unsigned long)theory->data_size,
             (unsigned long)(theory->data_end - theory->data_start),
             match ? "MATCH" : "MISMATCH");
}

static void audit_layer_transforms(void)
{
    const struct { const char *name; lv_obj_t *obj; } objects[] = {
        {"avatar", s_ui.avatar_container}, {"neck", s_ui.neck_container},
        {"head", s_ui.head_container}, {"eye_left", s_ui.rig_eye_left},
        {"eye_right", s_ui.rig_eye_right}, {"mouth", s_ui.rig_mouth},
    };
    for (unsigned i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
        if (!objects[i].obj) continue;
        ESP_LOGI("AVATAR_LAYER", "%s transform angle=%d pivot=(%d,%d) zoom=%d",
                 objects[i].name,
                 lv_obj_get_style_transform_angle(objects[i].obj, 0),
                 lv_obj_get_style_transform_pivot_x(objects[i].obj, 0),
                 lv_obj_get_style_transform_pivot_y(objects[i].obj, 0),
                 lv_obj_get_style_transform_zoom(objects[i].obj, 0));
    }
}

static __attribute__((unused)) const lv_img_dsc_t *valid_mouth_source(avatar_mouth_frame_t frame)
{
    const lv_img_dsc_t *source = avatar_layer_mouth(frame);
    if (layer_source_valid("mouth", source)) return source;
    source = avatar_layer_mouth(AVATAR_MOUTH_CLOSED);
    return layer_source_valid("mouth_default", source) ? source : NULL;
}

static lv_obj_t *create_avatar_container(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, 360, 360);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(container, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    return container;
}

static void set_rig_visible(bool visible)
{
    if (!s_ui.rig_root || !s_ui.avatar_image) return;
    if (visible) {
        lv_obj_clear_flag(s_ui.rig_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.avatar_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.rig_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ui.avatar_image, LV_OBJ_FLAG_HIDDEN);
    }
}

static __attribute__((unused)) bool state_uses_live_rig(julia_sub_state_t state)
{
    (void)state;
    return false;
}

#define AVATAR_SIZE 360
#define TRANSITION_X0 56
#define TRANSITION_Y0 32
#define TRANSITION_X1 304
#define TRANSITION_Y1 260
#define TRANSITION_FRAMES 14

static bool load_image_source(const lv_img_dsc_t *source)
{
    if (!source || !s_avatar_pixels) return false;
    const uint8_t *bytes = source->data;
    const int output_size = 360;
    uint8_t *decoded = NULL;
    unsigned decoded_width = 0;
    unsigned decoded_height = 0;
    bool is_png = source->data_size >= 8 &&
                  memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0;
    if (is_png) {
        unsigned error = lodepng_decode24(&decoded, &decoded_width, &decoded_height,
                                          bytes, source->data_size);
        if (error || !decoded || !decoded_width || !decoded_height) {
            if (decoded) lv_mem_free(decoded);
            ESP_LOGE(TAG, "PNG decode failed: %u", error);
            return false;
        }
        for (int y = 0; y < output_size; ++y) {
            unsigned sy = (unsigned)y * decoded_height / output_size;
            for (int x = 0; x < output_size; ++x) {
                unsigned sx = (unsigned)x * decoded_width / output_size;
                size_t index = ((size_t)sy * decoded_width + sx) * 3;
                s_avatar_pixels[(size_t)y * output_size + x] =
                    lv_color_make(decoded[index], decoded[index + 1], decoded[index + 2]);
            }
        }
        lv_mem_free(decoded);
        return true;
    }
    for (int y = 0; y < output_size; ++y) {
        int sy = y * source->header.h / output_size;
        for (int x = 0; x < output_size; ++x) {
            int sx = x * source->header.w / output_size;
            size_t source_index = (size_t)sy * source->header.w + sx;
            uint16_t raw = ((uint16_t)bytes[source_index * 2] << 8) |
                           (uint16_t)bytes[source_index * 2 + 1];
            uint8_t r = (uint8_t)(((raw >> 11) & 0x1f) * 255 / 31);
            uint8_t g = (uint8_t)(((raw >> 5) & 0x3f) * 255 / 63);
            uint8_t b = (uint8_t)((raw & 0x1f) * 255 / 31);
            s_avatar_pixels[(size_t)y * output_size + x] = lv_color_make(r, g, b);
        }
    }
    return true;
}

/* Replace only the eye rectangle in the single canvas buffer. The blink
 * assets are opaque RGB565 frames, so no old/new eye alpha blending occurs. */
static __attribute__((unused)) bool load_blink_eye_frame(uint8_t frame)
{
    if (!s_avatar_pixels || !s_state_pixels) return false;
    if (frame == 0 || frame >= 4) {
        for (int y = BLINK_Y0; y < BLINK_Y1; ++y) {
            memcpy(&s_avatar_pixels[(size_t)y * AVATAR_SIZE + BLINK_X0],
                   &s_state_pixels[(size_t)y * AVATAR_SIZE + BLINK_X0],
                   (BLINK_X1 - BLINK_X0) * sizeof(lv_color_t));
        }
        return true;
    }
    const lv_img_dsc_t *source = julia_blink_frame(frame);
    if (!source || source->header.cf != LV_IMG_CF_TRUE_COLOR ||
        source->header.w != AVATAR_SIZE || source->header.h != AVATAR_SIZE) return false;
    const uint8_t *bytes = source->data;
    for (int y = BLINK_Y0; y < BLINK_Y1; ++y) {
        for (int x = BLINK_X0; x < BLINK_X1; ++x) {
            size_t i = (size_t)y * AVATAR_SIZE + x;
            uint16_t raw = (uint16_t)bytes[i * 2] | ((uint16_t)bytes[i * 2 + 1] << 8);
            uint8_t r = (uint8_t)(((raw >> 11) & 0x1f) * 255 / 31);
            uint8_t g = (uint8_t)(((raw >> 5) & 0x3f) * 255 / 63);
            uint8_t b = (uint8_t)((raw & 0x1f) * 255 / 31);
            s_avatar_pixels[i] = lv_color_make(r, g, b);
        }
    }
    return true;
}

static bool show_blink_frame(uint8_t frame)
{
    if (!lvgl_port_lock(pdMS_TO_TICKS(300))) return false;
    uint8_t local_frame = frame == 2 ? 2 : (frame == 0 ? 0 : 1);
    bool loaded = s_ui.rig_eye_left && s_ui.rig_eye_right;
    if (loaded) {
        const lv_img_dsc_t *left = avatar_layer_eye(true, local_frame);
        const lv_img_dsc_t *right = avatar_layer_eye(false, local_frame);
        if (!left || !right) loaded = false;
        else {
            lv_img_set_src(s_ui.rig_eye_left, left);
            lv_img_set_src(s_ui.rig_eye_right, right);
        }
    }
    if (loaded) {
        if (local_frame == 0) {
            lv_obj_clear_flag(s_ui.rig_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_ui.rig_pupil_right, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.rig_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.rig_pupil_right, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_port_unlock();
    if (loaded) ESP_LOGI("AVATAR_EVENT", "t=%lld blink frame=%u", esp_timer_get_time()/1000, local_frame);
    return loaded;
}

static bool install_reference_png(void)
{
    if (!julia_sd_is_mounted()) return false;
    const char *path = JULIA_SD_MOUNT_POINT "/reference.png";
    size_t embedded_size = (size_t)(reference_png_end - reference_png_start);
    struct stat info;
    if (stat(path, &info) == 0 && (size_t)info.st_size == embedded_size) return true;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t written = fwrite(reference_png_start, 1, embedded_size, file);
    fclose(file);
    return written == embedded_size;
}

static bool load_png_from_sd(void)
{
    const char *path = JULIA_SD_MOUNT_POINT "/reference.png";
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    s_png_data = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_png_data) { fclose(file); return false; }
    size_t read_size = fread(s_png_data, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) { free(s_png_data); s_png_data = NULL; return false; }
    s_png_image.header.always_zero = 0;
    s_png_image.header.w = 0;
    s_png_image.header.h = 0;
    s_png_image.header.cf = 0;
    s_png_image.data_size = (uint32_t)size;
    s_png_image.data = s_png_data;
    return true;
}

static void apply_attentive_pose(void)
{
    enum { X0 = 70, Y0 = 40, X1 = 290, Y1 = 250, FEATHER = 14 };
    const int width = X1 - X0, height = Y1 - Y0;
    lv_color_t *original = heap_caps_malloc((size_t)width * height * sizeof(lv_color_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!original) return;
    for (int y = 0; y < height; ++y) {
        memcpy(original + (size_t)y * width,
               s_avatar_pixels + (size_t)(Y0 + y) * AVATAR_SIZE + X0,
               width * sizeof(lv_color_t));
    }
    const int cx = width / 2, cy = height / 2;
    for (int y = 0; y < height; ++y) {
        int sy = cy + ((y - cy) * 1000) / 1025;
        if (sy < 0) sy = 0;
        if (sy >= height) sy = height - 1;
        for (int x = 0; x < width; ++x) {
            int sx = cx + ((x - cx) * 1000) / 1025;
            if (sx < 0) sx = 0;
            if (sx >= width) sx = width - 1;
            int edge = x;
            if (width - 1 - x < edge) edge = width - 1 - x;
            if (y < edge) edge = y;
            if (height - 1 - y < edge) edge = height - 1 - y;
            lv_opa_t opacity = edge >= FEATHER ? LV_OPA_COVER
                                                : (lv_opa_t)(edge * 255 / FEATHER);
            lv_color_t base = original[(size_t)y * width + x];
            lv_color_t zoomed = original[(size_t)sy * width + sx];
            s_avatar_pixels[(size_t)(Y0 + y) * AVATAR_SIZE + X0 + x] =
                lv_color_mix(zoomed, base, opacity);
        }
    }
    free(original);
}

static bool load_avatar(julia_sub_state_t state)
{
    const lv_img_dsc_t *source = julia_ui_asset_for_state(state);
    bool loaded = load_image_source(source);
    if (loaded && state == JULIA_SUB_STATE_S3_3_USER_CALL) apply_attentive_pose();
    return loaded;
}

static __attribute__((unused)) void blink_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(3000 + esp_random() % 5001));
        if (!s_panel || s_transitioning) continue;
        bool standby = s_current_state >= JULIA_SUB_STATE_S1_1_NEAR_STANDBY &&
                       s_current_state <= JULIA_SUB_STATE_S1_3_CHARGING_STANDBY;
        bool attentive = s_current_state >= JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER &&
                         s_current_state <= JULIA_SUB_STATE_S3_4_RECOVERY_PROBE;
        if (!standby && !attentive) continue;
        if (!s_program_blink_enabled) continue;
        s_blinking = true;
        avatar_motion_pause();
        int count = (esp_random() % 100U) < 15U ? 2 : 1;
        for (int blink = 0; blink < count && !s_transitioning; ++blink) {
            if (!show_blink_frame(1)) break;
            int64_t layer_started = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(55));
            if (!show_blink_frame(2)) break;
            vTaskDelay(pdMS_TO_TICKS(65));
            if (!show_blink_frame(3)) break;
            vTaskDelay(pdMS_TO_TICKS(65));
            if (!show_blink_frame(0)) break;
#if JULIA_ANIM_LOG
            ESP_LOGI("JULIA_ANIM", "blink state=S%u duration_ms=%lld local_layers=eyes",
                     standby ? JULIA_MAIN_STATE_S1_STANDBY : JULIA_MAIN_STATE_S3_INITIATIVE,
                     (esp_timer_get_time() - layer_started) / 1000);
#endif
            if (blink + 1 < count) vTaskDelay(pdMS_TO_TICKS(95));
        }
        s_blinking = false;
        avatar_motion_resume();
    }
}

void julia_ui_set_program_blink_enabled(bool enabled) { s_program_blink_enabled = enabled; }

static __attribute__((unused)) bool apply_mouth_patch(uint8_t level)
{
    const lv_img_dsc_t *source = julia_mouth_asset(level);
    if (!source || !s_avatar_pixels) return false;
    const uint8_t *bytes = source->data;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            size_t source_index = ((size_t)y * 64 + x) * 2;
            uint16_t raw = (uint16_t)bytes[source_index] |
                           ((uint16_t)bytes[source_index + 1] << 8);
            s_avatar_pixels[(size_t)(y + 175) * 360 + x + 148] = lv_color_make(
                (uint8_t)(((raw >> 11) & 0x1f) * 255 / 31),
                (uint8_t)(((raw >> 5) & 0x3f) * 255 / 63),
                (uint8_t)((raw & 0x1f) * 255 / 31));
        }
    }
    return true;
}

static __attribute__((unused)) bool apply_mouth_patch_blended(uint16_t openness_q8)
{
    if (!s_avatar_pixels) return false;
    if (openness_q8 > 3U * 256U) openness_q8 = 3U * 256U;
    uint8_t low = openness_q8 >> 8;
    uint8_t high = low < 3 ? low + 1 : low;
    lv_opa_t mix = (lv_opa_t)(openness_q8 & 0xffU);
    const lv_img_dsc_t *low_source = julia_mouth_asset(low);
    const lv_img_dsc_t *high_source = julia_mouth_asset(high);
    if (!low_source || !high_source) return false;
    const uint8_t *low_bytes = low_source->data;
    const uint8_t *high_bytes = high_source->data;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            size_t i = ((size_t)y * 64 + x) * 2;
            uint16_t low_raw = (uint16_t)low_bytes[i] | ((uint16_t)low_bytes[i + 1] << 8);
            uint16_t high_raw = (uint16_t)high_bytes[i] | ((uint16_t)high_bytes[i + 1] << 8);
            lv_color_t low_color = lv_color_make(
                (uint8_t)(((low_raw >> 11) & 0x1f) * 255 / 31),
                (uint8_t)(((low_raw >> 5) & 0x3f) * 255 / 63),
                (uint8_t)((low_raw & 0x1f) * 255 / 31));
            lv_color_t high_color = lv_color_make(
                (uint8_t)(((high_raw >> 11) & 0x1f) * 255 / 31),
                (uint8_t)(((high_raw >> 5) & 0x3f) * 255 / 63),
                (uint8_t)((high_raw & 0x1f) * 255 / 31));
            s_avatar_pixels[(size_t)(y + 175) * 360 + x + 148] =
                lv_color_mix(high_color, low_color, mix);
        }
    }
    return true;
}

static const uint32_t s_expr_colors[JULIA_EXPR_COUNT] = {
    [JULIA_EXPR_SLEEP] = 0x596275,
    [JULIA_EXPR_WATCHING] = 0x52A7A0,
    [JULIA_EXPR_HAPPY] = 0xF2B84B,
    [JULIA_EXPR_SPEAKING] = 0xEA6A61,
    [JULIA_EXPR_CONFUSED] = 0x8B79A8,
};

static const char *s_expr_names[JULIA_EXPR_COUNT] = {
    [JULIA_EXPR_SLEEP] = "Sleep",
    [JULIA_EXPR_WATCHING] = "Watching",
    [JULIA_EXPR_HAPPY] = "Happy",
    [JULIA_EXPR_SPEAKING] = "Speaking",
    [JULIA_EXPR_CONFUSED] = "Confused",
};

static void set_mouth_height(void *obj, int32_t value)
{
    lv_obj_set_height((lv_obj_t *)obj, value);
}

static void stop_mouth_anim(void)
{
    lv_anim_del(s_ui.mouth, set_mouth_height);
    lv_obj_set_height(s_ui.mouth, 5);
}

static void start_mouth_anim(uint8_t intensity)
{
    stop_mouth_anim();
    lv_anim_init(&s_ui.mouth_anim);
    lv_anim_set_var(&s_ui.mouth_anim, s_ui.mouth);
    lv_anim_set_exec_cb(&s_ui.mouth_anim, set_mouth_height);
    lv_anim_set_values(&s_ui.mouth_anim, 4, 6 + intensity / 7);
    lv_anim_set_time(&s_ui.mouth_anim, 180);
    lv_anim_set_playback_time(&s_ui.mouth_anim, 180);
    lv_anim_set_repeat_count(&s_ui.mouth_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_ui.mouth_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&s_ui.mouth_anim);
}

static void apply_expression(expr_t expr, uint8_t intensity)
{
    if (expr >= JULIA_EXPR_COUNT) {
        return;
    }
    if (intensity > 100) {
        intensity = 100;
    }

    /* Generated portraits fully replace the legacy geometric face. */
    return;

    lv_obj_set_style_bg_color(s_ui.face, lv_color_hex(s_expr_colors[expr]), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.face, 150 + intensity, LV_PART_MAIN);
    lv_label_set_text(s_ui.expression_label, s_expr_names[expr]);

    switch (expr) {
    case JULIA_EXPR_SLEEP:
        lv_label_set_text(s_ui.eyes, "-   -");
        lv_obj_set_style_radius(s_ui.mouth, 0, LV_PART_MAIN);
        break;
    case JULIA_EXPR_WATCHING:
        lv_label_set_text(s_ui.eyes, "o   o");
        lv_obj_set_style_radius(s_ui.mouth, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        break;
    case JULIA_EXPR_HAPPY:
        lv_label_set_text(s_ui.eyes, "^   ^");
        lv_obj_set_style_radius(s_ui.mouth, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        break;
    case JULIA_EXPR_SPEAKING:
        lv_label_set_text(s_ui.eyes, "o   o");
        lv_obj_set_style_radius(s_ui.mouth, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        break;
    case JULIA_EXPR_CONFUSED:
        lv_label_set_text(s_ui.eyes, "o   ?");
        lv_obj_set_style_radius(s_ui.mouth, 0, LV_PART_MAIN);
        break;
    default:
        break;
    }

    if (expr == JULIA_EXPR_SPEAKING) {
        start_mouth_anim(intensity);
    } else {
        stop_mouth_anim();
    }
}

void julia_ui_init(void)
{
    ESP_LOGI(TAG, "UI asset version: JULIA_V3_20260724");
    if (!lvgl_port_lock(portMAX_DELAY)) {
        return;
    }

    ESP_LOGI(TAG, "Building standby UI");
    if (!s_doze_dma_rows) {
        s_doze_dma_rows = heap_caps_malloc(360U * DOZE_DMA_ROWS * sizeof(lv_color_t),
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_doze_dma_rows) ESP_LOGE(TAG, "Doze DMA workspace reservation failed");
    }
    if (!s_doze_frame_loaded && julia_sd_is_mounted() &&
        julia_sd_lock(pdMS_TO_TICKS(1500))) {
        FILE *doze = fopen(DOZE_FRAME_PATH, "rb");
        const size_t bytes = 360U * 360U * sizeof(lv_color_t);
        struct stat info;
        if (doze && fstat(fileno(doze), &info) == 0 && (size_t)info.st_size == bytes) {
            s_doze_frame = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            s_doze_frame_loaded = s_doze_frame && fread(s_doze_frame, 1, bytes, doze) == bytes;
            if (!s_doze_frame_loaded) {
                heap_caps_free(s_doze_frame);
                s_doze_frame = NULL;
            }
        }
        if (doze) fclose(doze);
        julia_sd_unlock();
        ESP_LOGI(TAG, "Doze asset preload source=%s bytes=%u",
                 s_doze_frame_loaded ? "sd" : "black-fallback", (unsigned)bytes);
    }

    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101317), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.avatar_slot = lv_obj_create(screen);
    lv_obj_add_flag(s_ui.avatar_slot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_ui.avatar_slot, 360, 360);
    lv_obj_align(s_ui.avatar_slot, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(s_ui.avatar_slot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.avatar_slot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.avatar_slot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_ui.avatar_slot, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.face = lv_obj_create(s_ui.avatar_slot);
    lv_obj_set_size(s_ui.face, 150, 150);
    lv_obj_align(s_ui.face, LV_ALIGN_CENTER, 0, 2);
    lv_obj_set_style_radius(s_ui.face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.face, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.face, lv_color_hex(0xF4F6F8), LV_PART_MAIN);
    lv_obj_clear_flag(s_ui.face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.face, LV_OBJ_FLAG_HIDDEN);

    s_ui.avatar_container = create_avatar_container(s_ui.avatar_slot);
    s_ui.neck_container = create_avatar_container(s_ui.avatar_container);
    s_ui.head_container = create_avatar_container(s_ui.neck_container);
    s_ui.avatar_image = lv_canvas_create(s_ui.head_container);
    if (install_reference_png() && load_png_from_sd()) {
        /* Keep the SD resource ready for future screens. */
    }
    s_avatar_pixels = heap_caps_malloc(360 * 360 * sizeof(lv_color_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_avatar_pixels && load_avatar(JULIA_SUB_STATE_S1_1_NEAR_STANDBY)) {
        lv_canvas_set_buffer(s_ui.avatar_image, s_avatar_pixels, 360, 360, LV_IMG_CF_TRUE_COLOR);
    }
    s_state_pixels = heap_caps_malloc(360 * 360 * sizeof(lv_color_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_state_pixels && s_avatar_pixels) {
        memcpy(s_state_pixels, s_avatar_pixels, 360 * 360 * sizeof(lv_color_t));
    }
    s_state_entered_at = xTaskGetTickCount();
    lv_obj_align(s_ui.avatar_image, LV_ALIGN_CENTER, 0, 0);
    s_ui.stream_canvas = lv_canvas_create(s_ui.head_container);
    lv_obj_set_size(s_ui.stream_canvas, AVATAR_SIZE, AVATAR_SIZE);
    lv_obj_align(s_ui.stream_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_ui.stream_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ui.stream_canvas, stream_canvas_draw_event,
                        LV_EVENT_DRAW_POST_END, NULL);
    s_ui.stream_canvas_alt = lv_canvas_create(s_ui.head_container);
    lv_obj_set_size(s_ui.stream_canvas_alt, AVATAR_SIZE, AVATAR_SIZE);
    lv_obj_align(s_ui.stream_canvas_alt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_ui.stream_canvas_alt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ui.stream_canvas_alt, stream_canvas_draw_event,
                        LV_EVENT_DRAW_POST_END, NULL);

    s_ui.eyes = lv_label_create(s_ui.face);
    lv_obj_set_style_text_color(s_ui.eyes, lv_color_hex(0x20242A), LV_PART_MAIN);
    lv_obj_align(s_ui.eyes, LV_ALIGN_CENTER, 0, -25);

    s_ui.mouth = lv_obj_create(s_ui.face);
    lv_obj_set_size(s_ui.mouth, 38, 5);
    lv_obj_align(s_ui.mouth, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_style_bg_color(s_ui.mouth, lv_color_hex(0x4A2830), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.mouth, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_ui.mouth, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.expression_label = lv_label_create(s_ui.avatar_slot);
    lv_obj_add_flag(s_ui.expression_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(s_ui.expression_label, lv_color_hex(0xE9EDF2), LV_PART_MAIN);
    lv_obj_align(s_ui.expression_label, LV_ALIGN_BOTTOM_MID, 0, -1);

    /* Remove the old translucent face and all of its eye/mouth children. */
    lv_obj_del(s_ui.face);
    s_ui.face = NULL;
    s_ui.eyes = NULL;
    s_ui.mouth = NULL;

    s_ui.rig_root = lv_obj_create(s_ui.avatar_slot);
    lv_obj_set_size(s_ui.rig_root, 360, 360);
    lv_obj_set_pos(s_ui.rig_root, 0, 0);
    lv_obj_set_style_pad_all(s_ui.rig_root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.rig_root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.rig_root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.rig_root, lv_color_hex(0xFAF9F6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.rig_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_ui.rig_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Use the pixel-verified composite as the baseline renderer. The
     * individual transparent layers remain embedded for staged re-enabling,
     * but are not mixed into the live tree until each one passes hardware
     * validation. */
    s_ui.rig_body = create_rig_image(s_ui.rig_root, julia_rig_composite(), 0, 0);
    s_ui.rig_eyelid_left = lv_obj_create(s_ui.rig_root);
    s_ui.rig_eyelid_right = lv_obj_create(s_ui.rig_root);
    lv_obj_t *eyelids[] = {s_ui.rig_eyelid_left, s_ui.rig_eyelid_right};
    for (int i = 0; i < 2; ++i) {
        lv_obj_set_size(eyelids[i], 51, 1);
        lv_obj_set_pos(eyelids[i], i == 0 ? 108 : 199, 132);
        lv_obj_set_style_bg_color(eyelids[i], lv_color_hex(0xF4D9D0), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(eyelids[i], LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(eyelids[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(eyelids[i], 10, LV_PART_MAIN);
        lv_obj_clear_flag(eyelids[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_move_foreground(s_ui.rig_eyelid_left);
    lv_obj_move_foreground(s_ui.rig_eyelid_right);
    /* Restore the original high-quality flat portrait as the user-visible
     * baseline. Keep the experimental rig allocated but hidden until a new
     * layer set has been validated against this exact character. */
    set_rig_visible(false);

    avatar_face_init(s_ui.head_container);
    s_ui.rig_eye_left = avatar_face_left_eye();
    s_ui.rig_eye_right = avatar_face_right_eye();
    s_ui.rig_mouth = avatar_face_mouth();
    s_ui.rig_pupil_left = NULL;
    s_ui.rig_pupil_right = NULL;
    s_ui.rig_hair_front = NULL;

    lv_obj_t *bubble = lv_obj_create(screen);
    lv_obj_set_size(bubble, 328, 112);
    lv_obj_align(bubble, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(bubble, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0xF3F5F7), LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bubble, 14, LV_PART_MAIN);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);

    s_ui.bubble_label = lv_label_create(bubble);
    lv_obj_set_width(s_ui.bubble_label, 300);
    lv_label_set_long_mode(s_ui.bubble_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ui.bubble_label, "I'm here with you.");
    lv_obj_set_style_text_color(s_ui.bubble_label, lv_color_hex(0x20242A), LV_PART_MAIN);
    lv_obj_align(s_ui.bubble_label, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 常驻黑层只在息屏提交帧时显示，避免动态创建对象造成碎片。 */
    s_ui.sleep_blackout = lv_obj_create(screen);
    lv_obj_remove_style_all(s_ui.sleep_blackout);
    lv_obj_set_size(s_ui.sleep_blackout, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_ui.sleep_blackout, 0, 0);
    lv_obj_set_style_bg_color(s_ui.sleep_blackout, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.sleep_blackout, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_ui.sleep_blackout, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.sleep_blackout, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_ui.sleep_blackout);
    s_ui.initialized = true;
    transition_director_init(transition_target_commit);
    idle_player_init();
    /* 分层眼睛由 avatar_micro_motion 统一调度。 */
    avatar_layer_bindings_t motion_layers = {
        .container=s_ui.avatar_container, .neck=s_ui.neck_container, .head=s_ui.head_container,
        .left_eye=s_ui.rig_eye_left, .right_eye=s_ui.rig_eye_right,
        .left_pupil=s_ui.rig_pupil_left, .right_pupil=s_ui.rig_pupil_right,
        .mouth=s_ui.rig_mouth, .hair_front=s_ui.rig_hair_front,
    };
    avatar_micro_motion_init(&motion_layers);
    audit_layer("eye_left_open", avatar_layer_eye(true, 0));
    audit_layer("eye_left_half", avatar_layer_eye(true, 1));
    audit_layer("eye_left_closed", avatar_layer_eye(true, 2));
    audit_layer("eye_right_open", avatar_layer_eye(false, 0));
    audit_layer("eye_right_half", avatar_layer_eye(false, 1));
    audit_layer("eye_right_closed", avatar_layer_eye(false, 2));
    audit_layer("mouth_closed", avatar_layer_mouth(AVATAR_MOUTH_CLOSED));
    audit_layer("mouth_half", avatar_layer_mouth(AVATAR_MOUTH_HALF));
    audit_layer("mouth_open", avatar_layer_mouth(AVATAR_MOUTH_OPEN));
    audit_layer_transforms();
    /* Blink scheduling is owned by avatar_parts/avatar_eyes.c. */
    /* 静态渲染诊断期间禁用所有会修改人物画布的后台任务。 */
    apply_expression(JULIA_EXPR_SLEEP, 50);
    ESP_LOGI(TAG, "Standby pixels ready");

    /* 状态请求采用异步队列：发送方只负责投递，动画状态机任务负责执行。
     * 深度 64 可覆盖维护命令的快速突发；任务会合并队列中连续请求，
     * 仅执行最后一个目标，避免过时状态逐个播放。 */
    s_state_queue = xQueueCreate(64, sizeof(julia_sub_state_t));
    if (s_state_queue == NULL) {
        ESP_LOGE(TAG, "state request queue create failed");
    } else if (xTaskCreate(state_worker_task, "avatar_state", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "state worker create failed");
        vQueueDelete(s_state_queue);
        s_state_queue = NULL;
    }
    lvgl_port_unlock();
}

void avatar_show_all(void)
{
    if (!s_ui.initialized || !lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    lv_obj_clear_flag(s_ui.avatar_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.neck_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.head_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.avatar_slot, LV_OBJ_FLAG_HIDDEN);
    avatar_motion_resume_all();
    lv_obj_invalidate(s_ui.avatar_slot);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "avatar layers shown atomically; motion resumed");
}

void julia_ui_set_sleep_blackout(bool enabled)
{
    if (!s_ui.initialized || !s_ui.sleep_blackout) return;
    if (enabled) {
        lv_obj_clear_flag(s_ui.sleep_blackout, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ui.sleep_blackout);
    } else {
        lv_obj_add_flag(s_ui.sleep_blackout, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(lv_scr_act());
}

void julia_ui_set_sleep_blackout_opa(uint8_t opacity)
{
    if (!s_ui.initialized || !s_ui.sleep_blackout) return;
    lv_obj_clear_flag(s_ui.sleep_blackout, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.sleep_blackout);
    lv_obj_set_style_bg_opa(s_ui.sleep_blackout, opacity, 0);
    lv_obj_invalidate(s_ui.sleep_blackout);
}

void julia_ui_set_expression(expr_t expr, uint8_t intensity)
{
    if (!s_ui.initialized || !lvgl_port_lock(portMAX_DELAY)) {
        return;
    }
    apply_expression(expr, intensity);
    lvgl_port_unlock();
}

static julia_main_state_t main_state_for(julia_sub_state_t state)
{
    return state <= JULIA_SUB_STATE_S0_3_MANUAL_SLEEP ? JULIA_MAIN_STATE_S0_SLEEP :
        state <= JULIA_SUB_STATE_S1_3_CHARGING_STANDBY ? JULIA_MAIN_STATE_S1_STANDBY :
        state <= JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION ? JULIA_MAIN_STATE_S2_COMPANION :
        state <= JULIA_SUB_STATE_S3_4_RECOVERY_PROBE ? JULIA_MAIN_STATE_S3_INITIATIVE :
        state <= JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE ? JULIA_MAIN_STATE_S4_DIALOG : JULIA_MAIN_STATE_S5_SILENT;
}

static void transition_target_commit(julia_sub_state_t target)
{
    avatar_clip_map_set_state((uint8_t)target);
    transition_director_preload_for(main_state_for(target));
    idle_player_enter(target);
}

static void streamed_transition_done(julia_main_state_t from, julia_main_state_t to,
                                     esp_err_t result, void *context)
{
    julia_sub_state_t target = (julia_sub_state_t)(uintptr_t)context;
    ESP_LOGI(TAG, "TRN transition complete S%u->S%u target=%u result=%s",
             from, to, target, esp_err_to_name(result));
    transition_target_commit(target);
}

static void state_transition_apply(julia_sub_state_t state)
{
    if (!s_ui.initialized || state >= JULIA_SUB_STATE_COUNT || !lvgl_port_lock(portMAX_DELAY)) {
        return;
    }
    if (state == s_current_state) {
        lvgl_port_unlock();
        return;
    }
    julia_sub_state_t previous = s_current_state;
    s_current_state = state;
    static const uint16_t durations[JULIA_MAIN_STATE_COUNT][JULIA_MAIN_STATE_COUNT] = {
        [JULIA_MAIN_STATE_S0_SLEEP][JULIA_MAIN_STATE_S1_STANDBY] = 800,
        [JULIA_MAIN_STATE_S1_STANDBY][JULIA_MAIN_STATE_S0_SLEEP] = 1000,
        [JULIA_MAIN_STATE_S1_STANDBY][JULIA_MAIN_STATE_S2_COMPANION] = 600,
        [JULIA_MAIN_STATE_S2_COMPANION][JULIA_MAIN_STATE_S3_INITIATIVE] = 400,
        [JULIA_MAIN_STATE_S3_INITIATIVE][JULIA_MAIN_STATE_S4_DIALOG] = 300,
        [JULIA_MAIN_STATE_S4_DIALOG][JULIA_MAIN_STATE_S1_STANDBY] = 800,
        [JULIA_MAIN_STATE_S4_DIALOG][JULIA_MAIN_STATE_S5_SILENT] = 1000,
        [JULIA_MAIN_STATE_S1_STANDBY][JULIA_MAIN_STATE_S3_INITIATIVE] = 500,
        [JULIA_MAIN_STATE_S2_COMPANION][JULIA_MAIN_STATE_S1_STANDBY] = 600,
    };
    julia_main_state_t from_main = main_state_for(previous);
    julia_main_state_t to_main = main_state_for(state);
    const transition_script_t *script = transition_director_find(from_main, to_main);
    uint16_t transition_ms = durations[from_main][to_main];
    if (script) transition_ms = script->duration_ms;
    if (!transition_ms) transition_ms = from_main == to_main ? 250 : 400;
    if (from_main != to_main && !script) {
        avatar_motion_transition_main(to_main, transition_ms);
    }
    if (state != JULIA_SUB_STATE_S3_3_USER_CALL &&
        (state < JULIA_SUB_STATE_S4_1_LIGHT_DIALOG ||
         state > JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE)) {
        s_dialog_phase = JULIA_DIALOG_PHASE_IDLE;
        avatar_micro_motion_set_dialog_phase(JULIA_DIALOG_PHASE_IDLE);
    }
    avatar_micro_motion_set_state(state);
    s_state_entered_at = xTaskGetTickCount();
    lvgl_port_unlock();
    idle_player_exit();
    avatar_face_set_state((uint8_t)to_main);
    bool directed = false;
    if (from_main != to_main && transition_player_has(from_main, to_main)) {
        led_transition_to((led_state_t)to_main, transition_ms);
        directed = transition_player_play(from_main, to_main, streamed_transition_done,
                                           (void *)(uintptr_t)state) == ESP_OK;
    }
    if (!directed) {
        if (from_main != to_main) led_transition_to((led_state_t)to_main, transition_ms);
        transition_target_commit(state);
    }
    julia_display_theme_on_state_transition(state, transition_ms);
    ESP_LOGI(TAG, "animation state target=%d main=S%u from=S%u transition_ms=%u mode=%s",
             state, to_main, from_main, transition_ms, directed ? "trn-stream" : "direct");
}

static void state_worker_task(void *argument)
{
    (void)argument;
    julia_sub_state_t requested;
    while (xQueueReceive(s_state_queue, &requested, portMAX_DELAY) == pdTRUE) {
        /* 严格保持不同状态请求的 FIFO 顺序。只有与当前目标完全相同的
         * 连续重复请求会由 state_transition_apply() 快速忽略。 */
        state_transition_apply(requested);
    }
    vTaskDelete(NULL);
}

void julia_ui_set_state(julia_sub_state_t state)
{
    if (!julia_ui_showcase_allows_state_change()) return;
    if (state >= JULIA_SUB_STATE_COUNT || !s_state_queue) return;
    if (xQueueSend(s_state_queue, &state, 0) != pdTRUE)
        ESP_LOGW(TAG, "state request queue full; request=%d dropped", state);
}

void julia_ui_speak(const char *text)
{
    if (!s_ui.initialized || text == NULL || !lvgl_port_lock(portMAX_DELAY)) {
        return;
    }
    lv_label_set_text(s_ui.bubble_label, text);
    apply_expression(JULIA_EXPR_SPEAKING, 80);
    lvgl_port_unlock();
}

void julia_ui_apply_theme(uint32_t background_rgb, uint16_t transition_ms)
{
    if (!s_ui.initialized || !lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(background_rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    /* 人物自身继续由双画布渲染；主题背景变化只失效屏幕背景。 */
    lv_obj_invalidate(screen);
    lvgl_port_unlock();
    (void)transition_ms;
}

void julia_ui_breathing_anim(void)
{
    /* The dedicated breathing task owns the low-frequency idle animation. */
}

void julia_ui_talking_start(void)
{
    if (!s_ui.initialized || !s_panel || !lvgl_port_lock(pdMS_TO_TICKS(1000))) return;
    s_talking = true;
    avatar_motion_pause();
    lvgl_port_unlock();
    avatar_face_set_rms(0);
}

void julia_ui_set_mouth_level(uint8_t level)
{
    if (!s_talking || !s_panel || !lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    lvgl_port_unlock();
    static const uint16_t rms_for_level[] = {0, 30, 65, 95};
    avatar_face_set_rms(rms_for_level[level < 4 ? level : 3]);
}

void julia_ui_set_mouth_openness(uint16_t openness_q8)
{
    if (!s_talking || !s_panel || !lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    lvgl_port_unlock();
    uint16_t rms = openness_q8 < 256 ? 0 : openness_q8 < 512 ? 30 :
                   openness_q8 < 768 ? 65 : 95;
    avatar_face_set_rms(rms);
}

void julia_ui_talking_stop(void)
{
    if (!s_talking) return;
    s_talking = false;
    avatar_motion_resume();
    if (!s_panel || !lvgl_port_lock(pdMS_TO_TICKS(1000))) return;
    lvgl_port_unlock();
    avatar_face_set_rms(0);
}

void julia_ui_set_dialog_phase(julia_dialog_phase_t phase)
{
    if (!s_ui.initialized || phase == s_dialog_phase || !lvgl_port_lock(pdMS_TO_TICKS(300))) return;
    s_dialog_phase = phase;
    avatar_micro_motion_set_dialog_phase((uint8_t)phase);
    lvgl_port_unlock();
    if (!s_program_motion_mode) avatar_clip_map_set_dialog_phase((uint8_t)phase);
    julia_display_theme_on_interaction();
}

void julia_ui_present_rgb565_frame(const uint16_t *pixels, size_t pixel_count)
{
    if (!pixels || pixel_count != AVATAR_SIZE * AVATAR_SIZE || !s_avatar_pixels) return;
    memcpy(s_avatar_pixels, pixels, pixel_count * sizeof(uint16_t));
    if (s_ui.avatar_image) lv_obj_invalidate(s_ui.avatar_image);
}

void julia_ui_bind_rgb565_frame(uint16_t *pixels, size_t pixel_count)
{
    if (s_program_motion_mode && !s_transition_frame_mode) return;
    if (!pixels || pixel_count != AVATAR_SIZE * AVATAR_SIZE || !s_ui.stream_canvas) return;
    lv_canvas_set_buffer(s_ui.stream_canvas, pixels, AVATAR_SIZE, AVATAR_SIZE,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_style_opa(s_ui.stream_canvas, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.avatar_image, LV_OBJ_FLAG_HIDDEN);
    if (s_ui.stream_canvas_alt) lv_obj_add_flag(s_ui.stream_canvas_alt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.stream_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_ui.stream_canvas);
}

void julia_ui_crossfade_rgb565_frames(uint16_t *old_pixels, uint16_t *new_pixels,
                                      size_t pixel_count, uint8_t progress)
{
    if (s_program_motion_mode && !s_transition_frame_mode) return;
    if (!old_pixels || !new_pixels || pixel_count != AVATAR_SIZE * AVATAR_SIZE ||
        !s_ui.stream_canvas || !s_ui.stream_canvas_alt) return;
    lv_canvas_set_buffer(s_ui.stream_canvas, old_pixels, AVATAR_SIZE, AVATAR_SIZE,
                         LV_IMG_CF_TRUE_COLOR);
    lv_canvas_set_buffer(s_ui.stream_canvas_alt, new_pixels, AVATAR_SIZE, AVATAR_SIZE,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(s_ui.avatar_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.stream_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.stream_canvas_alt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_ui.stream_canvas, (lv_opa_t)(255U - progress), LV_PART_MAIN);
    lv_obj_set_style_opa(s_ui.stream_canvas_alt, (lv_opa_t)progress, LV_PART_MAIN);
    lv_obj_move_foreground(s_ui.stream_canvas_alt);
    lv_obj_invalidate(s_ui.stream_canvas);
    lv_obj_invalidate(s_ui.stream_canvas_alt);
}

void julia_ui_set_transition_frame_mode(bool enabled)
{
    if (!enabled && s_ui.initialized && lvgl_port_lock(pdMS_TO_TICKS(250))) {
        if (s_ui.stream_canvas) lv_obj_add_flag(s_ui.stream_canvas, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.stream_canvas_alt) lv_obj_add_flag(s_ui.stream_canvas_alt, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.avatar_image) lv_obj_clear_flag(s_ui.avatar_image, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.avatar_slot) lv_obj_invalidate(s_ui.avatar_slot);
        esp_err_t sync_err = lvgl_port_refr_now_sync(pdMS_TO_TICKS(1000));
        lvgl_port_unlock();
        if (sync_err != ESP_OK)
            ESP_LOGW(TAG, "transition static frame sync failed: %s", esp_err_to_name(sync_err));
    }
    s_transition_frame_mode = enabled;
}

void julia_ui_set_idle_frame_mode(bool enabled, julia_sub_state_t state)
{
    s_idle_frame_mode = enabled;
    s_idle_frame_state = enabled ? state : JULIA_SUB_STATE_COUNT;
}

esp_err_t julia_ui_transition_direct_begin(void)
{
    if (!s_ui.initialized) return ESP_ERR_INVALID_STATE;
    s_transition_frame_mode = true;
    avatar_face_set_transition_active(true);
    if (s_idle_frame_mode && s_idle_frame_state >= JULIA_SUB_STATE_S4_1_LIGHT_DIALOG &&
        s_idle_frame_state <= JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE) {
        avatar_mouth_set_transition_active(false);
        avatar_mouth_set_visible(true);
    }
    return ESP_OK;
}

esp_err_t julia_ui_transition_direct_draw(const uint16_t *pixels, size_t bytes,
                                          const char *source)
{
    (void)source;
    if (!pixels || bytes != AVATAR_SIZE * AVATAR_SIZE * sizeof(uint16_t) ||
        !lvgl_port_lock(pdMS_TO_TICKS(250))) return ESP_ERR_INVALID_ARG;
    julia_ui_bind_rgb565_frame((uint16_t *)pixels, AVATAR_SIZE * AVATAR_SIZE);
    esp_err_t err = lvgl_port_refr_now_sync(pdMS_TO_TICKS(1000));
    lvgl_port_unlock();
    return err;
}

esp_err_t julia_ui_transition_direct_end(void)
{
    if (!s_ui.initialized) return ESP_ERR_INVALID_STATE;
    avatar_face_set_transition_active(false);
    if (!s_idle_frame_mode) {
        s_idle_frame_state = JULIA_SUB_STATE_COUNT;
        julia_ui_set_transition_frame_mode(false);
    }
    return ESP_OK;
}

esp_err_t julia_ui_draw_standby_direct(esp_lcd_panel_handle_t panel)
{
    return draw_avatar_rows(panel, 0, 360);
}

esp_err_t julia_ui_draw_doze_frame(const char *path, bool *asset_loaded)
{
    if (asset_loaded) *asset_loaded = false;
    if (!s_panel || !path || !lvgl_port_lock(pdMS_TO_TICKS(1000)))
        return ESP_ERR_INVALID_STATE;
    bool loaded = path[0] != '\0' && s_doze_frame_loaded;
    esp_err_t err = s_doze_dma_rows ? ESP_OK : ESP_ERR_NO_MEM;
    for (int y = 0; y < 360 && err == ESP_OK; y += DOZE_DMA_ROWS) {
        int count = y + DOZE_DMA_ROWS <= 360 ? DOZE_DMA_ROWS : 360 - y;
        size_t bytes = 360U * (size_t)count * sizeof(lv_color_t);
        if (loaded) {
            memcpy(s_doze_dma_rows, &s_doze_frame[(size_t)y * 360U], bytes);
        } else {
            memset(s_doze_dma_rows, 0, bytes);
        }
        if (err == ESP_OK)
            err = lvgl_port_draw_bitmap_sync(s_panel, 0, y, 360, y + count, s_doze_dma_rows);
    }
    lvgl_port_unlock();
    if (asset_loaded) *asset_loaded = loaded && err == ESP_OK;
    ESP_LOGI(TAG, "Doze frame commit source=%s result=%s path=%s",
             loaded ? "sd-cache" : "black-fallback",
             esp_err_to_name(err), path);
    return err;
}

static esp_err_t draw_avatar_rows(esp_lcd_panel_handle_t panel, int y_start, int y_end)
{
    return draw_avatar_region(panel, 0, y_start, 360, y_end);
}

static esp_err_t draw_avatar_region(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                    int x_end, int y_end)
{
    if (!panel || !s_avatar_pixels) return ESP_ERR_INVALID_STATE;
    if (x_start < 0 || x_end > 360 || x_start >= x_end ||
        y_start < 0 || y_end > 360 || y_start >= y_end) return ESP_ERR_INVALID_ARG;
    s_panel = panel;
    if (!lvgl_port_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    const int rows_per_transfer = 12;
    const int width = x_end - x_start;
    lv_color_t *dma_rows = heap_caps_malloc(width * rows_per_transfer * sizeof(lv_color_t),
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!dma_rows) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    for (int y = y_start; y < y_end && err == ESP_OK; y += rows_per_transfer) {
        int rows = (y + rows_per_transfer <= y_end) ? rows_per_transfer : y_end - y;
        for (int row = 0; row < rows; ++row) {
            memcpy(&dma_rows[row * width],
                   &s_avatar_pixels[(size_t)(y + row) * 360 + x_start],
                   width * sizeof(lv_color_t));
        }
        err = lvgl_port_draw_bitmap_sync(panel, x_start, y, x_end, y + rows, dma_rows);
    }
    heap_caps_free(dma_rows);
    lvgl_port_unlock();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Standby portrait transfer failed: %s", esp_err_to_name(err));
    else if (x_start == 0 && x_end == 360 && y_start == 0 && y_end == 360)
        ESP_LOGI(TAG, "Standby portrait: 360/360 rows sent");
    return err;
}

lv_obj_t *julia_ui_get_avatar_slot(void)
{
    return s_ui.avatar_slot;
}

julia_sub_state_t julia_ui_current_state(void) { return s_current_state; }
