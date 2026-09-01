/**
 * @file    avatar_eyes.c
 * @brief   眼睛部件实现：三态帧切换 + 周期性眨眼任务 + 显隐控制。
 *
 * 模块边界：
 *   - 资源：眼睛使用生成的 avatar_chroma_assets（eye_left/right_open/half/closed），经
 *     left_source()/right_source() 按帧号映射；动态调色/检测函数处理 RGB565 帧缓冲。
 *   - 眨眼调度：本模块自建 blink_task，每 3~8s 随机触发一次“闭眼 90ms → 睁眼 90ms”；
 *     何时眨眼由本文件决定，眼睛对象最终也可能被 avatar_micro_motion 覆盖 src 做状态换帧
 *     （二者共用同一 lv_obj_t，见 julia_ui.c 的“分层眼睛由 micro_motion 统一调度”）。
 *   - 瞳孔随动不在此处：本模块只把左右眼对象句柄暴露给上层。
 *
 * 线程模型：
 *   - blink_task 是独立 FreeRTOS 任务（PSRAM 栈），通过 avatar_eyes_show() 取 lvgl_port 锁刷帧；
 *   - 公开 API 由 julia_ui/上层调用（通常在 LVGL 锁内），内部再次 lock 是“重入安全”的保险。
 *   - 共享 s_main_state / s_transition_active / s_idle_closed / s_generation 为 volatile，
 *     供 blink_task 与公开 API 跨线程读取。
 *
 * 时间驱动：blink 用 vTaskDelay(3~8s) 的睡眠式随机触发，不使用 esp_timer。
 */
#include "avatar_eyes.h"

#include "avatar_chroma_assets.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "lvgl_port.h"

#ifndef JULIA_AVATAR_LOG
#define JULIA_AVATAR_LOG 1
#endif

/* ---- 模块级状态 ---- */
static lv_obj_t *s_left;               /* 左眼对象。 */
static lv_obj_t *s_right;              /* 右眼对象。 */
static volatile uint8_t s_main_state = 1; /* 当前主状态（默认 S1 待机），决定是否允许眨眼。 */
static volatile bool s_transition_active; /* 转场中：冻结眨眼、隐藏眼睛。 */
static volatile bool s_idle_closed;      /* 被外部要求持续闭眼（跨 blink 保持）。 */
static volatile uint32_t s_generation;   /* “代”计数：状态一变就 +1，使进行中的 90ms 眨眼作废。 */

/* 用于帧缓冲调色/检测的“瞳孔绿色”像素值（RGB565）。 */
#define PUPIL_GREEN_RGB565 0x2645U

/* 眼睛资源在 360×360 底图中的基准位置。闭眼素材的眼睑中心略偏高，
 * 只在眨眼帧下移以覆盖底图下缘残留；睁眼帧始终保持生成清单坐标。 */
#define EYE_LEFT_X              112
#define EYE_RIGHT_X             194
#define EYE_BASE_Y              108
#define EYE_HALF_Y_OFFSET       1
#define EYE_CLOSED_Y_OFFSET     3

static int eye_y_for_frame(avatar_eyes_frame_t frame)
{
    if (frame == AVATAR_EYES_CLOSED) return EYE_BASE_Y + EYE_CLOSED_Y_OFFSET;
    if (frame == AVATAR_EYES_HALF) return EYE_BASE_Y + EYE_HALF_Y_OFFSET;
    return EYE_BASE_Y;
}

/* 判定某像素是否落在左右眼区域（360×360 帧内的矩形窗）。 */
static bool in_eye_region(unsigned x, unsigned y)
{
    return x >= 45U && x < 315U && y >= 55U && y < 235U;
}

/* 将 360×360 RGB565 帧中“绿色”瞳孔像素重着色为金铜色（仅作用于眼睛区域矩形）。
 * 输入/输出：原地修改 pixels；非 360×360 或 pointers 为空则直接返回（无副作用）。
 * 判定：以 green 为主的像素（G 分量明显高于 R、B）视为瞳孔，按比例插值到金色，并夹取到 5/6/5 位宽。 */
void avatar_eyes_correct_pupils_rgb565(uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (!pixels || width != 360 || height != 360) return;
    for (unsigned y = 55; y < 235; ++y) {
        for (unsigned x = 45; x < 315; ++x) {
            if (!in_eye_region(x, y)) continue;
            uint16_t value = pixels[y * width + x];
            unsigned red = (value >> 11) & 0x1fU;
            unsigned green = (value >> 5) & 0x3fU;
            unsigned blue = value & 0x1fU;
            unsigned red6 = red * 2U;
            unsigned blue6 = blue * 2U;
            if (green >= 18U && green * 4U > red6 * 5U &&
                green * 4U > blue6 * 5U) {
                unsigned gold_red = 18U + green / 5U;
                unsigned gold_green = 16U + green / 2U;
                unsigned gold_blue = 2U + green / 16U;
                if (gold_red > 31U) gold_red = 31U;
                if (gold_green > 63U) gold_green = 63U;
                if (gold_blue > 31U) gold_blue = 31U;
                pixels[y * width + x] = (uint16_t)((gold_red << 11) |
                                                   (gold_green << 5) | gold_blue);
            }
        }
    }
}

/* 检测帧中是否存在“绿色块”：理想情况下瞳孔有颜色、不出现特定绿。若在眼睛窗口之外出现
 * 大量 PUPIL_GREEN_RGB565 像素（>256 个）则返回 true（存在问题）。
 * 输入：非 360×360 或 NULL 时保守返回 true（视为需检查）。 */
bool avatar_eyes_has_green_blob_rgb565(const uint16_t *pixels, uint16_t width,
                                       uint16_t height)
{
    if (!pixels || width != 360 || height != 360) return true;
    unsigned outside = 0;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            if (!in_eye_region(x, y) && pixels[y * width + x] == PUPIL_GREEN_RGB565 &&
                ++outside > 256U) return true;
    return false;
}

/* 按帧号取左/右眼资源（越界帧号回退到 OPEN）。 */
static const lv_img_dsc_t *left_source(avatar_eyes_frame_t frame)
{
    static const lv_img_dsc_t *sources[] = {
        &avatar_asset_eye_left_open,
        &avatar_asset_eye_left_half,
        &avatar_asset_eye_left_closed,
    };
    return sources[frame <= AVATAR_EYES_CLOSED ? frame : AVATAR_EYES_OPEN];
}

static const lv_img_dsc_t *right_source(avatar_eyes_frame_t frame)
{
    static const lv_img_dsc_t *sources[] = {
        &avatar_asset_eye_right_open,
        &avatar_asset_eye_right_half,
        &avatar_asset_eye_right_closed,
    };
    return sources[frame <= AVATAR_EYES_CLOSED ? frame : AVATAR_EYES_OPEN];
}

/* 立即把左右眼 src 切换到指定帧。前置：眼睛对象已创建。
 * 副作用：内部取 lvgl_port_lock(100ms)；若超时则放弃本帧（不阻塞调用方）。
 * 失败路径：对象未建或锁超时 → 直接返回；刷新耗时 >12ms 打慢刷警告。 */
void avatar_eyes_show(avatar_eyes_frame_t frame)
{
    if (!s_left || !s_right || !lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    int64_t started = esp_timer_get_time();
    int eye_y = eye_y_for_frame(frame);
    lv_obj_set_pos(s_left, EYE_LEFT_X, eye_y);
    lv_obj_set_pos(s_right, EYE_RIGHT_X, eye_y);
    lv_img_set_src(s_left, left_source(frame));
    lv_img_set_src(s_right, right_source(frame));
    lvgl_port_unlock();
    int64_t elapsed = esp_timer_get_time() - started;
    if (elapsed > 12000)
        ESP_LOGW("JULIA_AVATAR", "eye refresh slow frame=%u elapsed_us=%lld", frame, elapsed);
}

/* 眨眼任务：每 3~8s 随机触发一次“闭眼 90ms → 睁眼 90ms”。
 * 守卫（满足任一条件则本轮跳过）：转场中、被要求持续闭眼、主状态不在 S1/S3（待机/主动）。
 * “代”机制：触发后先记录 generation，闭眼 90ms 后才睁眼；若期间状态变化使 generation 增值
 * （set_state/set_idle_closed/set_transition_active 都会 ++），说明外部已抢先调整眼睛，
 * 则放弃本次睁眼，避免覆盖外部刚设的帧。 */
static void blink_task(void *argument)
{
    (void)argument;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(3000U + esp_random() % 5001U));
        if (s_transition_active || s_idle_closed ||
            (s_main_state != 1 && s_main_state != 3)) continue;
        uint32_t generation = s_generation;
        int64_t started = esp_timer_get_time();
#if JULIA_AVATAR_LOG
        ESP_LOGI("JULIA_AVATAR", "blink trigger state=S%u", s_main_state);
#endif
        avatar_eyes_show(AVATAR_EYES_CLOSED);
        vTaskDelay(pdMS_TO_TICKS(90));
        if (generation != s_generation || s_transition_active) continue;
        avatar_eyes_show(AVATAR_EYES_OPEN);
        vTaskDelay(pdMS_TO_TICKS(90));
#if JULIA_AVATAR_LOG
        ESP_LOGI("JULIA_AVATAR", "blink complete duration_ms=%lld",
                 (esp_timer_get_time() - started) / 1000);
#endif
    }
}

/* 创建左右眼对象并启动眨眼任务。前置：parent 有效、且尚未初始化（重复调用直接返回）。
 * 副作用：创建两个 lv_img（初始 src=开眼），启动 blink_task（PSRAM 栈）。 */
void avatar_eyes_init(lv_obj_t *parent)
{
    if (!parent || s_left || s_right) return;
    s_left = lv_img_create(parent);
    s_right = lv_img_create(parent);
    lv_img_set_src(s_left, &avatar_asset_eye_left_open);
    lv_img_set_src(s_right, &avatar_asset_eye_right_open);
    lv_obj_set_pos(s_left, EYE_LEFT_X, EYE_BASE_Y);
    lv_obj_set_pos(s_right, EYE_RIGHT_X, EYE_BASE_Y);
    lv_obj_clear_flag(s_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_right, LV_OBJ_FLAG_SCROLLABLE);
    if (xTaskCreateWithCaps(blink_task, "avatar_eyes", 3072, NULL, 2, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE("JULIA_AVATAR", "failed to create PSRAM blink task");
}

/* 记录主状态并使进行中的眨眼失效；非转场时复位为睁眼。由 avatar_face_set_state 转发来。 */
void avatar_eyes_set_state(uint8_t main_state)
{
    s_main_state = main_state;
    ++s_generation;
    if (!s_transition_active && !s_idle_closed) avatar_eyes_show(AVATAR_EYES_OPEN);
}

/* 请求持续闭眼/恢复睁眼，并作废进行中的眨眼（供微动引擎在睡眠等场景下保持闭眼）。 */
void avatar_eyes_set_idle_closed(bool closed)
{
    s_idle_closed = closed;
    ++s_generation;
    avatar_eyes_show(closed ? AVATAR_EYES_CLOSED : AVATAR_EYES_OPEN);
}

/* 转场开关：转场期间隐藏眼睛、作废眨眼；结束时恢复可见并复位为睁眼。 */
void avatar_eyes_set_transition_active(bool active)
{
    s_transition_active = active;
    ++s_generation;
    avatar_eyes_set_visible(!active);
    if (!active) avatar_eyes_show(AVATAR_EYES_OPEN);
}

/* 直接显示/隐藏左右眼。内部取 lvgl_port_lock(100ms)，超时则放弃。 */
void avatar_eyes_set_visible(bool visible)
{
    if (!s_left || !s_right || !lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    if (visible) {
        lv_obj_clear_flag(s_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_right, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_right, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

/* 左右眼句柄泄露给上层（用于微动引擎绑定/转场时联动）。 */
lv_obj_t *avatar_eyes_left_object(void) { return s_left; }
lv_obj_t *avatar_eyes_right_object(void) { return s_right; }
