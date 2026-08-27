#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    AVATAR_EYES_OPEN = 0,
    AVATAR_EYES_HALF,
    AVATAR_EYES_CLOSED,
} avatar_eyes_frame_t;

void avatar_eyes_init(lv_obj_t *parent);
void avatar_eyes_set_state(uint8_t main_state);
void avatar_eyes_set_transition_active(bool active);
void avatar_eyes_set_visible(bool visible);
void avatar_eyes_show(avatar_eyes_frame_t frame);
void avatar_eyes_correct_pupils_rgb565(uint16_t *pixels, uint16_t width, uint16_t height);
bool avatar_eyes_has_green_blob_rgb565(const uint16_t *pixels, uint16_t width,
                                       uint16_t height);
lv_obj_t *avatar_eyes_left_object(void);
lv_obj_t *avatar_eyes_right_object(void);
