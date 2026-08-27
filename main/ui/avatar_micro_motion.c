#include "avatar_micro_motion.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "lvgl_port.h"
#include "avatar_layer_assets.h"
#include "julia_display_theme.h"

#define UPDATE_MS 40U
#define MOTION_LOG_MS 1000U
#define MOTION_TAG "MOTION"
#define EVENT_TAG "AVATAR_EVENT"
#define Q10_ONE 1024
#define DISPLAY_STABILITY_HOTFIX 1
#ifndef AVATAR_LAYER_DEBUG
#define AVATAR_LAYER_DEBUG 0
#endif

typedef enum { GAZE_CENTER, GAZE_OUT, GAZE_HOLD, GAZE_BACK } gaze_stage_t;
typedef struct { lv_coord_t x; lv_coord_t y; } origin_t;

static struct {
    avatar_layer_bindings_t layer;
    origin_t le, re, lp, rp, mouth, hair;
    julia_sub_state_t state;
    uint8_t phase;
    uint8_t external_pause_count;
    uint32_t last_update;
    uint32_t last_log;
    uint64_t last_flush_count;
    uint32_t flush_fps;
    uint32_t next_gaze, gaze_started, gaze_duration;
    uint32_t next_nod, nod_started;
    uint32_t next_think_gaze;
    uint32_t next_rare, rare_started;
    size_t minimum_free_psram;
    int8_t gaze_x, gaze_y;
    gaze_stage_t gaze_stage;
    bool initialized, suspended, nodding, rare_hair;
    bool breathe_running, head_running, neck_running;
    int16_t pose_head_angle, pose_neck_angle;
    uint16_t pose_zoom;
    uint8_t pose_breath_amplitude;
    uint8_t left_eye_frame, right_eye_frame;
    julia_main_state_t transition_target;
} s;
static volatile bool s_all_paused;

static const lv_img_dsc_t *valid_layer_or_fallback(const char *name,
                                                    const lv_img_dsc_t *candidate,
                                                    const lv_img_dsc_t *fallback)
{
    if (avatar_layer_asset_valid(candidate)) return candidate;
    ESP_LOGW("AVATAR_LAYER", "%s frame invalid data=%p; falling back", name,
             candidate ? candidate->data : NULL);
    return avatar_layer_asset_valid(fallback) ? fallback : NULL;
}

static void reset_image_transform(lv_obj_t *image)
{
    if (!image) return;
    lv_anim_del(image, NULL);
    lv_obj_set_style_transform_angle(image, 0, 0);
    lv_obj_set_style_transform_zoom(image, 256, 0);
    lv_obj_set_style_transform_pivot_x(image, 0, 0);
    lv_obj_set_style_transform_pivot_y(image, 0, 0);
    lv_img_set_angle(image, 0);
    lv_img_set_zoom(image, 256);
    lv_img_set_pivot(image, 0, 0);
}

static void reset_stability_transforms(void)
{
    lv_obj_t *containers[] = {s.layer.container, s.layer.neck, s.layer.head};
    for (unsigned i = 0; i < sizeof(containers) / sizeof(containers[0]); ++i) {
        if (!containers[i]) continue;
        lv_obj_set_style_transform_angle(containers[i], 0, 0);
        lv_obj_set_style_transform_zoom(containers[i], 256, 0);
        lv_obj_set_style_transform_pivot_x(containers[i], 0, 0);
        lv_obj_set_style_transform_pivot_y(containers[i], 0, 0);
    }
    reset_image_transform(s.layer.left_eye);
    reset_image_transform(s.layer.right_eye);
    reset_image_transform(s.layer.left_pupil);
    reset_image_transform(s.layer.right_pupil);
    reset_image_transform(s.layer.mouth);
}

static uint32_t random_between(uint32_t low, uint32_t high)
{
    if (high <= low) return low;
    return low + esp_random() % (high - low + 1U);
}

static origin_t origin(lv_obj_t *obj)
{
    origin_t result = {0, 0};
    if (obj) { result.x = lv_obj_get_x(obj); result.y = lv_obj_get_y(obj); }
    return result;
}

static int32_t clamp_q10(uint32_t elapsed, uint32_t duration)
{
    if (!duration || elapsed >= duration) return Q10_ONE;
    return (int32_t)(elapsed * Q10_ONE / duration);
}

static int32_t ease_in_out_q10(int32_t t)
{
    if (t <= 0) return 0;
    if (t >= Q10_ONE) return Q10_ONE;
    if (t < Q10_ONE / 2) return (2 * t * t) / Q10_ONE;
    int32_t inverse = Q10_ONE - t;
    return Q10_ONE - (2 * inverse * inverse) / Q10_ONE;
}

static int32_t ease_out_q10(int32_t t)
{
    int32_t inverse = Q10_ONE - t;
    return Q10_ONE - (inverse * inverse) / Q10_ONE;
}

static void move(lv_obj_t *obj, origin_t at, int32_t dx_q10, int32_t dy_q10)
{
    if (!obj) return;
    lv_coord_t x = at.x + (lv_coord_t)(dx_q10 / Q10_ONE);
    lv_coord_t y = at.y + (lv_coord_t)(dy_q10 / Q10_ONE);
    if (lv_obj_get_x(obj) != x || lv_obj_get_y(obj) != y) lv_obj_set_pos(obj, x, y);
}

static bool sleeping(void) { return s.state <= JULIA_SUB_STATE_S0_3_MANUAL_SLEEP; }
static bool idle_state(void)
{
    return s.state >= JULIA_SUB_STATE_S1_1_NEAR_STANDBY &&
           s.state <= JULIA_SUB_STATE_S1_3_CHARGING_STANDBY && s.phase == 0;
}
static bool listening(void) { return s.phase == 1; }
static bool high_priority(void) { return s.external_pause_count || s.nodding || s.phase == 3; }
static bool low_motion_allowed(void)
{
    return s.initialized && !s_all_paused && !s.suspended && !high_priority() &&
           (idle_state() || listening());
}

static const char *active_action(void)
{
    if (s_all_paused || s.suspended) return "suspended";
    if (s.phase == 3) return "speak";
    if (s.nodding) return "nod";
    if (s.external_pause_count) return "blink";
    if (s.gaze_stage != GAZE_CENTER) return "scan";
    return idle_state() || listening() ? "breathe" : "still";
}

static void log_memory(const char *edge, const char *function)
{
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (!s.minimum_free_psram || psram < s.minimum_free_psram) s.minimum_free_psram = psram;
    ESP_LOGI(MOTION_TAG, "%s=%s action=%s heap=%u psram=%u largest=%u flush_fps=%u",
             edge, function, active_action(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)psram,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)s.flush_fps);
}

static void set_transform_zoom(void *object, int32_t value)
{
    lv_obj_t *container = object;
    if (!container) return;
    int32_t zoom = s.pose_zoom ? s.pose_zoom : 256;
    if (low_motion_allowed()) {
        int32_t delta = (value - 256) * s.pose_breath_amplitude / 4;
        zoom += listening() ? delta / 2 : delta;
    }
    lv_obj_set_style_transform_zoom(container, (lv_coord_t)zoom, 0);
}

static void set_head_angle(void *object, int32_t value)
{
    lv_obj_t *container = object;
    if (container) lv_obj_set_style_transform_angle(container,
        (lv_coord_t)(s.pose_head_angle + (low_motion_allowed() ? value : 0)), 0);
}

static void set_neck_angle(void *object, int32_t value)
{
    lv_obj_t *container = object;
    if (container) lv_obj_set_style_transform_angle(container,
        (lv_coord_t)(s.pose_neck_angle + (low_motion_allowed() ? value : 0)), 0);
}

static void start_repeating_anim(lv_obj_t *object, lv_anim_exec_xcb_t callback,
                                 int32_t from, int32_t to, uint32_t out_ms,
                                 uint32_t back_ms, uint32_t delay_ms)
{
    if (!object || !callback) return;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, callback);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_time(&animation, out_ms);
    lv_anim_set_playback_time(&animation, back_ms);
    lv_anim_set_delay(&animation, delay_ms);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_start(&animation);
}

void avatar_breathe_init(lv_obj_t *container)
{
    log_memory("enter", __func__);
    s.layer.container = container;
    if (container) {
        lv_obj_set_style_transform_pivot_x(container, 180, 0);
        lv_obj_set_style_transform_pivot_y(container, 270, 0);
        start_repeating_anim(container, set_transform_zoom, 256, 260, 1500,
                             random_between(1500, 2500), 0);
        s.breathe_running = true;
    }
    log_memory("exit", __func__);
}

void avatar_head_init(lv_obj_t *container)
{
    log_memory("enter", __func__);
    s.layer.head = container;
    if (container) {
        lv_obj_set_style_transform_pivot_x(container, 180, 0);
        lv_obj_set_style_transform_pivot_y(container, 225, 0);
        start_repeating_anim(container, set_head_angle, -20, 20, 2500, 2500, 0);
        s.head_running = true;
    }
    log_memory("exit", __func__);
}

void avatar_neck_init(lv_obj_t *container)
{
    log_memory("enter", __func__);
    s.layer.neck = container;
    if (container) {
        lv_obj_set_style_transform_pivot_x(container, 180, 0);
        lv_obj_set_style_transform_pivot_y(container, 245, 0);
        start_repeating_anim(container, set_neck_angle, -10, 10, 2500, 2500, 500);
        s.neck_running = true;
    }
    log_memory("exit", __func__);
}

void avatar_motion_pause(void)
{
    if (s.external_pause_count < UINT8_MAX) ++s.external_pause_count;
}

void avatar_motion_resume(void)
{
    if (s.external_pause_count) --s.external_pause_count;
}

static void schedule_for_state(uint32_t now)
{
    s.gaze_stage=GAZE_CENTER; s.gaze_x=0; s.gaze_y=0;
    s.next_gaze=now+random_between(2500,6000);
    s.next_nod=now+random_between(2000,3500);
    s.next_think_gaze=now+random_between(4000,7000);
    s.next_rare=now+random_between(15000,40000);
    s.nodding=false; s.rare_hair=false;
}

void avatar_micro_motion_init(const avatar_layer_bindings_t *layers)
{
    memset(&s,0,sizeof(s));
    s_all_paused = true;
    if (layers) s.layer=*layers;
    s.le=origin(s.layer.left_eye); s.re=origin(s.layer.right_eye);
    s.lp=origin(s.layer.left_pupil); s.rp=origin(s.layer.right_pupil);
    s.mouth=origin(s.layer.mouth); s.hair=origin(s.layer.hair_front);
    s.state=JULIA_SUB_STATE_S1_1_NEAR_STANDBY; s.initialized=true; s.suspended=true;
    s.pose_zoom=256; s.pose_breath_amplitude=4;
    s.left_eye_frame = s.right_eye_frame = UINT8_MAX;
    schedule_for_state(0);
#if DISPLAY_STABILITY_HOTFIX
    reset_stability_transforms();
#endif
    log_memory("enter", __func__);
    ESP_LOGI(MOTION_TAG, "initialized paused; waiting for boot reveal");
    log_memory("exit", __func__);
}

void avatar_micro_motion_set_state(julia_sub_state_t state)
{
    log_memory("enter", __func__);
    if (state < JULIA_SUB_STATE_COUNT) s.state=state;
    schedule_for_state(s.last_update);
    ESP_LOGI(EVENT_TAG,"t=%lu state=%d transition_ms=300",(unsigned long)s.last_update,state);
    log_memory("exit", __func__);
}

void avatar_micro_motion_set_dialog_phase(uint8_t phase)
{
    log_memory("enter", __func__);
    if (phase <= 3) s.phase=phase;
    schedule_for_state(s.last_update);
    ESP_LOGI(EVENT_TAG,"t=%lu phase=%u transition_ms=300",(unsigned long)s.last_update,phase);
    log_memory("exit", __func__);
}

void avatar_micro_motion_suspend(bool suspended) { s.suspended=suspended; }
void avatar_motion_pause_all(void)
{
    s_all_paused = true;
    s.suspended = true;
    if (s.layer.container) lv_anim_del(s.layer.container, set_transform_zoom);
    if (s.layer.neck) lv_anim_del(s.layer.neck, set_neck_angle);
    if (s.layer.head) lv_anim_del(s.layer.head, set_head_angle);
    s.breathe_running = s.head_running = s.neck_running = false;
    ESP_LOGI(MOTION_TAG, "all animations paused");
}

void avatar_motion_resume_all(void)
{
    if (!s_all_paused) return;
#if DISPLAY_STABILITY_HOTFIX
    reset_stability_transforms();
    s_all_paused = true;
    s.suspended = true;
    s.breathe_running = s.head_running = s.neck_running = false;
    ESP_LOGI(MOTION_TAG, "display stability mode: continuous avatar motion remains paused");
    return;
#else
    s_all_paused = false;
    s.suspended = false;
#if !DISPLAY_STABILITY_HOTFIX
    if (s.layer.container) {
        start_repeating_anim(s.layer.container, set_transform_zoom, 256, 260,
                             1500, random_between(1500, 2500), 0);
        s.breathe_running = true;
    }
    if (s.layer.neck) {
        start_repeating_anim(s.layer.neck, set_neck_angle, -10, 10, 2500, 2500, 500);
        s.neck_running = true;
    }
    if (s.layer.head) {
        start_repeating_anim(s.layer.head, set_head_angle, -20, 20, 2500, 2500, 0);
        s.head_running = true;
    }
#endif
    schedule_for_state(s.last_update);
    ESP_LOGI(MOTION_TAG, "all animations resumed");
#endif
}

bool avatar_motion_all_paused(void) { return s_all_paused; }

static __attribute__((unused)) void transition_head(void *object, int32_t value)
{ if (object) lv_obj_set_style_transform_angle(object, (lv_coord_t)value, 0); }
static __attribute__((unused)) void transition_neck(void *object, int32_t value)
{ if (object) lv_obj_set_style_transform_angle(object, (lv_coord_t)value, 0); }
static __attribute__((unused)) void transition_zoom(void *object, int32_t value)
{ if (object) lv_obj_set_style_transform_zoom(object, (lv_coord_t)value, 0); }
static void transition_opa(void *object, int32_t value)
{ if (object) lv_obj_set_style_opa(object, (lv_opa_t)value, 0); }

static void transition_anim(lv_obj_t *object, lv_anim_exec_xcb_t callback,
                            int32_t from, int32_t to, uint16_t duration_ms,
                            lv_anim_ready_cb_t ready)
{
    if (!object) return;
    lv_anim_del(object, callback);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, callback);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_time(&animation, duration_ms);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    if (ready) lv_anim_set_ready_cb(&animation, ready);
    lv_anim_start(&animation);
}

static void main_transition_ready(lv_anim_t *animation)
{
    (void)animation;
    if (s.transition_target >= JULIA_MAIN_STATE_S1_STANDBY &&
        s.transition_target <= JULIA_MAIN_STATE_S4_DIALOG) avatar_motion_resume_all();
    ESP_LOGI(EVENT_TAG, "main transition complete target=S%u", s.transition_target);
}

void avatar_motion_transition_main(julia_main_state_t state, uint16_t duration_ms)
{
    static const int16_t head[] = {-150, 0, 30, 80, 0, -50};
    static const int16_t neck[] = {-50, 0, 10, 30, 0, -20};
    static const uint16_t zoom[] = {256, 256, 256, 258, 256, 256};
    static const uint8_t eyes[] = {0, 255, 255, 255, 255, 0};
    static const uint8_t amplitude[] = {0, 4, 2, 2, 2, 0};
    if (!s.initialized || state >= JULIA_MAIN_STATE_COUNT) return;
    if (!duration_ms) duration_ms = 1;
    int32_t from_head = s.layer.head ? lv_obj_get_style_transform_angle(s.layer.head, 0) : 0;
    int32_t from_neck = s.layer.neck ? lv_obj_get_style_transform_angle(s.layer.neck, 0) : 0;
    int32_t from_zoom = s.layer.container ? lv_obj_get_style_transform_zoom(s.layer.container, 0) : 256;
    int32_t from_opa = s.layer.left_eye ? lv_obj_get_style_opa(s.layer.left_eye, 0) : 255;
    avatar_motion_pause_all();
    s.transition_target = state;
    s.pose_head_angle = head[state]; s.pose_neck_angle = neck[state];
    s.pose_zoom = zoom[state]; s.pose_breath_amplitude = amplitude[state];
#if !DISPLAY_STABILITY_HOTFIX
    transition_anim(s.layer.container, transition_zoom, from_zoom, zoom[state], duration_ms, NULL);
    transition_anim(s.layer.neck, transition_neck, from_neck, neck[state], duration_ms, NULL);
#else
    (void)from_neck;
    (void)from_zoom;
#endif
    transition_anim(s.layer.left_eye, transition_opa, from_opa, eyes[state], duration_ms,
                    main_transition_ready);
    transition_anim(s.layer.right_eye, transition_opa, from_opa, eyes[state], duration_ms, NULL);
    transition_anim(s.layer.left_pupil, transition_opa, from_opa, eyes[state], duration_ms, NULL);
    transition_anim(s.layer.right_pupil, transition_opa, from_opa, eyes[state], duration_ms, NULL);
    const lv_img_dsc_t *mouth_default = avatar_layer_mouth(AVATAR_MOUTH_CLOSED);
    const lv_img_dsc_t *mouth_source = valid_layer_or_fallback("mouth",
        avatar_layer_mouth(
            state == JULIA_MAIN_STATE_S3_INITIATIVE ? AVATAR_MOUTH_HALF : AVATAR_MOUTH_CLOSED),
        mouth_default);
    if (s.layer.mouth && mouth_source && state != JULIA_MAIN_STATE_S4_DIALOG)
        lv_img_set_src(s.layer.mouth, mouth_source);
#if !DISPLAY_STABILITY_HOTFIX
    transition_anim(s.layer.head, transition_head, from_head, head[state], duration_ms, NULL);
#else
    (void)from_head;
#endif
    ESP_LOGI(EVENT_TAG, "main transition target=S%u duration=%u head=%d neck=%d eyes=%u zoom=%u",
             state, duration_ms, head[state], neck[state], eyes[state], zoom[state]);
}
void on_user_interaction(void)
{
    schedule_for_state(s.last_update);
    reset_idle_timer();
}

static const avatar_motion_config_t motion_config={
    .blink_rate_x100=1500,.gaze_hold_min_ms=800,.gaze_hold_max_ms=2000,
    .breath_period_ms=4000,.breath_amplitude_px=1,.enabled=1
};
const avatar_motion_config_t *avatar_micro_motion_config(julia_sub_state_t state)
{ (void)state; return &motion_config; }

static void update_gaze(uint32_t now, int32_t *gx, int32_t *gy)
{
    if (s.gaze_stage==GAZE_CENTER && now>=s.next_gaze) {
        do { s.gaze_x=(int8_t)((int)(esp_random()%11U)-5); s.gaze_y=(int8_t)((int)(esp_random()%9U)-4); }
        while (abs(s.gaze_x)<3 && abs(s.gaze_y)<3);
        s.gaze_stage=GAZE_OUT; s.gaze_started=now; s.gaze_duration=180;
        ESP_LOGI(EVENT_TAG,"t=%lu gaze start x=%d y=%d",(unsigned long)now,s.gaze_x,s.gaze_y);
    }
    uint32_t elapsed=now-s.gaze_started;
    int32_t t=clamp_q10(elapsed,s.gaze_duration);
    if (s.gaze_stage==GAZE_OUT) {
        int32_t eased=ease_out_q10(t); *gx=s.gaze_x*eased; *gy=s.gaze_y*eased;
        if (t==Q10_ONE) { s.gaze_stage=GAZE_HOLD; s.gaze_started=now; s.gaze_duration=random_between(800,2000); }
    } else if (s.gaze_stage==GAZE_HOLD) {
        *gx=s.gaze_x*Q10_ONE; *gy=s.gaze_y*Q10_ONE;
        if (elapsed>=s.gaze_duration) { s.gaze_stage=GAZE_BACK; s.gaze_started=now; s.gaze_duration=420; }
    } else if (s.gaze_stage==GAZE_BACK) {
        int32_t inverse=Q10_ONE-ease_in_out_q10(t); *gx=s.gaze_x*inverse; *gy=s.gaze_y*inverse;
        if (t==Q10_ONE) { s.gaze_stage=GAZE_CENTER; s.next_gaze=now+random_between(2500,6000); ESP_LOGI(EVENT_TAG,"t=%lu gaze center",(unsigned long)now); }
    }
}

static void set_eye_frame(bool left, uint8_t frame, lv_obj_t *object)
{
    uint8_t *current = left ? &s.left_eye_frame : &s.right_eye_frame;
    if (*current == frame) return;
    const lv_img_dsc_t *source=valid_layer_or_fallback(left ? "eye_left" : "eye_right",
        avatar_layer_eye(left,frame), avatar_layer_eye(left,0));
    if (object && source) { lv_img_set_src(object,source); *current=frame; }
}

void update_avatar(uint32_t now)
{
    esp_task_wdt_reset();
    if (!s.initialized || s_all_paused || s.suspended || now-s.last_update<UPDATE_MS) return;
    s.last_update=now;
    if (now-s.last_log>=MOTION_LOG_MS) {
        uint32_t elapsed=now-s.last_log;
        uint64_t flush_count=0;
        lvgl_port_get_flush_metrics(&flush_count,NULL,NULL);
        if(s.last_flush_count && elapsed) s.flush_fps=(uint32_t)((flush_count-s.last_flush_count)*1000ULL/elapsed);
        s.last_flush_count=flush_count; s.last_log=now;
        log_memory("tick",__func__);
    }
    int32_t gx=0,gy=0,feature_y=0;
    if (sleeping()) {
        set_eye_frame(true,2,s.layer.left_eye); set_eye_frame(false,2,s.layer.right_eye);
        if(s.layer.left_pupil)lv_obj_add_flag(s.layer.left_pupil,LV_OBJ_FLAG_HIDDEN);
        if(s.layer.right_pupil)lv_obj_add_flag(s.layer.right_pupil,LV_OBJ_FLAG_HIDDEN);
    } else if (s.phase==0) {
        if (!high_priority()) {
            set_eye_frame(true,0,s.layer.left_eye); set_eye_frame(false,0,s.layer.right_eye);
        }
        if(s.layer.left_pupil)lv_obj_clear_flag(s.layer.left_pupil,LV_OBJ_FLAG_HIDDEN);
        if(s.layer.right_pupil)lv_obj_clear_flag(s.layer.right_pupil,LV_OBJ_FLAG_HIDDEN);
        if(!high_priority()) update_gaze(now,&gx,&gy);
    } else if (s.phase==1) {
        if(!s.nodding && now>=s.next_nod) { s.nodding=true; s.nod_started=now; ESP_LOGI(EVENT_TAG,"t=%lu nod start",(unsigned long)now); }
        if(s.nodding) {
            uint32_t elapsed=now-s.nod_started;
            int32_t t=clamp_q10(elapsed,700);
            feature_y=(t<225 ? (4*Q10_ONE*ease_out_q10(t*Q10_ONE/225))/Q10_ONE :
                       (4*Q10_ONE*(Q10_ONE-ease_in_out_q10((t-225)*Q10_ONE/799)))/Q10_ONE);
            if(t==Q10_ONE){s.nodding=false;s.next_nod=now+random_between(2000,3500);ESP_LOGI(EVENT_TAG,"t=%lu nod center",(unsigned long)now);}
        }
    } else if(s.phase==2) {
        if(now>=s.next_think_gaze){s.gaze_x=(esp_random()&1U)?4:-4;s.gaze_y=-4;s.next_think_gaze=now+random_between(4000,7000);ESP_LOGI(EVENT_TAG,"t=%lu think gaze x=%d y=-4",(unsigned long)now,s.gaze_x);}
        gx=s.gaze_x*Q10_ONE;gy=s.gaze_y*Q10_ONE;
    }
    if(!lvgl_port_lock(pdMS_TO_TICKS(4))) return;
    move(s.layer.left_pupil,s.lp,gx,gy+feature_y); move(s.layer.right_pupil,s.rp,gx,gy+feature_y);
    move(s.layer.left_eye,s.le,0,feature_y); move(s.layer.right_eye,s.re,0,feature_y);
    move(s.layer.mouth,s.mouth,0,feature_y);
    lvgl_port_unlock();
}

#ifdef AVATAR_DEBUG
void avatar_motion_debug_trigger(uint8_t action)
{
    if(action==0)s.next_gaze=s.last_update;
    else if(action==1)s.next_nod=s.last_update;
    else if(action==2)s.next_rare=s.last_update;
}
void avatar_motion_debug_print(void)
{
    ESP_LOGI(MOTION_TAG,"state=%d phase=%u action=%s breathe=%d head=%d neck=%d paused=%u",
             s.state,s.phase,active_action(),s.breathe_running,s.head_running,s.neck_running,s.external_pause_count);
}
void avatar_motion_debug_print_psram_peak(void)
{
    ESP_LOGI(MOTION_TAG,"psram_min=%u free=%u largest=%u",(unsigned)s.minimum_free_psram,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
#endif
