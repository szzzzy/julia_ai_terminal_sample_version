/**
 * @file    avatar_face.h
 * @brief   “脸部部件”总控接口：立绘底图 + 眼睛 + 嘴部的生命周期与状态入口。
 *
 * 层级：本模块是部件层（avatar_parts）的“面向脸部”聚合面，向上服务于总控 julia_ui.c，
 * 向下组合 avatar_eyes / avatar_mouth / 生成的底图资源（avatar_face_base / avatar_face_doze）。
 * 它创建“立绘底图”并持有眼睛/嘴部对象句柄，供上层绑定给微动引擎（avatar_micro_motion）。
 *
 * 职责边界：只管“部件存在、底图/眼/口在某个状态呈现”，不包含 FSM、表情选择与微动逻辑；
 * 也自建一个演示任务（模拟说话/换状态）用于无语音时的自检。所有 LVGL 写操作均在
 * lvgl_port 锁内完成（见各实现函数）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

/* 创建底图与眼睛/嘴部对象（父级 parent 之下）。前置：LVGL 已初始化、parent 为有效容器。 */
void avatar_face_init(lv_obj_t *parent);
/* 按主状态（JULIA_MAIN_STATE_*）切换眼睛状态并显示底图；若正在 doze 则不显示底图。 */
void avatar_face_set_state(uint8_t main_state);
/* 转场期间调用：隐藏全部部件层（active=true）或恢复（active=false），并联动眼睛/嘴部。 */
void avatar_face_set_transition_active(bool active);
/* 切换底图到睡眠/待机立绘并隐藏/恢复眼睛/嘴部。返回 ESP_OK，LVGL 锁超时返回 ESP_ERR_TIMEOUT。 */
esp_err_t avatar_face_set_doze(bool active);
/* 查询是否处于 doze（睡眠立绘）状态。 */
bool avatar_face_is_dozing(void);
/* 直接把 RMS 值转发给嘴部部件（嘴型档位→资源切换在 avatar_mouth.c 内完成）。 */
void avatar_face_set_rms(uint16_t rms);
/* 标记一次用户活动：刷新最近活动时间并退出演示模式。 */
void avatar_face_note_activity(void);
/* 允许/禁止演示任务：禁用时复位嘴型为关闭。 */
void avatar_face_demo_set_enabled(bool enabled);
/* 查询演示模式当前是否激活。 */
bool avatar_face_demo_enabled(void);
/* 记录按钮按压状态：按下刷新活动时间，松开复位嘴型。 */
void avatar_face_button_set_pressed(bool pressed);
/* 供上层绑定给微动引擎的眼睛/嘴部对象句柄。 */
lv_obj_t *avatar_face_left_eye(void);
lv_obj_t *avatar_face_right_eye(void);
lv_obj_t *avatar_face_mouth(void);
/* 底图对象句柄。 */
lv_obj_t *avatar_face_base_object(void);
