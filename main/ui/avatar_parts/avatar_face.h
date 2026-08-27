#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

void avatar_face_init(lv_obj_t *parent);
void avatar_face_set_state(uint8_t main_state);
void avatar_face_set_transition_active(bool active);
esp_err_t avatar_face_set_doze(bool active);
bool avatar_face_is_dozing(void);
void avatar_face_set_rms(uint16_t rms);
void avatar_face_note_activity(void);
void avatar_face_demo_set_enabled(bool enabled);
bool avatar_face_demo_enabled(void);
void avatar_face_button_set_pressed(bool pressed);
lv_obj_t *avatar_face_left_eye(void);
lv_obj_t *avatar_face_right_eye(void);
lv_obj_t *avatar_face_mouth(void);
lv_obj_t *avatar_face_base_object(void);
