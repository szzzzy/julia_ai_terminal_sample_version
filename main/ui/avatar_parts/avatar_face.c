/**
 * @file    avatar_face.c
 * @brief   “脸部部件”实现：立绘底图 + 眼睛 + 嘴部的创建与状态切换。
 *
 * 模块边界：
 *   - 生成的对口资源：底图用 avatar_asset_julia_s1_1_near_standby（待机）/ 
 *     avatar_asset_julia_s0_1_night_sleep（睡眠）；眼睛/嘴部对象由 avatar_eyes_init /
 *     avatar_mouth_init 创建，本文件仅保存它们的句柄并转发生成部分调用。
 *   - 本模块不包含 FSM 状态机与微动逻辑（那些在 julia_ui.c / avatar_micro_motion.c）。
 *   - 另有一个演示任务（simulation_task）：30s 无活动自动进入演示，模拟说话嘴型与状态轮播，
 *     仅用于无真实语音/按键时的自检；长按按钮则进入固定的 3 档 RMS 口型演示。
 *
 * 线程模型：
 *   - 公开 API 由 julia_ui 的调用方线程持 lvgl_port 锁执行（例如 state_worker_task）。
 *   - simulation_task 是独立 FreeRTOS 任务（PSRAM 栈），每 100ms 刷新一次，只调用
 *     avatar_mouth_set_rms / julia_ui_set_state（后者投递到异步状态队列，不直接操作 LVGL）。
 *   - 共享的 s_last_activity_us / s_demo_* / s_button_* 均为 volatile，模拟任务与公开 API
 *     在锁上下文之外读，靠单字节/整型原子性保证不撕裂。
 */
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

/* ---- 模块级状态 ---- */
static volatile int64_t s_last_activity_us;    /* 最近一次用户活动时刻（us），用于演示超时判定。 */
static volatile bool s_demo_allowed = true;    /* 是否允许进入演示（可被 demo_enabled 关闭）。 */
static volatile bool s_demo_active;            /* 当前是否正在演示。 */
static volatile bool s_button_pressed;         /* 演示按钮是否按住。 */
static volatile int64_t s_button_started_us;   /* 按钮按下的时刻（us），用于长按判定。 */
static lv_obj_t *s_base;                       /* 立绘底图对象。 */
static volatile bool s_dozing;                 /* 是否处于睡眠立绘。 */
static volatile uint8_t s_main_state = 1;      /* 当前主状态（初始化默认 S1 待机）。 */

/* 演示任务：100ms 一拍。
 *   - 按住按钮超过 600ms → 进入“长按口型”演示：随机切换 3 档 RMS；
 *   - 否则，若 30s 无活动且被允许 → 进入演示：按固定 RMS 序列模拟说话、每 5s 轮播一个子状态，
 *     用于无人交互时自检立绘与口型链路。不触碰 FSM，也不操作 LVGL 对象（仿真只调受控接口）。 */
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

/* 初始化眼/口部件并创建演示任务。前置：LVGL 已初始化，parent 为有效容器。
 * 副作用：创建底图与眼睛/嘴部对象（初始 src=待机底图/开眼/闭口），启动演示任务。
 * 失败路径：无硬失败；仅当演示任务创建失败时打错误日志（不影响立绘）。 */
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

/* 记录主状态并连同眼睛一起切换；doze 时不显示底图（避免盖在被隐藏的部件上）。 */
void avatar_face_set_state(uint8_t main_state)
{
    s_main_state = main_state;
    avatar_eyes_set_state(main_state);
    if (s_base && !s_dozing) {
        lv_obj_clear_flag(s_base, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 转场开关：active=true 期间隐藏底图与所有部件（避免转场过程中旧层闪现），
 * active=false 再一并恢复。眼睛/嘴部各自管理可见性，此处只需切换底图。 */
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

/* 切换到睡眠/待机立绘。
 * 前置：模块已 init。副作用：替换底图 src、隐藏/恢复眼睛与嘴部并立即sync刷新一次。
 * 失败路径：未创建底图或拿到 LVGL 锁超时（250ms）→ 返回 ESP_ERR_TIMEOUT；否则返回刷新错误码。
 * 注意：整帧同步刷新（lvgl_port_refr_now_sync）在调用方上下文阻塞，直到提交完成或超时。 */
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

/* 转发 RMS 到嘴部部件；档位到资源切换在 avatar_mouth_set_rms 内完成。 */
void avatar_face_set_rms(uint16_t rms) { avatar_mouth_set_rms(rms); }

/* 记一次活动并退出演示。 */
void avatar_face_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    s_demo_active = false;
}

/* 开关演示：启用时立即进入演示；禁用时退出并把嘴型复位到关闭。 */
void avatar_face_demo_set_enabled(bool enabled)
{
    s_demo_allowed = enabled;
    s_demo_active = enabled;
    if (!enabled) avatar_mouth_set_rms(0);
}

bool avatar_face_demo_enabled(void) { return s_demo_active; }

/* 记录按钮状态：按下即视为一次活动并记录起始时刻（供长按判定）；松开复位嘴型。 */
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
