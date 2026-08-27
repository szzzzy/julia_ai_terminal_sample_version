#include "julia_ui_assets.h"
#include "avatar_face_base.h"

/* Static states share the validated opaque RGB565 portrait. Motion clips and
 * local eye/mouth layers provide state-specific expression without retaining
 * the obsolete 2.5 MiB PNG bundle in the OTA partition. */
const lv_img_dsc_t *julia_ui_reference_asset(void)
{
    return &avatar_asset_julia_s1_1_near_standby;
}

const lv_img_dsc_t *julia_ui_asset_for_state(julia_sub_state_t state)
{
    (void)state;
    return &avatar_asset_julia_s1_1_near_standby;
}
