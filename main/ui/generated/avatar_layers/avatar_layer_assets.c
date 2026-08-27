#include "avatar_layer_assets.h"

extern const uint8_t eye_left_open_bin_start[] asm("_binary_eye_left_open_bin_start");
extern const uint8_t eye_left_open_bin_end[] asm("_binary_eye_left_open_bin_end");
static const lv_img_dsc_t eye_left_open_dsc = {
  .header.always_zero=0, .header.w=55, .header.h=58,
  .header.cf=LV_IMG_CF_TRUE_COLOR,
  .data_size=55*58*2, .data=eye_left_open_bin_start,
};

static const avatar_layer_asset_info_t eye_left_open_info = {"eye_left_open", 55, 58, 110,
  LV_IMG_CF_TRUE_COLOR, 6380, eye_left_open_bin_start, eye_left_open_bin_end};

extern const uint8_t eye_left_half_bin_start[] asm("_binary_eye_left_half_bin_start");
extern const uint8_t eye_left_half_bin_end[] asm("_binary_eye_left_half_bin_end");
static const lv_img_dsc_t eye_left_half_dsc = {
  .header.always_zero=0, .header.w=55, .header.h=58,
  .header.cf=LV_IMG_CF_TRUE_COLOR,
  .data_size=55*58*2, .data=eye_left_half_bin_start,
};

static const avatar_layer_asset_info_t eye_left_half_info = {"eye_left_half", 55, 58, 110,
  LV_IMG_CF_TRUE_COLOR, 6380, eye_left_half_bin_start, eye_left_half_bin_end};

extern const uint8_t eye_left_closed_bin_start[] asm("_binary_eye_left_closed_bin_start");
extern const uint8_t eye_left_closed_bin_end[] asm("_binary_eye_left_closed_bin_end");
static const lv_img_dsc_t eye_left_closed_dsc = {
  .header.always_zero=0, .header.w=55, .header.h=58,
  .header.cf=LV_IMG_CF_TRUE_COLOR,
  .data_size=55*58*2, .data=eye_left_closed_bin_start,
};

static const avatar_layer_asset_info_t eye_left_closed_info = {"eye_left_closed", 55, 58, 110,
  LV_IMG_CF_TRUE_COLOR, 6380, eye_left_closed_bin_start, eye_left_closed_bin_end};

extern const uint8_t avatar_pupil_left_bin_start[] asm("_binary_avatar_pupil_left_bin_start");
extern const uint8_t avatar_pupil_left_bin_end[] asm("_binary_avatar_pupil_left_bin_end");
static const lv_img_dsc_t avatar_pupil_left_dsc = {
  .header.always_zero=0, .header.w=27, .header.h=32,
  .header.cf=LV_IMG_CF_TRUE_COLOR_ALPHA,
  .data_size=27*32*3, .data=avatar_pupil_left_bin_start,
};

static const avatar_layer_asset_info_t avatar_pupil_left_info = {"avatar_pupil_left", 27, 32, 81,
  LV_IMG_CF_TRUE_COLOR_ALPHA, 2592, avatar_pupil_left_bin_start, avatar_pupil_left_bin_end};

extern const uint8_t eye_right_open_bin_start[] asm("_binary_eye_right_open_bin_start");
extern const uint8_t eye_right_open_bin_end[] asm("_binary_eye_right_open_bin_end");
static const lv_img_dsc_t eye_right_open_dsc = {
  .header.always_zero=0, .header.w=55, .header.h=58,
  .header.cf=LV_IMG_CF_TRUE_COLOR,
  .data_size=55*58*2, .data=eye_right_open_bin_start,
};

static const avatar_layer_asset_info_t eye_right_open_info = {"eye_right_open", 55, 58, 110,
  LV_IMG_CF_TRUE_COLOR, 6380, eye_right_open_bin_start, eye_right_open_bin_end};

extern const uint8_t eye_right_half_bin_start[] asm("_binary_eye_right_half_bin_start");
extern const uint8_t eye_right_half_bin_end[] asm("_binary_eye_right_half_bin_end");
static const lv_img_dsc_t eye_right_half_dsc = {
  .header.always_zero=0, .header.w=55, .header.h=58,
  .header.cf=LV_IMG_CF_TRUE_COLOR,
  .data_size=55*58*2, .data=eye_right_half_bin_start,
};

static const avatar_layer_asset_info_t eye_right_half_info = {"eye_right_half", 55, 58, 110,
  LV_IMG_CF_TRUE_COLOR, 6380, eye_right_half_bin_start, eye_right_half_bin_end};

extern const uint8_t eye_right_closed_bin_start[] asm("_binary_eye_right_closed_bin_start");
extern const uint8_t eye_right_closed_bin_end[] asm("_binary_eye_right_closed_bin_end");
static const lv_img_dsc_t eye_right_closed_dsc = {
  .header.always_zero=0, .header.w=55, .header.h=58,
  .header.cf=LV_IMG_CF_TRUE_COLOR,
  .data_size=55*58*2, .data=eye_right_closed_bin_start,
};

static const avatar_layer_asset_info_t eye_right_closed_info = {"eye_right_closed", 55, 58, 110,
  LV_IMG_CF_TRUE_COLOR, 6380, eye_right_closed_bin_start, eye_right_closed_bin_end};

extern const uint8_t avatar_pupil_right_bin_start[] asm("_binary_avatar_pupil_right_bin_start");
extern const uint8_t avatar_pupil_right_bin_end[] asm("_binary_avatar_pupil_right_bin_end");
static const lv_img_dsc_t avatar_pupil_right_dsc = {
  .header.always_zero=0, .header.w=27, .header.h=32,
  .header.cf=LV_IMG_CF_TRUE_COLOR_ALPHA,
  .data_size=27*32*3, .data=avatar_pupil_right_bin_start,
};

static const avatar_layer_asset_info_t avatar_pupil_right_info = {"avatar_pupil_right", 27, 32, 81,
  LV_IMG_CF_TRUE_COLOR_ALPHA, 2592, avatar_pupil_right_bin_start, avatar_pupil_right_bin_end};

extern const uint8_t mouth_closed_bin_start[] asm("_binary_mouth_closed_bin_start");
extern const uint8_t mouth_closed_bin_end[] asm("_binary_mouth_closed_bin_end");
static const lv_img_dsc_t mouth_closed_dsc = {
  .header.always_zero=0, .header.w=54, .header.h=34,
  .header.cf=LV_IMG_CF_TRUE_COLOR,
  .data_size=54*34*2, .data=mouth_closed_bin_start,
};

static const avatar_layer_asset_info_t mouth_closed_info = {"mouth_closed", 54, 34, 108,
  LV_IMG_CF_TRUE_COLOR, 3672, mouth_closed_bin_start, mouth_closed_bin_end};

extern const uint8_t mouth_half_bin_start[] asm("_binary_mouth_half_bin_start");
extern const uint8_t mouth_half_bin_end[] asm("_binary_mouth_half_bin_end");
static const lv_img_dsc_t mouth_half_dsc = {
  .header.always_zero=0, .header.w=54, .header.h=34,
  .header.cf=LV_IMG_CF_TRUE_COLOR,
  .data_size=54*34*2, .data=mouth_half_bin_start,
};

static const avatar_layer_asset_info_t mouth_half_info = {"mouth_half", 54, 34, 108,
  LV_IMG_CF_TRUE_COLOR, 3672, mouth_half_bin_start, mouth_half_bin_end};

extern const uint8_t mouth_open_bin_start[] asm("_binary_mouth_open_bin_start");
extern const uint8_t mouth_open_bin_end[] asm("_binary_mouth_open_bin_end");
static const lv_img_dsc_t mouth_open_dsc = {
  .header.always_zero=0, .header.w=54, .header.h=34,
  .header.cf=LV_IMG_CF_TRUE_COLOR,
  .data_size=54*34*2, .data=mouth_open_bin_start,
};

static const avatar_layer_asset_info_t mouth_open_info = {"mouth_open", 54, 34, 108,
  LV_IMG_CF_TRUE_COLOR, 3672, mouth_open_bin_start, mouth_open_bin_end};

extern const uint8_t hair_tip_bin_start[] asm("_binary_hair_tip_bin_start");
extern const uint8_t hair_tip_bin_end[] asm("_binary_hair_tip_bin_end");
static const lv_img_dsc_t hair_tip_dsc = {
  .header.always_zero=0, .header.w=34, .header.h=52,
  .header.cf=LV_IMG_CF_TRUE_COLOR_ALPHA,
  .data_size=34*52*3, .data=hair_tip_bin_start,
};

static const avatar_layer_asset_info_t hair_tip_info = {"hair_tip", 34, 52, 102,
  LV_IMG_CF_TRUE_COLOR_ALPHA, 5304, hair_tip_bin_start, hair_tip_bin_end};

const lv_img_dsc_t *avatar_layer_eye(bool left, uint8_t frame) {
  static const lv_img_dsc_t *e[2][3]={{&eye_left_open_dsc,&eye_left_half_dsc,&eye_left_closed_dsc},{&eye_right_open_dsc,&eye_right_half_dsc,&eye_right_closed_dsc}};
  return e[left?0:1][frame<3?frame:0];
}
const lv_img_dsc_t *avatar_layer_pupil(bool left) { return left?&avatar_pupil_left_dsc:&avatar_pupil_right_dsc; }
const lv_img_dsc_t *avatar_layer_mouth(avatar_mouth_frame_t frame) {
  static const lv_img_dsc_t *m[3]={&mouth_closed_dsc,&mouth_half_dsc,&mouth_open_dsc}; return m[frame<3?frame:2];
}
const lv_img_dsc_t *avatar_layer_hair_tip(void) { return &hair_tip_dsc; }

const avatar_layer_asset_info_t *avatar_layer_asset_info(const lv_img_dsc_t *descriptor) {
  static const struct { const lv_img_dsc_t *d; const avatar_layer_asset_info_t *i; } assets[] = {
    {&eye_left_open_dsc, &eye_left_open_info},
    {&eye_left_half_dsc, &eye_left_half_info},
    {&eye_left_closed_dsc, &eye_left_closed_info},
    {&avatar_pupil_left_dsc, &avatar_pupil_left_info},
    {&eye_right_open_dsc, &eye_right_open_info},
    {&eye_right_half_dsc, &eye_right_half_info},
    {&eye_right_closed_dsc, &eye_right_closed_info},
    {&avatar_pupil_right_dsc, &avatar_pupil_right_info},
    {&mouth_closed_dsc, &mouth_closed_info},
    {&mouth_half_dsc, &mouth_half_info},
    {&mouth_open_dsc, &mouth_open_info},
    {&hair_tip_dsc, &hair_tip_info},
  };
  for (unsigned i=0; i<sizeof(assets)/sizeof(assets[0]); ++i) if (assets[i].d==descriptor) return assets[i].i;
  return NULL;
}
bool avatar_layer_asset_valid(const lv_img_dsc_t *descriptor) {
  const avatar_layer_asset_info_t *i=avatar_layer_asset_info(descriptor);
  return i && descriptor->data && descriptor->data==i->data_start &&
         descriptor->header.w==i->width && descriptor->header.h==i->height &&
         descriptor->header.cf==i->color_format && descriptor->data_size==i->data_size &&
         (uint32_t)(i->data_end-i->data_start)==i->data_size;
}
