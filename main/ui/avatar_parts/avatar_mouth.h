/**
 * @file    avatar_mouth.h
 * @brief   嘴部部件接口：四档嘴型切换 + RMS 档位映射 + 显隐。
 *
 * 嘴型四档（avatar_mouth_shape_t）：IDLE(闭)、SPEAK1(半开)、SPEAK2(开)、SPEAK3(更大开)。
 * 资源映射在 avatar_mouth.c 的 source_for()：每档对应一张生成的 mouth_* 资源。
 * RMS→档位的换算由 set_rms 完成（档位阈值见 .c）。这是 L0/L1 的“RMS 嘴型”核心落点。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/* 嘴型档位。0=闭、1=半开、2=开、3=最大开。 */
typedef enum {
    AVATAR_MOUTH_IDLE = 0,
    AVATAR_MOUTH_SPEAK1,
    AVATAR_MOUTH_SPEAK2,
    AVATAR_MOUTH_SPEAK3,
} avatar_mouth_shape_t;

/* 创建嘴部对象（初始=闭口）。前置：parent 为有效容器、未重复初始化。 */
void avatar_mouth_init(lv_obj_t *parent);
/* 按 RMS 值换算成档位后切嘴型（无额外锁定，只用 set_shape 内的锁）。 */
void avatar_mouth_set_rms(uint16_t rms);
/* 直接切到指定档位并携带 RMS 值用于日志；转场中或档位未变则忽略。 */
void avatar_mouth_set_shape(avatar_mouth_shape_t shape, uint16_t rms);
/* 转场开关：转场期间隐藏嘴部；结束恢复并强制回到闭口。 */
void avatar_mouth_set_transition_active(bool active);
/* 直接显示/隐藏嘴部（不切换档位）。 */
void avatar_mouth_set_visible(bool visible);
/* 嘴部对象句柄（供上层绑定给微动引擎）。 */
lv_obj_t *avatar_mouth_object(void);
