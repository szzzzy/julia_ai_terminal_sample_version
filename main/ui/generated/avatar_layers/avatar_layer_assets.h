#pragma once
#include "lvgl.h"
typedef enum { AVATAR_MOUTH_CLOSED, AVATAR_MOUTH_HALF, AVATAR_MOUTH_OPEN } avatar_mouth_frame_t;
const lv_img_dsc_t *avatar_layer_eye(bool left, uint8_t frame);
const lv_img_dsc_t *avatar_layer_pupil(bool left);
const lv_img_dsc_t *avatar_layer_mouth(avatar_mouth_frame_t frame);
const lv_img_dsc_t *avatar_layer_hair_tip(void);
typedef struct {
    const char *name;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    lv_img_cf_t color_format;
    uint32_t data_size;
    const uint8_t *data_start;
    const uint8_t *data_end;
} avatar_layer_asset_info_t;
const avatar_layer_asset_info_t *avatar_layer_asset_info(const lv_img_dsc_t *descriptor);
bool avatar_layer_asset_valid(const lv_img_dsc_t *descriptor);
