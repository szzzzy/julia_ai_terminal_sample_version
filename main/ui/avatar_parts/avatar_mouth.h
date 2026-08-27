#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    AVATAR_MOUTH_IDLE = 0,
    AVATAR_MOUTH_SPEAK1,
    AVATAR_MOUTH_SPEAK2,
    AVATAR_MOUTH_SPEAK3,
} avatar_mouth_shape_t;

void avatar_mouth_init(lv_obj_t *parent);
void avatar_mouth_set_rms(uint16_t rms);
void avatar_mouth_set_shape(avatar_mouth_shape_t shape, uint16_t rms);
void avatar_mouth_set_transition_active(bool active);
void avatar_mouth_set_visible(bool visible);
lv_obj_t *avatar_mouth_object(void);
