/**
 * @file    julia_ui.c
 * @brief   Julia 立绘 UI 总控（fused 工程移植而来，L2/L3 尚未裁剪的遗留实现）。
 *
 * 模块职责与边界：
 *   - 本模块是"调度/总控"层：负责 LVGL 资源与立绘的创建（julia_ui_init）、
 *     FSM 子状态 → 表情/画面的切换（julia_ui_set_state）、嘴型开合
 *     （talking_start / set_mouth_openness / talking_stop）、对话框相位
 *     （IDLE/LISTENING/THINKING/SPEAKING）、doze 帧提交（julia_ui_draw_doze_frame）、
 *     L0 简单主题（julia_ui_apply_theme）以及 RGB565 直接呈现路径
 *     （present / bind / crossfade / transition_direct_*）。
 *   - 它不做底层渲染：立绘部件的逐层摆放/眨眼/呼吸由 avatar_parts 与
 *     avatar_micro_motion 承担；背光由 julia_backlight 承担；显示功率/主题策略由
 *     julia_display_theme（或当前生效的 app/julia_idle_display.c）承担。
 *
 * 移植与构建状态（重要，非 bug）：
 *   - main/CMakeLists.txt 的 srcs 并未纳入本文件，也未纳入 julia_display_theme.c
 *     与 avatar_parts/avatar_face.c。本文件 #include 的 julia_ui_showcase.h、
 *     avatar_anim_engine.h、avatar_clip_map.h、transition_director.h、
 *     transition_player.h、idle_player.h 在 base 工程中并不存在
 *     （见 docs/UI_L0L1_PORT.md §2），因此本文件目前无法独立编译。
 *     这属于"预期中的待裁剪"，不要误判为链接错误或 bug。
 *     julia_display_theme.h 与 breathing_led.h 在 base 中存在（但 julia_display_theme
 *     的实现与呼吸灯模块未纳入当前构建），因此这两处 include 本身可解析。
 *   - 运行时真正生效的 L1 立绘链路在 app_main 里走 julia_display_init() +
 *     julia_avatar_init() + julia_idle_display_init()，并不会调用 julia_ui_init()。
 *     NOTE：需结合调用方确认——本文件（及 julia_ui.h）是否仍被其它模块引用，
 *     以决定是彻底删除还是完成 L2/L3 裁剪后重新纳入构建。
 *
 * 线程模型：
 *   - LVGL 对象只能在持有 lvgl_port_lock() 的临界区内操作；lvgl_port 内部有
 *     一个 "lvgl" 任务（优先级 5）每 10ms 调用 lv_timer_handler()，另有一个
 *     esp_timer 每 2ms 推进 lv_tick_inc()。
 *   - 本模块另起一个 "avatar_state" 任务（优先级 3，栈 8192）从 s_state_queue
 *     取状态请求并串行执行 state_transition_apply()，确保"状态迁移"只发生在一个
 *     线程，避免多处同时改 LVGL 对象。
 *   - voice_service / WSS 等任务调用 talking/mouth 接口时同样必须经
 *     lvgl_port_lock()，因此接口内部统一以 lvgl_port_lock 为并发边界。
 *
 * 数据流：
 *   - 立绘：julia_ui_asset_for_state() 返回的 bin/PNG → load_image_source() 解码
 *     写入 s_avatar_pixels（PSRAM，360x360 RGB565）→ lv_canvas_set_buffer() 绑定
 *     avatar_image 画布 → LVGL 刷新到 LCD。
 *   - RGB565 直接路径：外部帧 → julia_ui_bind/crossfade_rgb565_frames() 绑定到
 *     stream_canvas / stream_canvas_alt，绕过静态画布，用于流式/转场帧。
 *   - doze 帧：/sdcard/julia/doze_frame.bin 预加载到 s_doze_frame（PSRAM），
 *     julia_ui_draw_doze_frame() 在暂停 LVGL 刷新期间按行 DMA 直写 LCD。
 */
#include "julia_ui.h"
#include "julia_ui_showcase.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl_port.h"
#include "julia_ui_assets.h"
#include "julia_blink_assets.h" /* Legacy declarations; dead full-frame helpers are link-GC'd. */
#include "avatar_micro_motion.h"

/* 以下 L2/L3 头文件为 fused 移植残留：avatar_anim_engine（表情/转场动画引擎）、
 * avatar_clip_map（相位剪辑映射）、transition_director / transition_player（转场导演/
 * 播放器）、idle_player（待机播放）、julia_display_theme（显示主题）、breathing_led
 * （呼吸灯）。其中前 6 个模块在 base 工程中**不存在**（docs/UI_L0L1_PORT.md §4 明确
 * 禁止引入），因此本文件当前不能编译——按裁剪计划应删除这些 include 及对应调用；
 * julia_display_theme.h 与 breathing_led.h 存在，但对应的实现未纳入当前构建。 */
#include "avatar_anim_engine.h"
#include "avatar_clip_map.h"
#include "transition_director.h"
#include "transition_player.h"
#include "idle_player.h"
#include "julia_display_theme.h"
#include "breathing_led.h"
#include "julia_rig_assets.h"
#include "avatar_layer_assets.h"
#include "avatar_face.h"
#include "avatar_mouth.h"
#include "julia_sd.h"
#include "extra/libs/png/lodepng.h"

/* 嵌入的 reference.png 导出符号（链接器由 EMBED_FILES 生成 _binary_*_start/end）。
 * 初始时写入 SD 卡 /sdcard/julia/reference.png，供未来界面复用。 */
extern const uint8_t reference_png_start[] asm("_binary_julia_reference_ui_png_start");
extern const uint8_t reference_png_end[] asm("_binary_julia_reference_ui_png_end");

/* 本模块持有的全部 LVGL 对象与派生状态。所有字段只在持有 lvgl_port_lock 时读写；
 * initialized 一旦置真表示 julia_ui_init() 已成功完成，之后各接口以它为前置条件。 */
typedef struct {
    lv_obj_t *avatar_slot;
    lv_obj_t *avatar_container;
    lv_obj_t *neck_container;
    lv_obj_t *head_container;
    lv_obj_t *avatar_image;     /* 静态画布：绑定 s_avatar_pixels（360x360 RGB565）。 */
    lv_obj_t *stream_canvas;    /* 流式画布 1：RGB565 直接路径 / 转场帧。 */
    lv_obj_t *stream_canvas_alt;/* 流式画布 2：与 stream_canvas 做透明度交叉淡化。 */
    lv_obj_t *face;             /* 旧几何脸（julia_ui_init 末尾被删除，见 s_ui.face=NULL）。 */
    lv_obj_t *eyes;
    lv_obj_t *mouth;
    lv_obj_t *expression_label;
    lv_obj_t *bubble_label;     /* 底部气泡文字块（julia_ui_speak / 气泡）。 */
    lv_obj_t *sleep_blackout;   /* 常驻黑层：仅息屏提交帧时显示。 */
    lv_obj_t *rig_root;         /* 分层立绘根对象（拼接 rig 资源用）。 */
    lv_obj_t *rig_body;
    lv_obj_t *rig_head;
    lv_obj_t *rig_hair_front;
    lv_obj_t *rig_hair_back;
    lv_obj_t *rig_eye_left;
    lv_obj_t *rig_eye_right;
    lv_obj_t *rig_eyelid_left;
    lv_obj_t *rig_eyelid_right;
    lv_obj_t *rig_pupil_left;
    lv_obj_t *rig_pupil_right;
    lv_obj_t *rig_mouth;
    lv_anim_t mouth_anim;       /* 旧"嘴型高度"LVGL 动画（几何脸时用，现已被立绘取代）。 */
    bool initialized;
} julia_ui_ctx_t;

static julia_ui_ctx_t s_ui;
static const char *TAG = "JULIA_UI";
static void transition_target_commit(julia_sub_state_t target);
#ifndef AVATAR_LAYER_DEBUG
#define AVATAR_LAYER_DEBUG 0
#endif

#ifndef JULIA_ANIM_LOG
#define JULIA_ANIM_LOG 1
#endif
/* 静态画布缓冲（PSRAM）：s_avatar_pixels 为当前显示帧，s_state_pixels 为"静止骨骼"
 * 拷贝，用于眨眼等只替换眼部矩形时取回未眨眼区域（load_blink_eye_frame）。 */
static lv_color_t *s_avatar_pixels;
static lv_color_t *s_state_pixels;
#define DOZE_DMA_ROWS 12
static lv_color_t *s_doze_dma_rows;   /* DMA 直写 LCD 的行缓冲（内部 RAM）。 */
static lv_color_t *s_doze_frame;      /* 预加载的 doze 帧（PSRAM，360x360 RGB565）。 */
static bool s_doze_frame_loaded;      /* doze 帧是否成功从 SD 预加载。 */
static uint8_t *s_png_data;           /* SD 上 reference.png 的整块数据。 */
static lv_img_dsc_t s_png_image;      /* 由 s_png_data 描述的 LVGL 图像描述符。 */
static esp_lcd_panel_handle_t s_panel;
static volatile julia_sub_state_t s_current_state = JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
static volatile bool s_transitioning; /* 正在播放转场，暂停眨眼/微动等后台动画。 */
static volatile bool s_blinking;      /* 眨眼进行中（blink_task 置位）。 */
/* 说话守卫：talking_start 才置真，talking_stop 清除。set_mouth_* 仅在 s_talking 为真时
 * 生效，避免非说话状态下的残余嘴型更新（见 docs/UI_L0L1_PORT.md §6 常见坑 4）。 */
static volatile bool s_talking;
static volatile bool s_program_blink_enabled = true;   /* 程序化眨眼开关。 */
static volatile bool s_program_motion_mode = true;     /* 程序化微动/动画开关。 */
static volatile bool s_idle_frame_mode;                /* 待机静态帧模式。 */
static volatile julia_sub_state_t s_idle_frame_state = JULIA_SUB_STATE_COUNT;
static volatile bool s_transition_frame_mode;          /* 转场帧模式。 */
static volatile julia_dialog_phase_t s_dialog_phase;   /* 当前对话框相位。 */
static TickType_t s_state_entered_at;                  /* 最近一次状态进入的时刻。 */
static QueueHandle_t s_state_queue;                    /* 状态请求异步队列。 */
static void state_transition_apply(julia_sub_state_t state);
static void state_worker_task(void *argument);
static esp_err_t draw_avatar_rows(esp_lcd_panel_handle_t panel, int y_start, int y_end);
static esp_err_t draw_avatar_region(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                    int x_end, int y_end);
/* 旧对话框相位位移回调：以 LVGL 动画驱动对象水平移动（几何脸时期遗留）。 */
static __attribute__((unused)) void dialog_phase_set_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)x);
}

/* 流式画布绘制完成钩子：把"画布绘制结束"事件转发给 L2 动画引擎（装饰器），
 * 让转场/动画引擎知道这一帧已绘制完成。属于 L2 残留，base 不含动画引擎。 */
static void stream_canvas_draw_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DRAW_POST_END)
        avatar_anim_engine_on_canvas_draw_complete();
}

/* 眨眼时只替换眼睛所在矩形，不重绘整幅立绘，降低单次更新开销。 */
#define BLINK_X0 82
#define BLINK_Y0 108
#define BLINK_X1 278
#define BLINK_Y1 196

/* 创建一个以 image 资源为内容的 LVGL 图片对象，并放到 (x,y) 处。 */
static lv_obj_t *create_rig_image(lv_obj_t *parent, const lv_img_dsc_t *source, int x, int y)
{
    if (!parent || !source) return NULL;
    lv_obj_t *image = lv_img_create(parent);
    lv_img_set_src(image, source);
    lv_obj_set_pos(image, x, y);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_SCROLLABLE);
    return image;
}

/* 校验图层资源描述符是否有效（数据指针/大小在合法范围内）。 */
static bool layer_source_valid(const char *name, const lv_img_dsc_t *source)
{
    bool valid = avatar_layer_asset_valid(source);
    if (!valid) {
        ESP_LOGW("AVATAR_LAYER", "%s rejected: descriptor/data range invalid data=%p",
                 name, source ? source->data : NULL);
    }
    return valid;
}

/* 打印图层资源元数据（stride/大小/驻留区域/理论值与实际是否一致），用于排障。 */
static void audit_layer(const char *name, const lv_img_dsc_t *source)
{
    const avatar_layer_asset_info_t *theory = avatar_layer_asset_info(source);
    if (!source || !theory) {
        ESP_LOGE("AVATAR_LAYER", "%s descriptor/theory missing", name);
        return;
    }
    uint32_t actual_stride = source->header.h ? source->data_size / source->header.h : 0;
    bool match = avatar_layer_asset_valid(source) && actual_stride == theory->stride;
    const char *residency = esp_ptr_external_ram(source->data) ? "PSRAM" :
                            (esp_ptr_in_drom(source->data) ? "FLASH" : "INVALID");
    ESP_LOGI("AVATAR_LAYER",
             "%s dsc cf=%u w=%u h=%u stride=%lu size=%lu data=%p residency=%s",
             name, source->header.cf, source->header.w, source->header.h,
             (unsigned long)actual_stride, (unsigned long)source->data_size,
             source->data, residency);
    ESP_LOG_BUFFER_HEX_LEVEL("AVATAR_LAYER", source->data,
                             source->data_size < 16 ? source->data_size : 16, ESP_LOG_INFO);
    ESP_LOGI("AVATAR_LAYER",
             "%s theory cf=%u w=%u h=%u stride=%u size=%lu linker=%lu compare=%s",
             theory->name, theory->color_format, theory->width, theory->height,
             theory->stride, (unsigned long)theory->data_size,
             (unsigned long)(theory->data_end - theory->data_start),
             match ? "MATCH" : "MISMATCH");
}

static void audit_layer_transforms(void)
{
    const struct { const char *name; lv_obj_t *obj; } objects[] = {
        {"avatar", s_ui.avatar_container}, {"neck", s_ui.neck_container},
        {"head", s_ui.head_container}, {"eye_left", s_ui.rig_eye_left},
        {"eye_right", s_ui.rig_eye_right}, {"mouth", s_ui.rig_mouth},
    };
    for (unsigned i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
        if (!objects[i].obj) continue;
        ESP_LOGI("AVATAR_LAYER", "%s transform angle=%d pivot=(%d,%d) zoom=%d",
                 objects[i].name,
                 lv_obj_get_style_transform_angle(objects[i].obj, 0),
                 lv_obj_get_style_transform_pivot_x(objects[i].obj, 0),
                 lv_obj_get_style_transform_pivot_y(objects[i].obj, 0),
                 lv_obj_get_style_transform_zoom(objects[i].obj, 0));
    }
}

static __attribute__((unused)) const lv_img_dsc_t *valid_mouth_source(avatar_mouth_frame_t frame)
{
    const lv_img_dsc_t *source = avatar_layer_mouth(frame);
    if (layer_source_valid("mouth", source)) return source;
    source = avatar_layer_mouth(AVATAR_MOUTH_CLOSED);
    return layer_source_valid("mouth_default", source) ? source : NULL;
}

/* 创建一个承载头像子层的透明容器，默认隐藏、不可滚动/不可点击。 */
static lv_obj_t *create_avatar_container(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, 360, 360);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(container, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    return container;
}

/* 在"rig 立绘树"与"单张 avatar_image"之间互斥显示：rig 立绘被验证通过前，用户看到
 * 的是经过像素校验的高质量合成图（avatar_image），rig 树保持分配但隐藏。 */
static void set_rig_visible(bool visible)
{
    if (!s_ui.rig_root || !s_ui.avatar_image) return;
    if (visible) {
        lv_obj_clear_flag(s_ui.rig_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.avatar_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.rig_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ui.avatar_image, LV_OBJ_FLAG_HIDDEN);
    }
}

/* L2/L3 残留：判断某状态是否需要"活的 rig 立绘"。当前恒返回 false（所有状态都
 * 走静态合成图/Motion 层），保留为占位并标注 dead-code，裁剪时会一并移除。 */
static __attribute__((unused)) bool state_uses_live_rig(julia_sub_state_t state)
{
    (void)state;
    return false;
}

#define AVATAR_SIZE 360
#define TRANSITION_X0 56
#define TRANSITION_Y0 32
#define TRANSITION_X1 304
#define TRANSITION_Y1 260
#define TRANSITION_FRAMES 14

/* 把立绘资源解码进 s_avatar_pixels（360x360 RGB565）：
 *   - PNG 源（png 魔数）：用 lodepng 解出 24bit 后按最近邻缩放采样到 360x360；
 *   - 非 PNG 源：按 RGB565 读取，同样最近邻缩放到 360x360。
 * 前置：s_avatar_pixels 已分配；返回 false 表示源缺失或解码失败（此时保留旧画面）。
 */
static bool load_image_source(const lv_img_dsc_t *source)
{
    if (!source || !s_avatar_pixels) return false;
    const uint8_t *bytes = source->data;
    const int output_size = 360;
    uint8_t *decoded = NULL;
    unsigned decoded_width = 0;
    unsigned decoded_height = 0;
    bool is_png = source->data_size >= 8 &&
                  memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0;
    if (is_png) {
        unsigned error = lodepng_decode24(&decoded, &decoded_width, &decoded_height,
                                          bytes, source->data_size);
        if (error || !decoded || !decoded_width || !decoded_height) {
            if (decoded) lv_mem_free(decoded);
            ESP_LOGE(TAG, "PNG decode failed: %u", error);
            return false;
        }
        for (int y = 0; y < output_size; ++y) {
            unsigned sy = (unsigned)y * decoded_height / output_size;
            for (int x = 0; x < output_size; ++x) {
                unsigned sx = (unsigned)x * decoded_width / output_size;
                size_t index = ((size_t)sy * decoded_width + sx) * 3;
                s_avatar_pixels[(size_t)y * output_size + x] =
                    lv_color_make(decoded[index], decoded[index + 1], decoded[index + 2]);
            }
        }
        lv_mem_free(decoded);
        return true;
    }
    for (int y = 0; y < output_size; ++y) {
        int sy = y * source->header.h / output_size;
        for (int x = 0; x < output_size; ++x) {
            int sx = x * source->header.w / output_size;
            size_t source_index = (size_t)sy * source->header.w + sx;
            uint16_t raw = ((uint16_t)bytes[source_index * 2] << 8) |
                           (uint16_t)bytes[source_index * 2 + 1];
            uint8_t r = (uint8_t)(((raw >> 11) & 0x1f) * 255 / 31);
            uint8_t g = (uint8_t)(((raw >> 5) & 0x3f) * 255 / 63);
            uint8_t b = (uint8_t)((raw & 0x1f) * 255 / 31);
            s_avatar_pixels[(size_t)y * output_size + x] = lv_color_make(r, g, b);
        }
    }
    return true;
}

/* 只在单画布缓冲中替换眼睛矩形，用于眨眼动效。眨眼资源为不透明 RGB565 帧，
 * 因此不存在新旧眼 alpha 混合；frame==0 表示回到"张开"状态（从 s_state_pixels
 * 恢复无眼区域）。frame∈[0,4) 之外视为恢复到张开帧。 */
static __attribute__((unused)) bool load_blink_eye_frame(uint8_t frame)
{
    if (!s_avatar_pixels || !s_state_pixels) return false;
    if (frame == 0 || frame >= 4) {
        for (int y = BLINK_Y0; y < BLINK_Y1; ++y) {
            memcpy(&s_avatar_pixels[(size_t)y * AVATAR_SIZE + BLINK_X0],
                   &s_state_pixels[(size_t)y * AVATAR_SIZE + BLINK_X0],
                   (BLINK_X1 - BLINK_X0) * sizeof(lv_color_t));
        }
        return true;
    }
    const lv_img_dsc_t *source = julia_blink_frame(frame);
    if (!source || source->header.cf != LV_IMG_CF_TRUE_COLOR ||
        source->header.w != AVATAR_SIZE || source->header.h != AVATAR_SIZE) return false;
    const uint8_t *bytes = source->data;
    for (int y = BLINK_Y0; y < BLINK_Y1; ++y) {
        for (int x = BLINK_X0; x < BLINK_X1; ++x) {
            size_t i = (size_t)y * AVATAR_SIZE + x;
            uint16_t raw = (uint16_t)bytes[i * 2] | ((uint16_t)bytes[i * 2 + 1] << 8);
            uint8_t r = (uint8_t)(((raw >> 11) & 0x1f) * 255 / 31);
            uint8_t g = (uint8_t)(((raw >> 5) & 0x3f) * 255 / 63);
            uint8_t b = (uint8_t)((raw & 0x1f) * 255 / 31);
            s_avatar_pixels[i] = lv_color_make(r, g, b);
        }
    }
    return true;
}

/* 显示某一眨眼帧：先在 LVGL 锁内换左右眼的 image 源并隐藏/显示瞳孔
 * （半闭/全闭时隐藏瞳孔），再解锁。返回是否成功（眼睛对象或资源缺失即失败）。 */
static bool show_blink_frame(uint8_t frame)
{
    if (!lvgl_port_lock(pdMS_TO_TICKS(300))) return false;
    uint8_t local_frame = frame == 2 ? 2 : (frame == 0 ? 0 : 1);
    bool loaded = s_ui.rig_eye_left && s_ui.rig_eye_right;
    if (loaded) {
        const lv_img_dsc_t *left = avatar_layer_eye(true, local_frame);
        const lv_img_dsc_t *right = avatar_layer_eye(false, local_frame);
        if (!left || !right) loaded = false;
        else {
            lv_img_set_src(s_ui.rig_eye_left, left);
            lv_img_set_src(s_ui.rig_eye_right, right);
        }
    }
    if (loaded) {
        if (local_frame == 0) {
            lv_obj_clear_flag(s_ui.rig_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_ui.rig_pupil_right, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.rig_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.rig_pupil_right, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_port_unlock();
    if (loaded) ESP_LOGI("AVATAR_EVENT", "t=%lld blink frame=%u", esp_timer_get_time()/1000, local_frame);
    return loaded;
}

/* 把嵌入固件的 reference.png 写入 SD 卡（若已存在且大小一致则跳过），
 * 供未来"参考界面/截图"复用。SD 未挂载时静默失败。 */
static bool install_reference_png(void)
{
    if (!julia_sd_is_mounted()) return false;
    const char *path = JULIA_SD_MOUNT_POINT "/reference.png";
    size_t embedded_size = (size_t)(reference_png_end - reference_png_start);
    struct stat info;
    if (stat(path, &info) == 0 && (size_t)info.st_size == embedded_size) return true;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t written = fwrite(reference_png_start, 1, embedded_size, file);
    fclose(file);
    return written == embedded_size;
}

/* 从 SD 读回 reference.png 到 PSRAM，构造一个 LVGL 图像描述符 s_png_image。 */
static bool load_png_from_sd(void)
{
    const char *path = JULIA_SD_MOUNT_POINT "/reference.png";
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    s_png_data = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_png_data) { fclose(file); return false; }
    size_t read_size = fread(s_png_data, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) { free(s_png_data); s_png_data = NULL; return false; }
    s_png_image.header.always_zero = 0;
    s_png_image.header.w = 0;
    s_png_image.header.h = 0;
    s_png_image.header.cf = 0;
    s_png_image.data_size = (uint32_t)size;
    s_png_image.data = s_png_data;
    return true;
}

/* "注视姿态"微调：对指定矩形区域做轻微放大（0.5%左右的中心缩扩）并做边缘羽化，
 * 使立绘看起来更"专注"。仅对 JULIA_SUB_STATE_S3_3_USER_CALL 状态在加载后调用。 */
static void apply_attentive_pose(void)
{
    enum { X0 = 70, Y0 = 40, X1 = 290, Y1 = 250, FEATHER = 14 };
    const int width = X1 - X0, height = Y1 - Y0;
    lv_color_t *original = heap_caps_malloc((size_t)width * height * sizeof(lv_color_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!original) return;
    for (int y = 0; y < height; ++y) {
        memcpy(original + (size_t)y * width,
               s_avatar_pixels + (size_t)(Y0 + y) * AVATAR_SIZE + X0,
               width * sizeof(lv_color_t));
    }
    const int cx = width / 2, cy = height / 2;
    for (int y = 0; y < height; ++y) {
        int sy = cy + ((y - cy) * 1000) / 1025;
        if (sy < 0) sy = 0;
        if (sy >= height) sy = height - 1;
        for (int x = 0; x < width; ++x) {
            int sx = cx + ((x - cx) * 1000) / 1025;
            if (sx < 0) sx = 0;
            if (sx >= width) sx = width - 1;
            int edge = x;
            if (width - 1 - x < edge) edge = width - 1 - x;
            if (y < edge) edge = y;
            if (height - 1 - y < edge) edge = height - 1 - y;
            lv_opa_t opacity = edge >= FEATHER ? LV_OPA_COVER
                                                : (lv_opa_t)(edge * 255 / FEATHER);
            lv_color_t base = original[(size_t)y * width + x];
            lv_color_t zoomed = original[(size_t)sy * width + sx];
            s_avatar_pixels[(size_t)(Y0 + y) * AVATAR_SIZE + X0 + x] =
                lv_color_mix(zoomed, base, opacity);
        }
    }
    free(original);
}

/* 加载某子状态对应的立绘资源到静态画布。当前 julia_ui_asset_for_state() 对任意
 * 状态都返回同一张经过像素校验的合成图，因此所有状态共享同一背景；
 * 状态差异由 Motion/眨眼/嘴型层表达。USER_CALL 额外叠加 apply_attentive_pose()。 */
static bool load_avatar(julia_sub_state_t state)
{
    const lv_img_dsc_t *source = julia_ui_asset_for_state(state);
    bool loaded = load_image_source(source);
    if (loaded && state == JULIA_SUB_STATE_S3_3_USER_CALL) apply_attentive_pose();
    return loaded;
}

/* 后台眨眼任务：周期性（3s~8s 随机）在"待机/主动"两个主状态下触发一次单次或
 * 双次眨眼。每次眨眼按 张开→半闭→全闭→张开 顺序调用 show_blink_frame()，
 * 并在眨眼期间暂停微动、结束后恢复。转场进行时跳过。 */
static __attribute__((unused)) void blink_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(3000 + esp_random() % 5001));
        if (!s_panel || s_transitioning) continue;
        bool standby = s_current_state >= JULIA_SUB_STATE_S1_1_NEAR_STANDBY &&
                       s_current_state <= JULIA_SUB_STATE_S1_3_CHARGING_STANDBY;
        bool attentive = s_current_state >= JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER &&
                         s_current_state <= JULIA_SUB_STATE_S3_4_RECOVERY_PROBE;
        if (!standby && !attentive) continue;
        if (!s_program_blink_enabled) continue;
        s_blinking = true;
        avatar_motion_pause();
        int count = (esp_random() % 100U) < 15U ? 2 : 1;
        for (int blink = 0; blink < count && !s_transitioning; ++blink) {
            if (!show_blink_frame(1)) break;
            int64_t layer_started = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(55));
            if (!show_blink_frame(2)) break;
            vTaskDelay(pdMS_TO_TICKS(65));
            if (!show_blink_frame(3)) break;
            vTaskDelay(pdMS_TO_TICKS(65));
            if (!show_blink_frame(0)) break;
#if JULIA_ANIM_LOG
            ESP_LOGI("JULIA_ANIM", "blink state=S%u duration_ms=%lld local_layers=eyes",
                     standby ? JULIA_MAIN_STATE_S1_STANDBY : JULIA_MAIN_STATE_S3_INITIATIVE,
                     (esp_timer_get_time() - layer_started) / 1000);
#endif
            if (blink + 1 < count) vTaskDelay(pdMS_TO_TICKS(95));
        }
        s_blinking = false;
        avatar_motion_resume();
    }
}

/* old API: julia_ui_set_mouth_level(). */
void julia_ui_set_program_blink_enabled(bool enabled) { s_program_blink_enabled = enabled; }

/* 旧"嘴型贴图"路径（几何画布时期）：用 64x64 嘴型资源直接盖写画布固定矩形
 * （y=175..238, x=148..211）。现已被 RGB565 嘴型层（avatar_face_set_rms）取代，
 * 本函数为 __attribute__((unused)) 残留，保留以免破坏链接，不应再被调用。 */
static __attribute__((unused)) bool apply_mouth_patch(uint8_t level)
{
    const lv_img_dsc_t *source = julia_mouth_asset(level);
    if (!source || !s_avatar_pixels) return false;
    const uint8_t *bytes = source->data;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            size_t source_index = ((size_t)y * 64 + x) * 2;
            uint16_t raw = (uint16_t)bytes[source_index] |
                           ((uint16_t)bytes[source_index + 1] << 8);
            s_avatar_pixels[(size_t)(y + 175) * 360 + x + 148] = lv_color_make(
                (uint8_t)(((raw >> 11) & 0x1f) * 255 / 31),
                (uint8_t)(((raw >> 5) & 0x3f) * 255 / 63),
                (uint8_t)((raw & 0x1f) * 255 / 31));
        }
    }
    return true;
}

/* 旧"连续开合"嘴型贴图：openness_q8 的低字节作为相邻两档嘴型之间的混合权重，
 * 在画布上按 alpha 混合两档资源。同样为 __attribute__((unused)) 残留，
 * 已被 avatar_face_set_rms() 的 4 档嘴型层取代。 */
static __attribute__((unused)) bool apply_mouth_patch_blended(uint16_t openness_q8)
{
    if (!s_avatar_pixels) return false;
    if (openness_q8 > 3U * 256U) openness_q8 = 3U * 256U;
    uint8_t low = openness_q8 >> 8;
    uint8_t high = low < 3 ? low + 1 : low;
    lv_opa_t mix = (lv_opa_t)(openness_q8 & 0xffU);
    const lv_img_dsc_t *low_source = julia_mouth_asset(low);
    const lv_img_dsc_t *high_source = julia_mouth_asset(high);
    if (!low_source || !high_source) return false;
    const uint8_t *low_bytes = low_source->data;
    const uint8_t *high_bytes = high_source->data;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            size_t i = ((size_t)y * 64 + x) * 2;
            uint16_t low_raw = (uint16_t)low_bytes[i] | ((uint16_t)low_bytes[i + 1] << 8);
            uint16_t high_raw = (uint16_t)high_bytes[i] | ((uint16_t)high_bytes[i + 1] << 8);
            lv_color_t low_color = lv_color_make(
                (uint8_t)(((low_raw >> 11) & 0x1f) * 255 / 31),
                (uint8_t)(((low_raw >> 5) & 0x3f) * 255 / 63),
                (uint8_t)((low_raw & 0x1f) * 255 / 31));
            lv_color_t high_color = lv_color_make(
                (uint8_t)(((high_raw >> 11) & 0x1f) * 255 / 31),
                (uint8_t)(((high_raw >> 5) & 0x3f) * 255 / 63),
                (uint8_t)((high_raw & 0x1f) * 255 / 31));
            s_avatar_pixels[(size_t)(y + 175) * 360 + x + 148] =
                lv_color_mix(high_color, low_color, mix);
        }
    }
    return true;
}

/* 表情配色表：为每种表达式定义背景颜色（几何脸时期使用，现已被立绘取代）。 */
static const uint32_t s_expr_colors[JULIA_EXPR_COUNT] = {
    [JULIA_EXPR_SLEEP] = 0x596275,
    [JULIA_EXPR_WATCHING] = 0x52A7A0,
    [JULIA_EXPR_HAPPY] = 0xF2B84B,
    [JULIA_EXPR_SPEAKING] = 0xEA6A61,
    [JULIA_EXPR_CONFUSED] = 0x8B79A8,
};

/* 表情名称表：仅供日志/标签显示。 */
static const char *s_expr_names[JULIA_EXPR_COUNT] = {
    [JULIA_EXPR_SLEEP] = "Sleep",
    [JULIA_EXPR_WATCHING] = "Watching",
    [JULIA_EXPR_HAPPY] = "Happy",
    [JULIA_EXPR_SPEAKING] = "Speaking",
    [JULIA_EXPR_CONFUSED] = "Confused",
};

/* LVGL 动画回调：设置嘴型对象高度（旧几何脸动嘴用）。 */
static void set_mouth_height(void *obj, int32_t value)
{
    lv_obj_set_height((lv_obj_t *)obj, value);
}

/* 停止并复位嘴型高度动画，回到闭合高度 5px。 */
static void stop_mouth_anim(void)
{
    lv_anim_del(s_ui.mouth, set_mouth_height);
    lv_obj_set_height(s_ui.mouth, 5);
}

/* 启动嘴型高度往复动画（旧几何脸说话动嘴）：在 4px 与 6+intensity/7px 之间
 * 往复、无限循环、缓入缓出。被 apply_expression 在 SPEAKING 时调用。 */
static void start_mouth_anim(uint8_t intensity)
{
    stop_mouth_anim();
    lv_anim_init(&s_ui.mouth_anim);
    lv_anim_set_var(&s_ui.mouth_anim, s_ui.mouth);
    lv_anim_set_exec_cb(&s_ui.mouth_anim, set_mouth_height);
    lv_anim_set_values(&s_ui.mouth_anim, 4, 6 + intensity / 7);
    lv_anim_set_time(&s_ui.mouth_anim, 180);
    lv_anim_set_playback_time(&s_ui.mouth_anim, 180);
    lv_anim_set_repeat_count(&s_ui.mouth_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_ui.mouth_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&s_ui.mouth_anim);
}

/* 应用"表情"到 UI。注意：本函数体在下面第 3 条语句就返回，故以下所有对
 * face/eyes/mouth/label 的代码全部为死代码（dead code），实际永不执行。
 *
 * 原因：fused 工程已用一张"经过像素校验的高质量合成立绘"完整替代旧的几何脸
 * （圆形脸 + 眼睛文本 '-'/'o' + 高度动画嘴）。生成的立绘会在 init 里直接呈现到
 * 画布（s_ui.face 也在 init 末尾被删除），此处的表情分支不再有意义。
 *
 * 因此这里刻意保留 return; 及其后死代码，并标为 DEAD。裁剪时（docs/UI_L0L1_PORT.md
 * §2）可整体删除本函数体：调用方 julia_ui_set_expression / julia_ui_speak 在立绘
 * 方案下只需走到 return 即可。请不要把它当作"待实现"而补全。 */
static void apply_expression(expr_t expr, uint8_t intensity)
{
    if (expr >= JULIA_EXPR_COUNT) {
        return;
    }
    if (intensity > 100) {
        intensity = 100;
    }

    /* DEAD CODE：generated 立绘已完整替代 legacy 几何脸，以下不再执行。 */
    return;

    lv_obj_set_style_bg_color(s_ui.face, lv_color_hex(s_expr_colors[expr]), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.face, 150 + intensity, LV_PART_MAIN);
    lv_label_set_text(s_ui.expression_label, s_expr_names[expr]);

    switch (expr) {
    case JULIA_EXPR_SLEEP:
        lv_label_set_text(s_ui.eyes, "-   -");
        lv_obj_set_style_radius(s_ui.mouth, 0, LV_PART_MAIN);
        break;
    case JULIA_EXPR_WATCHING:
        lv_label_set_text(s_ui.eyes, "o   o");
        lv_obj_set_style_radius(s_ui.mouth, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        break;
    case JULIA_EXPR_HAPPY:
        lv_label_set_text(s_ui.eyes, "^   ^");
        lv_obj_set_style_radius(s_ui.mouth, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        break;
    case JULIA_EXPR_SPEAKING:
        lv_label_set_text(s_ui.eyes, "o   o");
        lv_obj_set_style_radius(s_ui.mouth, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        break;
    case JULIA_EXPR_CONFUSED:
        lv_label_set_text(s_ui.eyes, "o   ?");
        lv_obj_set_style_radius(s_ui.mouth, 0, LV_PART_MAIN);
        break;
    default:
        break;
    }

    if (expr == JULIA_EXPR_SPEAKING) {
        start_mouth_anim(intensity);
    } else {
        stop_mouth_anim();
    }
}

void julia_ui_init(void)
{
    ESP_LOGI(TAG, "UI asset version: JULIA_V3_20260724");
    if (!lvgl_port_lock(portMAX_DELAY)) {
        return;
    }

    ESP_LOGI(TAG, "Building standby UI");

    /* 预加载 doze 帧：预留 DMA 行缓冲 + 从 SD 读入整帧到 PSRAM。SD 未挂载、文件缺失
     * 或大小不符时回退为"黑屏"（s_doze_frame_loaded=false），由 draw_doze_frame 在
     * 提交时填 0。读取在持有 LVGL 锁 + SD 锁下进行，避免与其它 SD 访问冲突。 */
    if (!s_doze_dma_rows) {
        s_doze_dma_rows = heap_caps_malloc(360U * DOZE_DMA_ROWS * sizeof(lv_color_t),
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_doze_dma_rows) ESP_LOGE(TAG, "Doze DMA workspace reservation failed");
    }
    if (!s_doze_frame_loaded && julia_sd_is_mounted() &&
        julia_sd_lock(pdMS_TO_TICKS(1500))) {
        FILE *doze = fopen(DOZE_FRAME_PATH, "rb");
        const size_t bytes = 360U * 360U * sizeof(lv_color_t);
        struct stat info;
        if (doze && fstat(fileno(doze), &info) == 0 && (size_t)info.st_size == bytes) {
            s_doze_frame = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            s_doze_frame_loaded = s_doze_frame && fread(s_doze_frame, 1, bytes, doze) == bytes;
            if (!s_doze_frame_loaded) {
                heap_caps_free(s_doze_frame);
                s_doze_frame = NULL;
            }
        }
        if (doze) fclose(doze);
        julia_sd_unlock();
        ESP_LOGI(TAG, "Doze asset preload source=%s bytes=%u",
                 s_doze_frame_loaded ? "sd" : "black-fallback", (unsigned)bytes);
    }

    /* 顶层屏幕：纯色背景，去掉默认滚动；avatar_slot 是对外"替换占位符"父容器。 */
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101317), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* avatar_slot：预留"放真实立绘"的父对象（外部经 julia_ui_get_avatar_slot 获取）。 */
    s_ui.avatar_slot = lv_obj_create(screen);
    lv_obj_add_flag(s_ui.avatar_slot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_ui.avatar_slot, 360, 360);
    lv_obj_align(s_ui.avatar_slot, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(s_ui.avatar_slot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.avatar_slot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.avatar_slot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_ui.avatar_slot, LV_OBJ_FLAG_SCROLLABLE);

    /* 旧几何脸（圆形脸 + 眼睛文本 + 高度动画嘴）：仅作回退方案短暂存在，末尾被删除。 */
    s_ui.face = lv_obj_create(s_ui.avatar_slot);
    lv_obj_set_size(s_ui.face, 150, 150);
    lv_obj_align(s_ui.face, LV_ALIGN_CENTER, 0, 2);
    lv_obj_set_style_radius(s_ui.face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.face, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.face, lv_color_hex(0xF4F6F8), LV_PART_MAIN);
    lv_obj_clear_flag(s_ui.face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.face, LV_OBJ_FLAG_HIDDEN);

    /* avatar_container/neck/head：立绘分层挂载所需的透明容器链。 */
    s_ui.avatar_container = create_avatar_container(s_ui.avatar_slot);
    s_ui.neck_container = create_avatar_container(s_ui.avatar_container);
    s_ui.head_container = create_avatar_container(s_ui.neck_container);
    s_ui.avatar_image = lv_canvas_create(s_ui.head_container);
    if (install_reference_png() && load_png_from_sd()) {
        /* Keep the SD resource ready for future screens. */
    }
    /* 静态画布缓冲：s_avatar_pixels 绑定到 avatar_image；s_state_pixels 是"无眼剪辑"
     * 的底图拷贝，供眨眼恢复眼部矩形用（见 load_blink_eye_frame）。 */
    s_avatar_pixels = heap_caps_malloc(360 * 360 * sizeof(lv_color_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_avatar_pixels && load_avatar(JULIA_SUB_STATE_S1_1_NEAR_STANDBY)) {
        lv_canvas_set_buffer(s_ui.avatar_image, s_avatar_pixels, 360, 360, LV_IMG_CF_TRUE_COLOR);
    }
    s_state_pixels = heap_caps_malloc(360 * 360 * sizeof(lv_color_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_state_pixels && s_avatar_pixels) {
        memcpy(s_state_pixels, s_avatar_pixels, 360 * 360 * sizeof(lv_color_t));
    }
    s_state_entered_at = xTaskGetTickCount();
    lv_obj_align(s_ui.avatar_image, LV_ALIGN_CENTER, 0, 0);

    /* 两个流式画布：RGB565 直接呈现 / 转场帧的载体。默认隐藏，由
     * bind/crossfade/transition_direct_* 在需要时显示并隐藏静态画布。 */
    s_ui.stream_canvas = lv_canvas_create(s_ui.head_container);
    lv_obj_set_size(s_ui.stream_canvas, AVATAR_SIZE, AVATAR_SIZE);
    lv_obj_align(s_ui.stream_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_ui.stream_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ui.stream_canvas, stream_canvas_draw_event,
                        LV_EVENT_DRAW_POST_END, NULL);
    s_ui.stream_canvas_alt = lv_canvas_create(s_ui.head_container);
    lv_obj_set_size(s_ui.stream_canvas_alt, AVATAR_SIZE, AVATAR_SIZE);
    lv_obj_align(s_ui.stream_canvas_alt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_ui.stream_canvas_alt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ui.stream_canvas_alt, stream_canvas_draw_event,
                        LV_EVENT_DRAW_POST_END, NULL);

    /* 旧几何脸的 eyes/mouth/label 子对象（在下方被删除）。 */
    s_ui.eyes = lv_label_create(s_ui.face);
    lv_obj_set_style_text_color(s_ui.eyes, lv_color_hex(0x20242A), LV_PART_MAIN);
    lv_obj_align(s_ui.eyes, LV_ALIGN_CENTER, 0, -25);

    s_ui.mouth = lv_obj_create(s_ui.face);
    lv_obj_set_size(s_ui.mouth, 38, 5);
    lv_obj_align(s_ui.mouth, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_style_bg_color(s_ui.mouth, lv_color_hex(0x4A2830), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.mouth, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_ui.mouth, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.expression_label = lv_label_create(s_ui.avatar_slot);
    lv_obj_add_flag(s_ui.expression_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(s_ui.expression_label, lv_color_hex(0xE9EDF2), LV_PART_MAIN);
    lv_obj_align(s_ui.expression_label, LV_ALIGN_BOTTOM_MID, 0, -1);

    /* 立绘方案下彻底移除旧半透明脸及其眼/嘴子对象，避免与立绘叠加出现重影。 */
    lv_obj_del(s_ui.face);
    s_ui.face = NULL;
    s_ui.eyes = NULL;
    s_ui.mouth = NULL;

    /* rig 立绘树：预留把各透明图层按 rig 摆放的位置。rig_body 放的是
     * julia_rig_composite()（像素校验过的合成图）；真正验证通过前保持隐藏。 */
    s_ui.rig_root = lv_obj_create(s_ui.avatar_slot);
    lv_obj_set_size(s_ui.rig_root, 360, 360);
    lv_obj_set_pos(s_ui.rig_root, 0, 0);
    lv_obj_set_style_pad_all(s_ui.rig_root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.rig_root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.rig_root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.rig_root, lv_color_hex(0xFAF9F6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.rig_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_ui.rig_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Use the pixel-verified composite as the baseline renderer. The
     * individual transparent layers remain embedded for staged re-enabling,
     * but are not mixed into the live tree until each one passes hardware
     * validation. */
    s_ui.rig_body = create_rig_image(s_ui.rig_root, julia_rig_composite(), 0, 0);
    s_ui.rig_eyelid_left = lv_obj_create(s_ui.rig_root);
    s_ui.rig_eyelid_right = lv_obj_create(s_ui.rig_root);
    lv_obj_t *eyelids[] = {s_ui.rig_eyelid_left, s_ui.rig_eyelid_right};
    for (int i = 0; i < 2; ++i) {
        lv_obj_set_size(eyelids[i], 51, 1);
        lv_obj_set_pos(eyelids[i], i == 0 ? 108 : 199, 132);
        lv_obj_set_style_bg_color(eyelids[i], lv_color_hex(0xF4D9D0), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(eyelids[i], LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(eyelids[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(eyelids[i], 10, LV_PART_MAIN);
        lv_obj_clear_flag(eyelids[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_move_foreground(s_ui.rig_eyelid_left);
    lv_obj_move_foreground(s_ui.rig_eyelid_right);
    /* Restore the original high-quality flat portrait as the user-visible
     * baseline. Keep the experimental rig allocated but hidden until a new
     * layer set has been validated against this exact character. */
    set_rig_visible(false);

    /* 分层立绘：把眼睛/嘴等子层挂到 head_container，并把句柄交给
     * avatar_micro_motion 统一调度（眨眼/呼吸/瞳孔）。 */
    avatar_face_init(s_ui.head_container);
    s_ui.rig_eye_left = avatar_face_left_eye();
    s_ui.rig_eye_right = avatar_face_right_eye();
    s_ui.rig_mouth = avatar_face_mouth();
    s_ui.rig_pupil_left = NULL;
    s_ui.rig_pupil_right = NULL;
    s_ui.rig_hair_front = NULL;

    /* 底部气泡：显示说话文本（julia_ui_speak / 气泡动效）。 */
    lv_obj_t *bubble = lv_obj_create(screen);
    lv_obj_set_size(bubble, 328, 112);
    lv_obj_align(bubble, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(bubble, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0xF3F5F7), LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bubble, 14, LV_PART_MAIN);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);

    s_ui.bubble_label = lv_label_create(bubble);
    lv_obj_set_width(s_ui.bubble_label, 300);
    lv_label_set_long_mode(s_ui.bubble_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ui.bubble_label, "I'm here with you.");
    lv_obj_set_style_text_color(s_ui.bubble_label, lv_color_hex(0x20242A), LV_PART_MAIN);
    lv_obj_align(s_ui.bubble_label, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 常驻黑层只在息屏提交帧时显示，避免动态创建对象造成碎片。 */
    s_ui.sleep_blackout = lv_obj_create(screen);
    lv_obj_remove_style_all(s_ui.sleep_blackout);
    lv_obj_set_size(s_ui.sleep_blackout, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_ui.sleep_blackout, 0, 0);
    lv_obj_set_style_bg_color(s_ui.sleep_blackout, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.sleep_blackout, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_ui.sleep_blackout, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.sleep_blackout, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_ui.sleep_blackout);
    s_ui.initialized = true;
    /* L2/L3 残留：transition 导演与 idle 播放器的初始化（base 无这些模块）。 */
    transition_director_init(transition_target_commit);
    idle_player_init();
    /* 分层眼睛由 avatar_micro_motion 统一调度。 */
    avatar_layer_bindings_t motion_layers = {
        .container=s_ui.avatar_container, .neck=s_ui.neck_container, .head=s_ui.head_container,
        .left_eye=s_ui.rig_eye_left, .right_eye=s_ui.rig_eye_right,
        .left_pupil=s_ui.rig_pupil_left, .right_pupil=s_ui.rig_pupil_right,
        .mouth=s_ui.rig_mouth, .hair_front=s_ui.rig_hair_front,
    };
    avatar_micro_motion_init(&motion_layers);
    /* 资源元数据自检：仅打印，用于确认嵌入的图层描述符与理论值匹配。 */
    audit_layer("eye_left_open", avatar_layer_eye(true, 0));
    audit_layer("eye_left_half", avatar_layer_eye(true, 1));
    audit_layer("eye_left_closed", avatar_layer_eye(true, 2));
    audit_layer("eye_right_open", avatar_layer_eye(false, 0));
    audit_layer("eye_right_half", avatar_layer_eye(false, 1));
    audit_layer("eye_right_closed", avatar_layer_eye(false, 2));
    audit_layer("mouth_closed", avatar_layer_mouth(AVATAR_MOUTH_CLOSED));
    audit_layer("mouth_half", avatar_layer_mouth(AVATAR_MOUTH_HALF));
    audit_layer("mouth_open", avatar_layer_mouth(AVATAR_MOUTH_OPEN));
    audit_layer_transforms();
    /* Blink scheduling is owned by avatar_parts/avatar_eyes.c. */
    /* 静态渲染诊断期间禁用所有会修改人物画布的后台任务。 */
    apply_expression(JULIA_EXPR_SLEEP, 50);
    ESP_LOGI(TAG, "Standby pixels ready");

    /* 状态请求采用异步队列：发送方只负责投递，动画状态机任务负责执行。
     * 深度 64 可覆盖维护命令的快速突发；任务会合并队列中连续请求，
     * 仅执行最后一个目标，避免过时状态逐个播放。 */
    s_state_queue = xQueueCreate(64, sizeof(julia_sub_state_t));
    if (s_state_queue == NULL) {
        ESP_LOGE(TAG, "state request queue create failed");
    } else if (xTaskCreate(state_worker_task, "avatar_state", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "state worker create failed");
        vQueueDelete(s_state_queue);
        s_state_queue = NULL;
    }
    lvgl_port_unlock();
}

/* 原子地显示整套立绘分层并恢复微动：在 LVGL 锁内一次性清除所有隐藏标志，
 * 避免各层先后出现造成的闪烁；随后恢复微动并让整屏失效重绘。 */
void avatar_show_all(void)
{
    if (!s_ui.initialized || !lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    lv_obj_clear_flag(s_ui.avatar_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.neck_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.head_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.avatar_slot, LV_OBJ_FLAG_HIDDEN);
    avatar_motion_resume_all();
    lv_obj_invalidate(s_ui.avatar_slot);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "avatar layers shown atomically; motion resumed");
}

/* 显示/隐藏常驻黑层（息屏提交帧时用于遮挡画面）。 */
void julia_ui_set_sleep_blackout(bool enabled)
{
    if (!s_ui.initialized || !s_ui.sleep_blackout) return;
    if (enabled) {
        lv_obj_clear_flag(s_ui.sleep_blackout, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ui.sleep_blackout);
    } else {
        lv_obj_add_flag(s_ui.sleep_blackout, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(lv_scr_act());
}

/* 设置黑层透明度（可调息屏程度）。会先确保黑层可见并移到前景。 */
void julia_ui_set_sleep_blackout_opa(uint8_t opacity)
{
    if (!s_ui.initialized || !s_ui.sleep_blackout) return;
    lv_obj_clear_flag(s_ui.sleep_blackout, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.sleep_blackout);
    lv_obj_set_style_bg_opa(s_ui.sleep_blackout, opacity, 0);
    lv_obj_invalidate(s_ui.sleep_blackout);
}

/* 设置"表情"。实际渲染由 apply_expression 执行；因 apply_expression 在立绘方案下
 * 直接 return，本接口目前仅负责在 LVGL 锁内调用它（无实际视觉变更）。 */
void julia_ui_set_expression(expr_t expr, uint8_t intensity)
{
    if (!s_ui.initialized || !lvgl_port_lock(portMAX_DELAY)) {
        return;
    }
    apply_expression(expr, intensity);
    lvgl_port_unlock();
}

/* 子状态 → 主状态 的一一映射（按子状态编号区间归并到 S0~S5）。 */
static julia_main_state_t main_state_for(julia_sub_state_t state)
{
    return state <= JULIA_SUB_STATE_S0_3_MANUAL_SLEEP ? JULIA_MAIN_STATE_S0_SLEEP :
        state <= JULIA_SUB_STATE_S1_3_CHARGING_STANDBY ? JULIA_MAIN_STATE_S1_STANDBY :
        state <= JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION ? JULIA_MAIN_STATE_S2_COMPANION :
        state <= JULIA_SUB_STATE_S3_4_RECOVERY_PROBE ? JULIA_MAIN_STATE_S3_INITIATIVE :
        state <= JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE ? JULIA_MAIN_STATE_S4_DIALOG : JULIA_MAIN_STATE_S5_SILENT;
}

/* L2/L3 残留：转场达到目标之后提交目标状态（preload / 剪辑映射 / idle 进入）。 */
static void transition_target_commit(julia_sub_state_t target)
{
    avatar_clip_map_set_state((uint8_t)target);
    transition_director_preload_for(main_state_for(target));
    idle_player_enter(target);
}

/* L2/L3 残留：流式转场完成回调，转交给 transition_target_commit。 */
static void streamed_transition_done(julia_main_state_t from, julia_main_state_t to,
                                     esp_err_t result, void *context)
{
    julia_sub_state_t target = (julia_sub_state_t)(uintptr_t)context;
    ESP_LOGI(TAG, "TRN transition complete S%u->S%u target=%u result=%s",
             from, to, target, esp_err_to_name(result));
    transition_target_commit(target);
}

/* 把 FSM 子状态应用到 UI：在 LVGL 锁内更新 s_current_state，依据"主状态迁移组合"
 * 选出转场时长，驱动微动/相位/状态，然后在锁外执行 L2/L3 的转场播放或直接提交。
 *
 * 并发：本函数只应由 state_worker_task 调用（状态迁移单线程化）。durations 表给出
 * 各主状态间默认转场毫秒数；transition_director 的脚本优先级更高（可覆盖）。
 * 注意前半段持锁，后半段（avatar_face_set_state / transition_player_play 等）开锁执行，
 * 因为这些 L2 播放器自身会重新获取 LVGL 锁，避免死锁。 */
static void state_transition_apply(julia_sub_state_t state)
{
    if (!s_ui.initialized || state >= JULIA_SUB_STATE_COUNT || !lvgl_port_lock(portMAX_DELAY)) {
        return;
    }
    if (state == s_current_state) {
        lvgl_port_unlock();
        return;
    }
    julia_sub_state_t previous = s_current_state;
    s_current_state = state;
    static const uint16_t durations[JULIA_MAIN_STATE_COUNT][JULIA_MAIN_STATE_COUNT] = {
        [JULIA_MAIN_STATE_S0_SLEEP][JULIA_MAIN_STATE_S1_STANDBY] = 800,
        [JULIA_MAIN_STATE_S1_STANDBY][JULIA_MAIN_STATE_S0_SLEEP] = 1000,
        [JULIA_MAIN_STATE_S1_STANDBY][JULIA_MAIN_STATE_S2_COMPANION] = 600,
        [JULIA_MAIN_STATE_S2_COMPANION][JULIA_MAIN_STATE_S3_INITIATIVE] = 400,
        [JULIA_MAIN_STATE_S3_INITIATIVE][JULIA_MAIN_STATE_S4_DIALOG] = 300,
        [JULIA_MAIN_STATE_S4_DIALOG][JULIA_MAIN_STATE_S1_STANDBY] = 800,
        [JULIA_MAIN_STATE_S4_DIALOG][JULIA_MAIN_STATE_S5_SILENT] = 1000,
        [JULIA_MAIN_STATE_S1_STANDBY][JULIA_MAIN_STATE_S3_INITIATIVE] = 500,
        [JULIA_MAIN_STATE_S2_COMPANION][JULIA_MAIN_STATE_S1_STANDBY] = 600,
    };
    julia_main_state_t from_main = main_state_for(previous);
    julia_main_state_t to_main = main_state_for(state);
    const transition_script_t *script = transition_director_find(from_main, to_main);
    uint16_t transition_ms = durations[from_main][to_main];
    if (script) transition_ms = script->duration_ms;
    if (!transition_ms) transition_ms = from_main == to_main ? 250 : 400;
    if (from_main != to_main && !script) {
        avatar_motion_transition_main(to_main, transition_ms);
    }
    if (state != JULIA_SUB_STATE_S3_3_USER_CALL &&
        (state < JULIA_SUB_STATE_S4_1_LIGHT_DIALOG ||
         state > JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE)) {
        s_dialog_phase = JULIA_DIALOG_PHASE_IDLE;
        avatar_micro_motion_set_dialog_phase(JULIA_DIALOG_PHASE_IDLE);
    }
    avatar_micro_motion_set_state(state);
    s_state_entered_at = xTaskGetTickCount();
    lvgl_port_unlock();
    idle_player_exit();
    avatar_face_set_state((uint8_t)to_main);
    bool directed = false;
    if (from_main != to_main && transition_player_has(from_main, to_main)) {
        led_transition_to((led_state_t)to_main, transition_ms);
        directed = transition_player_play(from_main, to_main, streamed_transition_done,
                                           (void *)(uintptr_t)state) == ESP_OK;
    }
    if (!directed) {
        if (from_main != to_main) led_transition_to((led_state_t)to_main, transition_ms);
        transition_target_commit(state);
    }
    julia_display_theme_on_state_transition(state, transition_ms);
    ESP_LOGI(TAG, "animation state target=%d main=S%u from=S%u transition_ms=%u mode=%s",
             state, to_main, from_main, transition_ms, directed ? "trn-stream" : "direct");
}

/* 状态工作任务：唯一消费 s_state_queue 的地方，串行执行状态迁移，
 * 避免多个调用方并发改动 LVGL 对象。队列为空时阻塞等待。 */
static void state_worker_task(void *argument)
{
    (void)argument;
    julia_sub_state_t requested;
    while (xQueueReceive(s_state_queue, &requested, portMAX_DELAY) == pdTRUE) {
        /* 严格保持不同状态请求的 FIFO 顺序。只有与当前目标完全相同的
         * 连续重复请求会由 state_transition_apply() 快速忽略。 */
        state_transition_apply(requested);
    }
    vTaskDelete(NULL);
}

/* 外部入口：把 FSM 子状态请求投递到异步队列（非阻塞，0 超时）。发送方通常来自
 * julia_voice（FSM 回调线程）。队列满则丢弃并告警，不阻塞调用方。 */
void julia_ui_set_state(julia_sub_state_t state)
{
    if (!julia_ui_showcase_allows_state_change()) return;
    if (state >= JULIA_SUB_STATE_COUNT || !s_state_queue) return;
    if (xQueueSend(s_state_queue, &state, 0) != pdTRUE)
        ESP_LOGW(TAG, "state request queue full; request=%d dropped", state);
}

/* 显示说话文本气泡，并把表情设为 SPEAKING。 */
void julia_ui_speak(const char *text)
{
    if (!s_ui.initialized || text == NULL || !lvgl_port_lock(portMAX_DELAY)) {
        return;
    }
    lv_label_set_text(s_ui.bubble_label, text);
    apply_expression(JULIA_EXPR_SPEAKING, 80);
    lvgl_port_unlock();
}

/* L0 简单主题：只改屏幕背景色；过渡时长参数被忽略（保留接口兼容）。
 * 人物自身仍由双画布渲染（见下方注释），因此只让屏幕背景失效重绘。 */
void julia_ui_apply_theme(uint32_t background_rgb, uint16_t transition_ms)
{
    if (!s_ui.initialized || !lvgl_port_lock(pdMS_TO_TICKS(500))) return;
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(background_rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    /* 人物自身继续由双画布渲染；主题背景变化只失效屏幕背景。 */
    lv_obj_invalidate(screen);
    lvgl_port_unlock();
    (void)transition_ms;
}

/* 呼吸动画占位：低频 idle 动画由专门的呼吸任务（julia_backlight / 微动）拥有。 */
void julia_ui_breathing_anim(void)
{
    /* The dedicated breathing task owns the low-frequency idle animation. */
}

/* 说话开始：置位 s_talking 守卫、暂停微动、并把嘴型清零。
 * 必须先调用（否则 set_mouth_openness/set_mouth_level 因 s_talking 为假而直接返回，
 * 嘴型不会动——见 docs/UI_L0L1_PORT.md §6 常见坑 4）。由 lipsync 在语音下行开始前调用。 */
void julia_ui_talking_start(void)
{
    if (!s_ui.initialized || !s_panel || !lvgl_port_lock(pdMS_TO_TICKS(1000))) return;
    s_talking = true;
    avatar_motion_pause();
    lvgl_port_unlock();
    avatar_face_set_rms(0);
}

/* 按离散档位(0..3)设置嘴型。仅在 s_talking 时生效。 */
void julia_ui_set_mouth_level(uint8_t level)
{
    if (!s_talking || !s_panel || !lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    lvgl_port_unlock();
    static const uint16_t rms_for_level[] = {0, 30, 65, 95};
    avatar_face_set_rms(rms_for_level[level < 4 ? level : 3]);
}

/* 开口度(Q8)设置嘴型：把连续开口度粗分为 0/30/65/95 四档 RMS 传给嘴型层。
 * 仅在 s_talking 时生效（s_talking 守卫）。 */
void julia_ui_set_mouth_openness(uint16_t openness_q8)
{
    if (!s_talking || !s_panel || !lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    lvgl_port_unlock();
    uint16_t rms = openness_q8 < 256 ? 0 : openness_q8 < 512 ? 30 :
                   openness_q8 < 768 ? 65 : 95;
    avatar_face_set_rms(rms);
}

/* 说话结束：清除守卫、恢复微动并把嘴型清零。 */
void julia_ui_talking_stop(void)
{
    if (!s_talking) return;
    s_talking = false;
    avatar_motion_resume();
    if (!s_panel || !lvgl_port_lock(pdMS_TO_TICKS(1000))) return;
    lvgl_port_unlock();
    avatar_face_set_rms(0);
}

/* 设置对话框相位（IDLE/LISTENING/THINKING/SPEAKING），并转发给微动层；
 * 若程序化微动被关闭还会同步到 L2 clip_map。相位相同则忽略，避免重复调用。 */
void julia_ui_set_dialog_phase(julia_dialog_phase_t phase)
{
    if (!s_ui.initialized || phase == s_dialog_phase || !lvgl_port_lock(pdMS_TO_TICKS(300))) return;
    s_dialog_phase = phase;
    avatar_micro_motion_set_dialog_phase((uint8_t)phase);
    lvgl_port_unlock();
    if (!s_program_motion_mode) avatar_clip_map_set_dialog_phase((uint8_t)phase);
    julia_display_theme_on_interaction();
}

/* RGB565 直接呈现：把整帧拷贝进静态画布缓冲并让 canvas 失效，稍后由 LVGL 刷新。 */
void julia_ui_present_rgb565_frame(const uint16_t *pixels, size_t pixel_count)
{
    if (!pixels || pixel_count != AVATAR_SIZE * AVATAR_SIZE || !s_avatar_pixels) return;
    memcpy(s_avatar_pixels, pixels, pixel_count * sizeof(uint16_t));
    if (s_ui.avatar_image) lv_obj_invalidate(s_ui.avatar_image);
}

/* 把外部帧缓冲绑定到流式画布并显示，隐藏静态画布与其另一个流式画布。
 * 转场帧模式（s_transition_frame_mode）关闭且程序化微动开启时被跳过，保存画布结构。 */
void julia_ui_bind_rgb565_frame(uint16_t *pixels, size_t pixel_count)
{
    if (s_program_motion_mode && !s_transition_frame_mode) return;
    if (!pixels || pixel_count != AVATAR_SIZE * AVATAR_SIZE || !s_ui.stream_canvas) return;
    lv_canvas_set_buffer(s_ui.stream_canvas, pixels, AVATAR_SIZE, AVATAR_SIZE,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_style_opa(s_ui.stream_canvas, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.avatar_image, LV_OBJ_FLAG_HIDDEN);
    if (s_ui.stream_canvas_alt) lv_obj_add_flag(s_ui.stream_canvas_alt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.stream_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_ui.stream_canvas);
}

/* 两帧交叉淡化：把 old/new 分别绑定到两个流式画布，通过各自不透明度(progress)与
 * 前景切换实现 alpha 过渡。前进度从 0..255，old 透明度=255-progress，new=progress。 */
void julia_ui_crossfade_rgb565_frames(uint16_t *old_pixels, uint16_t *new_pixels,
                                      size_t pixel_count, uint8_t progress)
{
    if (s_program_motion_mode && !s_transition_frame_mode) return;
    if (!old_pixels || !new_pixels || pixel_count != AVATAR_SIZE * AVATAR_SIZE ||
        !s_ui.stream_canvas || !s_ui.stream_canvas_alt) return;
    lv_canvas_set_buffer(s_ui.stream_canvas, old_pixels, AVATAR_SIZE, AVATAR_SIZE,
                         LV_IMG_CF_TRUE_COLOR);
    lv_canvas_set_buffer(s_ui.stream_canvas_alt, new_pixels, AVATAR_SIZE, AVATAR_SIZE,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(s_ui.avatar_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.stream_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.stream_canvas_alt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_ui.stream_canvas, (lv_opa_t)(255U - progress), LV_PART_MAIN);
    lv_obj_set_style_opa(s_ui.stream_canvas_alt, (lv_opa_t)progress, LV_PART_MAIN);
    lv_obj_move_foreground(s_ui.stream_canvas_alt);
    lv_obj_invalidate(s_ui.stream_canvas);
    lv_obj_invalidate(s_ui.stream_canvas_alt);
}

/* 启用/关闭转场帧模式：关闭时把两个流式画布隐藏、恢复静态画布显示，并同步刷新一帧，
 * 确保画面稳定回到静态立绘。 */
void julia_ui_set_transition_frame_mode(bool enabled)
{
    if (!enabled && s_ui.initialized && lvgl_port_lock(pdMS_TO_TICKS(250))) {
        if (s_ui.stream_canvas) lv_obj_add_flag(s_ui.stream_canvas, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.stream_canvas_alt) lv_obj_add_flag(s_ui.stream_canvas_alt, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.avatar_image) lv_obj_clear_flag(s_ui.avatar_image, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.avatar_slot) lv_obj_invalidate(s_ui.avatar_slot);
        esp_err_t sync_err = lvgl_port_refr_now_sync(pdMS_TO_TICKS(1000));
        lvgl_port_unlock();
        if (sync_err != ESP_OK)
            ESP_LOGW(TAG, "transition static frame sync failed: %s", esp_err_to_name(sync_err));
    }
    s_transition_frame_mode = enabled;
}

/* 待机帧模式：enabled 时记录一个"待机目标子状态"，供 transition_direct_begin 判断
 * 是否需要把嘴型层切成"张开的待机嘴"。关闭时清空该子状态。 */
void julia_ui_set_idle_frame_mode(bool enabled, julia_sub_state_t state)
{
    s_idle_frame_mode = enabled;
    s_idle_frame_state = enabled ? state : JULIA_SUB_STATE_COUNT;
}

/* transition_direct 开始：进入转场帧模式、标记转场活跃；待机帧模式下若目标落在
 * 对话区间，则关闭嘴型过渡、显示嘴型（准备显示待机嘴）。 */
esp_err_t julia_ui_transition_direct_begin(void)
{
    if (!s_ui.initialized) return ESP_ERR_INVALID_STATE;
    s_transition_frame_mode = true;
    avatar_face_set_transition_active(true);
    if (s_idle_frame_mode && s_idle_frame_state >= JULIA_SUB_STATE_S4_1_LIGHT_DIALOG &&
        s_idle_frame_state <= JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE) {
        avatar_mouth_set_transition_active(false);
        avatar_mouth_set_visible(true);
    }
    return ESP_OK;
}

/* transition_direct 逐帧绘制：绑定一帧到流式画布并同步刷新到面板。返回刷新结果。 */
esp_err_t julia_ui_transition_direct_draw(const uint16_t *pixels, size_t bytes,
                                          const char *source)
{
    (void)source;
    if (!pixels || bytes != AVATAR_SIZE * AVATAR_SIZE * sizeof(uint16_t) ||
        !lvgl_port_lock(pdMS_TO_TICKS(250))) return ESP_ERR_INVALID_ARG;
    julia_ui_bind_rgb565_frame((uint16_t *)pixels, AVATAR_SIZE * AVATAR_SIZE);
    esp_err_t err = lvgl_port_refr_now_sync(pdMS_TO_TICKS(1000));
    lvgl_port_unlock();
    return err;
}

/* transition_direct 结束：清除转场标记；非待机模式时退出转场帧模式并回落到静态立绘。 */
esp_err_t julia_ui_transition_direct_end(void)
{
    if (!s_ui.initialized) return ESP_ERR_INVALID_STATE;
    avatar_face_set_transition_active(false);
    if (!s_idle_frame_mode) {
        s_idle_frame_state = JULIA_SUB_STATE_COUNT;
        julia_ui_set_transition_frame_mode(false);
    }
    return ESP_OK;
}

/* 直接呈现一帧"待机立绘"到面板：把 360x360 的静态画布按整行交给 draw_avatar_region。 */
esp_err_t julia_ui_draw_standby_direct(esp_lcd_panel_handle_t panel)
{
    return draw_avatar_rows(panel, 0, 360);
}

/* 提交 doze 帧到面板：在 LVGL 刷新暂停期间按 DOZE_DMA_ROWS 分行把 doze 帧（或黑屏
 * 回退）以 DMA 直写 LCD。path 非空且 s_doze_frame_loaded 才真正取 doze 帧，否则填 0。
 * 成功后把 *asset_loaded 置真。 */
esp_err_t julia_ui_draw_doze_frame(const char *path, bool *asset_loaded)
{
    if (asset_loaded) *asset_loaded = false;
    if (!s_panel || !path || !lvgl_port_lock(pdMS_TO_TICKS(1000)))
        return ESP_ERR_INVALID_STATE;
    bool loaded = path[0] != '\0' && s_doze_frame_loaded;
    esp_err_t err = s_doze_dma_rows ? ESP_OK : ESP_ERR_NO_MEM;
    for (int y = 0; y < 360 && err == ESP_OK; y += DOZE_DMA_ROWS) {
        int count = y + DOZE_DMA_ROWS <= 360 ? DOZE_DMA_ROWS : 360 - y;
        size_t bytes = 360U * (size_t)count * sizeof(lv_color_t);
        if (loaded) {
            memcpy(s_doze_dma_rows, &s_doze_frame[(size_t)y * 360U], bytes);
        } else {
            memset(s_doze_dma_rows, 0, bytes);
        }
        if (err == ESP_OK)
            err = lvgl_port_draw_bitmap_sync(s_panel, 0, y, 360, y + count, s_doze_dma_rows);
    }
    lvgl_port_unlock();
    if (asset_loaded) *asset_loaded = loaded && err == ESP_OK;
    ESP_LOGI(TAG, "Doze frame commit source=%s result=%s path=%s",
             loaded ? "sd-cache" : "black-fallback",
             esp_err_to_name(err), path);
    return err;
}

/* 竖直范围(row y_start..y_end)的直接呈现入口。 */
static esp_err_t draw_avatar_rows(esp_lcd_panel_handle_t panel, int y_start, int y_end)
{
    return draw_avatar_region(panel, 0, y_start, 360, y_end);
}

/* 把静态画布指定矩形按 12 行一组 DMA 直写 LCD。使用内部 DMA 缓冲，避免从 PSRAM
 * 直接引用（QSPI DMA 需要内部/同步地址）。任一行失败即中止并返回错误。 */
static esp_err_t draw_avatar_region(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                    int x_end, int y_end)
{
    if (!panel || !s_avatar_pixels) return ESP_ERR_INVALID_STATE;
    if (x_start < 0 || x_end > 360 || x_start >= x_end ||
        y_start < 0 || y_end > 360 || y_start >= y_end) return ESP_ERR_INVALID_ARG;
    s_panel = panel;
    if (!lvgl_port_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    const int rows_per_transfer = 12;
    const int width = x_end - x_start;
    lv_color_t *dma_rows = heap_caps_malloc(width * rows_per_transfer * sizeof(lv_color_t),
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!dma_rows) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    for (int y = y_start; y < y_end && err == ESP_OK; y += rows_per_transfer) {
        int rows = (y + rows_per_transfer <= y_end) ? rows_per_transfer : y_end - y;
        for (int row = 0; row < rows; ++row) {
            memcpy(&dma_rows[row * width],
                   &s_avatar_pixels[(size_t)(y + row) * 360 + x_start],
                   width * sizeof(lv_color_t));
        }
        err = lvgl_port_draw_bitmap_sync(panel, x_start, y, x_end, y + rows, dma_rows);
    }
    heap_caps_free(dma_rows);
    lvgl_port_unlock();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Standby portrait transfer failed: %s", esp_err_to_name(err));
    else if (x_start == 0 && x_end == 360 && y_start == 0 && y_end == 360)
        ESP_LOGI(TAG, "Standby portrait: 360/360 rows sent");
    return err;
}

lv_obj_t *julia_ui_get_avatar_slot(void)
{
    return s_ui.avatar_slot;
}

julia_sub_state_t julia_ui_current_state(void) { return s_current_state; }
