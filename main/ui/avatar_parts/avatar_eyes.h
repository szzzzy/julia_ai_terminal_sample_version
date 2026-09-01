/**
 * @file    avatar_eyes.h
 * @brief   眼睛部件接口：三态帧切换 + 周期性眨眼任务 + 显隐 + 瞳孔检测。
 *
 * 眼睛三态（avatar_eyes_frame_t）对应三张生成资源：OPEN/HALF/CLOSED。
 * 周期性眨眼由本模块启用的独立任务（blink_task）驱动，调度权在本模块；
 * 瞳孔的“随动/微动”则交给上层 avatar_micro_motion 处理（本模块只暴露眼睛对象句柄）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/* 眼睛帧：三态。0=睁眼、1=半闭、2=全闭。 */
typedef enum {
    AVATAR_EYES_OPEN = 0,
    AVATAR_EYES_HALF,
    AVATAR_EYES_CLOSED,
} avatar_eyes_frame_t;

/* 创建左右眼对象并启动眨眼任务。前置：parent 为有效容器，未重复初始化。 */
void avatar_eyes_init(lv_obj_t *parent);
/* 跟随主状态：记录状态、递增“代”使进行中的眨眼失效，并复位为睁眼（转场除外）。 */
void avatar_eyes_set_state(uint8_t main_state);
/* 转场开关：转场期间隐藏眼睛并冻结眨眼。 */
void avatar_eyes_set_transition_active(bool active);
/* 直接显示/隐藏左右眼（不涉及帧切换）。 */
void avatar_eyes_set_visible(bool visible);
/* 立即把左右眼切到指定帧（内部取 LVGL 锁）。 */
void avatar_eyes_show(avatar_eyes_frame_t frame);
/** 持续保持闭眼，直到调用方显式解除；期间周期眨眼任务不会恢复睁眼。 */
void avatar_eyes_set_idle_closed(bool closed);
/* 将 360×360 RGB565 帧内的“绿色瞳孔”像素重着色为金铜色（眼部特征处理）。 */
void avatar_eyes_correct_pupils_rgb565(uint16_t *pixels, uint16_t width, uint16_t height);
/* 检测帧内是否存在超出预期区域、且呈现特定绿色块（用于校验/调试）。 */
bool avatar_eyes_has_green_blob_rgb565(const uint16_t *pixels, uint16_t width,
                                       uint16_t height);
/* 左右眼对象句柄（供上层绑定给微动引擎）。 */
lv_obj_t *avatar_eyes_left_object(void);
lv_obj_t *avatar_eyes_right_object(void);
