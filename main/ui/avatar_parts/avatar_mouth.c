/**
 * @file    avatar_mouth.c
 * @brief   嘴部部件实现：四档嘴型切换、RMS 档位换算与显隐控制。
 *
 * 模块边界：
 *   - 资源：嘴型用生成的 avatar_chroma_assets（mouth_closed/half/open/speak3），由 source_for()
 *     按档位映射；这是 L0/L1 “RMS→嘴型”的最终渲染落点。
 *   - 档位语义：IDLE(≤15)、SPEAK1(≤50)、SPEAK2(≤80)、SPEAK3(>80)。阈值为经验值，
 *     用于把连续 RMS 量化成 4 阶开口，避免逐帧抖动。
 *   - 本模块不做笑/抿嘴等表情，也不负责立绘整屏动画（那是 julia_ui.c / avatar_micro_motion.c）。
 *
 * 线程模型：公开 API 由上层（julia_ui / micro_motion / 演示任务）调用；set_shape 内部取
 * lvgl_port_lock(100ms)，超时则放弃本次切换。除 s_shape/s_transition_active 外无跨线程共享态。
 */
#include "avatar_mouth.h"

#include "avatar_chroma_assets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl_port.h"

#ifndef JULIA_AVATAR_LOG
#define JULIA_AVATAR_LOG 0
#endif

/* ---- 模块级状态 ---- */
static lv_obj_t *s_mouth;                      /* 嘴部对象。 */
static avatar_mouth_shape_t s_shape = AVATAR_MOUTH_IDLE; /* 当前生效的档位（用于去重）。 */
static bool s_transition_active;               /* 转场中：冻结嘴型切换并隐藏。 */

/* 档位→资源映射：IDLE=closed、SPEAK1=half、SPEAK2=open、SPEAK3=speak3。
 * 越界档位回退到 speak3（最大开口，保守可见）。 */
static const lv_img_dsc_t *source_for(avatar_mouth_shape_t shape)
{
    if (shape == AVATAR_MOUTH_IDLE) return &avatar_asset_mouth_closed;
    if (shape == AVATAR_MOUTH_SPEAK1) return &avatar_asset_mouth_half;
    if (shape == AVATAR_MOUTH_SPEAK2) return &avatar_asset_mouth_open;
    return &avatar_asset_mouth_speak3;
}

/* 创建嘴部对象并置初始闭口。前置：parent 有效、尚未初始化（重复调用直接返回）。 */
void avatar_mouth_init(lv_obj_t *parent)
{
    if (!parent || s_mouth) return;
    s_mouth = lv_img_create(parent);
    lv_img_set_src(s_mouth, source_for(AVATAR_MOUTH_IDLE));
    lv_obj_set_pos(s_mouth, 153, 177);
    lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_SCROLLABLE);
}

/* 直接切到指定档位。前置：对象已建、非转场。首遇条件：档位与当前不同才重设 src。
 * 副作用：内部取 lvgl_port_lock(100ms)；超时则放弃本次切换。
 * 失败路径：对象未建/转场中/锁超时 → 直接返回；刷新耗时 >12ms 打慢刷警告。 */
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

/* RMS→档位：按阈值 15/50/80 把连续 RMS 量化成 4 阶开口，再交给 set_shape。
 * 这是语音下行（julia_ui_set_mouth_openness 等）驱动嘴型的换算入口。 */
void avatar_mouth_set_rms(uint16_t rms)
{
    avatar_mouth_shape_t shape = rms <= 15 ? AVATAR_MOUTH_IDLE :
                                 rms <= 50 ? AVATAR_MOUTH_SPEAK1 :
                                 rms <= 80 ? AVATAR_MOUTH_SPEAK2 : AVATAR_MOUTH_SPEAK3;
    avatar_mouth_set_shape(shape, rms);
}

/* 转场开关：转场期间隐藏嘴部并冻结切换；结束时先强制 s_shape=SPEAK3 以绕过“同档去重”，
 * 再把档位复位到 IDLE（闭口）并恢复可见。 */
void avatar_mouth_set_transition_active(bool active)
{
    s_transition_active = active;
    avatar_mouth_set_visible(!active);
    if (!active) {
        s_shape = AVATAR_MOUTH_SPEAK3;
        avatar_mouth_set_shape(AVATAR_MOUTH_IDLE, 0);
    }
}

/* 直接显示/隐藏嘴部。内部取 lvgl_port_lock(100ms)，超时则放弃。 */
void avatar_mouth_set_visible(bool visible)
{
    if (!s_mouth || !lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    bool currently_visible = !lv_obj_has_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
    if (visible != currently_visible) {
        if (visible) lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_mouth);
    }
    lvgl_port_unlock();
}

/* 嘴部句柄泄露给上层（用于微动引擎绑定/转场时联动）。 */
lv_obj_t *avatar_mouth_object(void) { return s_mouth; }
