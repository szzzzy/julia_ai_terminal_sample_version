#include "julia_rig_assets.h"

#define RIG_ASSET(name, width, height) \
    extern const uint8_t name##_bin_start[] asm("_binary_" #name "_bin_start"); \
    extern const uint8_t name##_bin_end[] asm("_binary_" #name "_bin_end"); \
    static const lv_img_dsc_t name##_dsc = { \
        .header.always_zero = 0, .header.w = width, .header.h = height, \
        .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA, \
        .data_size = width * height * 3, .data = name##_bin_start, \
    }; \
    const lv_img_dsc_t *julia_rig_##name(void) { \
        return &name##_dsc; \
    }

RIG_ASSET(body, 250, 166)
RIG_ASSET(hair_back, 234, 240)
RIG_ASSET(face, 154, 210)
RIG_ASSET(hair_front, 242, 226)
RIG_ASSET(eye_left, 51, 51)
RIG_ASSET(eye_right, 51, 51)
RIG_ASSET(pupil_left, 30, 36)
RIG_ASSET(pupil_right, 30, 36)
RIG_ASSET(mouth, 38, 13)

extern const uint8_t composite_rgb565_bin_start[]
    asm("_binary_composite_rgb565_bin_start");
static const lv_img_dsc_t composite_dsc = {
    .header.always_zero = 0,
    .header.w = 360,
    .header.h = 360,
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data_size = 360 * 360 * 2,
    .data = composite_rgb565_bin_start,
};
const lv_img_dsc_t *julia_rig_composite(void) { return &composite_dsc; }
