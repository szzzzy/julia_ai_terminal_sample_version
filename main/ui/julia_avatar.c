#include "julia_avatar.h"

#include <stdbool.h>
#include <stdint.h>

#include "avatar_rle.h"
#include "esp_crc.h"
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
#define AVATAR_FRAME_WIDTH         360U
#define AVATAR_FRAME_HEIGHT        360U
#define AVATAR_FRAME_PIXELS        (AVATAR_FRAME_WIDTH * AVATAR_FRAME_HEIGHT)
#define AVATAR_FRAME_BYTES         (AVATAR_FRAME_PIXELS * sizeof(uint16_t))

/* Transforming the 360x360 root invalidates the complete display on every
 * animation tick.  On the QSPI panel that frame is committed in ten strips,
 * without a TE signal to keep the writes outside the LCD scanout window.  The
 * result is continuous visible tearing/flicker.  Keep animation updates local
 * to the eyes and mouth until panel-synchronised full-frame rendering exists. */
#define AVATAR_ENABLE_FULL_FRAME_MOTION 0

static const char *TAG = "julia_avatar";
static lv_obj_t *s_motion_root;
static lv_obj_t *s_base;
static volatile bool s_ready;
static bool s_talking;
static uint32_t s_smoothed_rms;
static uint8_t s_mouth_level;
static uint8_t s_target_mouth_level;
static uint32_t s_last_pcm_ms;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static julia_avatar_dialog_phase_t s_dialog_phase = JULIA_AVATAR_DIALOG_IDLE;
/* Last phase successfully assigned to the LVGL base image.  It is separate
 * from the requested state so a lock timeout can be retried safely. */
static julia_avatar_dialog_phase_t s_applied_dialog_phase =
    (julia_avatar_dialog_phase_t)(JULIA_AVATAR_DIALOG_SPEAKING + 1);
static portMUX_TYPE s_phase_lock = portMUX_INITIALIZER_UNLOCKED;

extern const uint8_t LISTEN_bin_start[] asm("_binary_LISTEN_bin_start");
extern const uint8_t LISTEN_bin_end[] asm("_binary_LISTEN_bin_end");
extern const uint8_t THINK_bin_start[] asm("_binary_THINK_bin_start");
extern const uint8_t THINK_bin_end[] asm("_binary_THINK_bin_end");
extern const uint8_t SPEAK_bin_start[] asm("_binary_SPEAK_bin_start");
extern const uint8_t SPEAK_bin_end[] asm("_binary_SPEAK_bin_end");

typedef struct {
    const char *name;
    const uint8_t *compressed_start;
    const uint8_t *compressed_end;
    uint32_t expected_crc;
    uint16_t *pixels;
    bool decoded;
    bool loading;
    lv_img_dsc_t image;
} avatar_phase_frame_t;

static avatar_phase_frame_t s_phase_frames[] = {
    [JULIA_AVATAR_DIALOG_LISTENING - 1] = {
        .name = "LISTEN", .compressed_start = LISTEN_bin_start, .compressed_end = LISTEN_bin_end,
        .expected_crc = 0x122fda66U,
    },
    [JULIA_AVATAR_DIALOG_THINKING - 1] = {
        .name = "THINK", .compressed_start = THINK_bin_start, .compressed_end = THINK_bin_end,
        .expected_crc = 0x26dc6a30U,
    },
    [JULIA_AVATAR_DIALOG_SPEAKING - 1] = {
        .name = "SPEAK", .compressed_start = SPEAK_bin_start, .compressed_end = SPEAK_bin_end,
        .expected_crc = 0xe8a17787U,
    },
};

static const char *dialog_phase_name(julia_avatar_dialog_phase_t phase)
{
    switch (phase) {
    case JULIA_AVATAR_DIALOG_IDLE: return "IDLE";
    case JULIA_AVATAR_DIALOG_LISTENING: return "LISTENING";
    case JULIA_AVATAR_DIALOG_THINKING: return "THINKING";
    case JULIA_AVATAR_DIALOG_SPEAKING: return "SPEAKING";
    default: return "INVALID";
    }
}

static bool dialog_phase_is_current(julia_avatar_dialog_phase_t phase)
{
    bool current;
    portENTER_CRITICAL(&s_phase_lock);
    current = s_dialog_phase == phase;
    portEXIT_CRITICAL(&s_phase_lock);
    return current;
}

static bool avatar_phase_frame_ensure(avatar_phase_frame_t *frame)
{
    if (frame == NULL) return false;

    portENTER_CRITICAL(&s_phase_lock);
    bool ready = frame->decoded;
    bool loading = frame->loading;
    if (!ready && !loading) frame->loading = true;
    portEXIT_CRITICAL(&s_phase_lock);
    if (ready) return true;
    if (loading) return false;

    uint16_t *pixels = heap_caps_malloc(AVATAR_FRAME_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = pixels == NULL ? ESP_ERR_NO_MEM :
                    avatar_rle_decode_rgb565(frame->compressed_start,
                                              (size_t)(frame->compressed_end - frame->compressed_start),
                                              pixels, AVATAR_FRAME_PIXELS);
    if (err == ESP_OK) {
        uint32_t crc = esp_crc32_le(0, (const uint8_t *)pixels, AVATAR_FRAME_BYTES);
        if (crc != frame->expected_crc) {
            ESP_LOGE(TAG, "%s frame CRC mismatch: got=%08lx expected=%08lx", frame->name,
                     (unsigned long)crc, (unsigned long)frame->expected_crc);
            err = ESP_ERR_INVALID_CRC;
        }
    }

    portENTER_CRITICAL(&s_phase_lock);
    frame->loading = false;
    if (err == ESP_OK) {
        frame->pixels = pixels;
        frame->decoded = true;
        frame->image.header.always_zero = 0;
        frame->image.header.w = AVATAR_FRAME_WIDTH;
        frame->image.header.h = AVATAR_FRAME_HEIGHT;
        frame->image.header.cf = LV_IMG_CF_TRUE_COLOR;
        frame->image.data_size = AVATAR_FRAME_BYTES;
        frame->image.data = (const uint8_t *)pixels;
    }
    portEXIT_CRITICAL(&s_phase_lock);

    if (err != ESP_OK) {
        heap_caps_free(pixels);
        ESP_LOGW(TAG, "%s phase frame unavailable (%s); using base portrait", frame->name,
                 esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "%s phase frame decoded into PSRAM (%u bytes)", frame->name,
             (unsigned)AVATAR_FRAME_BYTES);
    return true;
}

static const lv_img_dsc_t *avatar_source_for_phase(julia_avatar_dialog_phase_t phase)
{
    if (phase == JULIA_AVATAR_DIALOG_IDLE ||
        phase < JULIA_AVATAR_DIALOG_IDLE || phase > JULIA_AVATAR_DIALOG_SPEAKING) {
        return &avatar_asset_julia_s1_1_near_standby;
    }
    avatar_phase_frame_t *frame = &s_phase_frames[phase - 1];
    return avatar_phase_frame_ensure(frame) ? &frame->image : &avatar_asset_julia_s1_1_near_standby;
}

static bool avatar_apply_dialog_phase(julia_avatar_dialog_phase_t phase)
{
    const lv_img_dsc_t *source = avatar_source_for_phase(phase);
    /* Decoding can take long enough for a later transport command to supersede
     * this request. Never let a stale request overwrite the latest phase. */
    if (!s_base || !dialog_phase_is_current(phase)) return false;
    if (!lvgl_port_lock(pdMS_TO_TICKS(250))) {
        ESP_LOGW(TAG, "LVGL lock timeout applying %s phase", dialog_phase_name(phase));
        return false;
    }
    bool applied = false;
    if (dialog_phase_is_current(phase)) {
        lv_img_set_src(s_base, source);
        lv_obj_invalidate(s_base);
        portENTER_CRITICAL(&s_phase_lock);
        if (s_dialog_phase == phase) {
            s_applied_dialog_phase = phase;
            applied = true;
        }
        portEXIT_CRITICAL(&s_phase_lock);
    }
    lvgl_port_unlock();
    return applied;
}

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
    /* Close immediately on SPKE/session teardown rather than waiting for the
     * next 40 ms lip-sync tick. */
    if (s_ready) avatar_mouth_set_shape(AVATAR_MOUTH_IDLE, 0);
}

void julia_avatar_set_dialog_phase(julia_avatar_dialog_phase_t phase)
{
    if (phase < JULIA_AVATAR_DIALOG_IDLE || phase > JULIA_AVATAR_DIALOG_SPEAKING) {
        ESP_LOGW(TAG, "Ignoring invalid UI phase %d", (int)phase);
        return;
    }
    julia_avatar_dialog_phase_t previous;
    bool needs_apply;
    portENTER_CRITICAL(&s_phase_lock);
    previous = s_dialog_phase;
    if (previous != phase) s_dialog_phase = phase;
    needs_apply = previous != phase || s_applied_dialog_phase != phase;
    portEXIT_CRITICAL(&s_phase_lock);

    if (previous != phase) {
        ESP_LOGI(TAG, "UI phase: %s -> %s", dialog_phase_name(previous), dialog_phase_name(phase));
    }
    if (needs_apply) avatar_apply_dialog_phase(phase);
}

julia_avatar_dialog_phase_t julia_avatar_get_dialog_phase(void)
{
    julia_avatar_dialog_phase_t phase;
    portENTER_CRITICAL(&s_phase_lock);
    phase = s_dialog_phase;
    portEXIT_CRITICAL(&s_phase_lock);
    return phase;
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

    s_base = lv_img_create(s_motion_root);
    lv_img_set_src(s_base, &avatar_asset_julia_s1_1_near_standby);
    lv_obj_set_pos(s_base, 0, 0);
    lv_obj_clear_flag(s_base, LV_OBJ_FLAG_SCROLLABLE);

    avatar_eyes_init(s_motion_root);
    avatar_mouth_init(s_motion_root);
    lv_obj_invalidate(screen);
    lvgl_port_unlock();

    /* A phase can be requested before display initialisation; honour it once
     * the one and only avatar object tree exists. */
    avatar_apply_dialog_phase(julia_avatar_get_dialog_phase());

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
