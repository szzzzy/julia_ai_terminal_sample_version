/**
 * @file    julia_avatar.c
 * @brief   把听音、等待回答和说话状态转换为用户看到的 Julia 表情。
 *
 * 普通状态使用稳定底图，通过眼睛区分正在听、等待回答和播放回答；设备说话时，
 * 嘴型由已经送往扬声器的声音强度驱动，而不是由网络到包速度驱动。睡眠时切换为
 * 完整闭眼画面。背光和是否进入待机由其它模块统一决定。
 *
 * 独立任务每 40 ms 更新一次嘴型和微动。语音任务只写入“当前阶段”和声音强度，
 * 不直接操作界面对象；所有 LVGL 修改在内部串行完成。
 *
 * 内嵌相位画面解压到外部内存并校验完整性后才显示。嘴型将短时间声音能量分成四档，
 * 没有新声音或设备不在说话时自动闭嘴。
 */
#include "julia_avatar.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "avatar_rle.h"
#include "esp_check.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "avatar_chroma_assets.h"
#include "avatar_face_base.h"
#include "avatar_face_doze.h"
#include "avatar_eyes.h"
#include "avatar_mouth.h"
#include "julia_backlight.h"
#include "lvgl_port.h"

#define AVATAR_UPDATE_MS          40U
#define AVATAR_PCM_HOLD_MS        180U
#define AVATAR_BREATH_PERIOD_MS   4200U
#define AVATAR_NOD_PERIOD_MS      11000U
#define AVATAR_NOD_DURATION_MS    720U
#define AVATAR_FRAME_WIDTH         360U
#define AVATAR_FRAME_HEIGHT        360U
#define AVATAR_FRAME_PIXELS        (AVATAR_FRAME_WIDTH * AVATAR_FRAME_HEIGHT)
#define AVATAR_FRAME_BYTES         (AVATAR_FRAME_PIXELS * sizeof(uint16_t))
#define BOOT_BLINK_COUNT            8U
#define BOOT_BLINK_OPEN_MS          255U
#define BOOT_BLINK_CLOSED_MS        120U
#define STATUS_LABEL_X               60
#define STATUS_LABEL_Y              100
#define STATUS_LABEL_WIDTH          220
#define OFFLINE_LABEL_Y             120

/* 整体移动 360×360 根对象会让每一帧都刷新全屏；当前 QSPI 面板分十条发送且没有
 * 撕裂同步信号，持续全屏更新会出现明显闪烁。因此微动只修改局部眼睛和嘴巴，
 * 不移动整幅立绘。 */
#define AVATAR_ENABLE_FULL_FRAME_MOTION 0

static const char *TAG = "julia_avatar";
static lv_obj_t *s_motion_root;
static lv_obj_t *s_base;
static lv_obj_t *s_status_label;
static lv_obj_t *s_offline_label;
static volatile bool s_ready;
static bool s_talking;
static uint32_t s_smoothed_rms;
static uint8_t s_mouth_level;
static uint8_t s_target_mouth_level;
static uint32_t s_last_pcm_ms;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static julia_avatar_dialog_phase_t s_dialog_phase = JULIA_AVATAR_DIALOG_IDLE;
static bool s_dozing;
static bool s_offline;
/* Last phase successfully assigned to the LVGL base image.  It is separate
 * from the requested state so a lock timeout can be retried safely. */
static julia_avatar_dialog_phase_t s_applied_dialog_phase =
    (julia_avatar_dialog_phase_t)(JULIA_AVATAR_DIALOG_SPEAKING + 1);
static portMUX_TYPE s_phase_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_boot_sequence_played;
static char s_status_text[32] = "S0 BOOT";

static void status_label_place(void)
{
    if (s_status_label == NULL) return;
    lv_obj_set_pos(s_status_label, STATUS_LABEL_X, STATUS_LABEL_Y);
    lv_obj_set_width(s_status_label, STATUS_LABEL_WIDTH);
    lv_obj_move_foreground(s_status_label);
}

void julia_avatar_set_status_text(const char *text)
{
    if (text == NULL || text[0] == '\0') return;
    char snapshot[sizeof(s_status_text)];
    portENTER_CRITICAL(&s_phase_lock);
    strncpy(s_status_text, text, sizeof(s_status_text) - 1U);
    s_status_text[sizeof(s_status_text) - 1U] = '\0';
    memcpy(snapshot, s_status_text, sizeof(snapshot));
    portEXIT_CRITICAL(&s_phase_lock);

    if (s_status_label == NULL || !lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    lv_label_set_text(s_status_label, snapshot);
    /* 每次更新都重新应用固定坐标，防止布局或后续 UI 操作覆盖调试字幕位置。 */
    status_label_place();
    lv_obj_invalidate(s_status_label);
    lvgl_port_unlock();
}

void julia_avatar_set_offline(bool offline)
{
    portENTER_CRITICAL(&s_phase_lock);
    s_offline = offline;
    portEXIT_CRITICAL(&s_phase_lock);

    if (s_offline_label == NULL || !lvgl_port_lock(pdMS_TO_TICKS(100))) return;
    if (offline) lv_obj_clear_flag(s_offline_label, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_offline_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_offline_label);
    lv_obj_invalidate(s_offline_label);
    lvgl_port_unlock();
}

/* 嵌入的相位帧二进制导出符号（链接器按 EMBED_FILES 生成 _binary_*_start/end）。
 * 每个相位一张 360x360 RGB565 帧，用项目自有 RLE 压缩后嵌入固件。 */
extern const uint8_t LISTEN_bin_start[] asm("_binary_LISTEN_bin_start");
extern const uint8_t LISTEN_bin_end[] asm("_binary_LISTEN_bin_end");
extern const uint8_t THINK_bin_start[] asm("_binary_THINK_bin_start");
extern const uint8_t THINK_bin_end[] asm("_binary_THINK_bin_end");
extern const uint8_t SPEAK_bin_start[] asm("_binary_SPEAK_bin_start");
extern const uint8_t SPEAK_bin_end[] asm("_binary_SPEAK_bin_end");

/* 相位帧的运行时状态：压缩段、解码后缓存(PSRAM)与 CRC。loading 标志避免多个
 * 任务同时触发重复解码。数组下标 = JULIA_AVATAR_DIALOG_* - 1（IDLE 不占位）。 */
typedef struct {
    const char *name;
    const uint8_t *compressed_start;
    const uint8_t *compressed_end;
    uint32_t expected_crc;
    uint16_t *pixels;
    bool decoded;
    bool loading;
    lv_img_dsc_t image;
} avatar_phase_frame_t;

static avatar_phase_frame_t s_phase_frames[] __attribute__((unused)) = {
    [JULIA_AVATAR_DIALOG_LISTENING - 1] = {
        .name = "LISTEN", .compressed_start = LISTEN_bin_start, .compressed_end = LISTEN_bin_end,
        .expected_crc = 0x122fda66U,
    },
    [JULIA_AVATAR_DIALOG_THINKING - 1] = {
        .name = "THINK", .compressed_start = THINK_bin_start, .compressed_end = THINK_bin_end,
        .expected_crc = 0x26dc6a30U,
    },
    [JULIA_AVATAR_DIALOG_SPEAKING - 1] = {
        .name = "SPEAK", .compressed_start = SPEAK_bin_start, .compressed_end = SPEAK_bin_end,
        .expected_crc = 0xe8a17787U,
    },
};

/* 相位名 → 可读字符串（用于日志）。 */
static const char *dialog_phase_name(julia_avatar_dialog_phase_t phase)
{
    switch (phase) {
    case JULIA_AVATAR_DIALOG_IDLE: return "IDLE";
    case JULIA_AVATAR_DIALOG_LISTENING: return "LISTENING";
    case JULIA_AVATAR_DIALOG_THINKING: return "THINKING";
    case JULIA_AVATAR_DIALOG_SPEAKING: return "SPEAKING";
    default: return "INVALID";
    }
}

static bool dialog_phase_is_current(julia_avatar_dialog_phase_t phase)
{
    bool current;
    portENTER_CRITICAL(&s_phase_lock);
    current = s_dialog_phase == phase;
    portEXIT_CRITICAL(&s_phase_lock);
    return current;
}

/* 确保某相位帧已解码：首次调用时分配 PSRAM 缓冲并 RLE 解压、校验 CRC32；
 * 之后直接返回缓存。用 loading 标志防止并发重复解码。解码结果写入 frame->pixels/
 * image。失败时回退到静态立绘（返回值 false，由调用方决定替代源）。 */
static bool __attribute__((unused)) avatar_phase_frame_ensure(avatar_phase_frame_t *frame)
{
    if (frame == NULL) return false;

    portENTER_CRITICAL(&s_phase_lock);
    bool ready = frame->decoded;
    bool loading = frame->loading;
    if (!ready && !loading) frame->loading = true;
    portEXIT_CRITICAL(&s_phase_lock);
    if (ready) return true;
    if (loading) return false;

    uint16_t *pixels = heap_caps_malloc(AVATAR_FRAME_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = pixels == NULL ? ESP_ERR_NO_MEM :
                    avatar_rle_decode_rgb565(frame->compressed_start,
                                              (size_t)(frame->compressed_end - frame->compressed_start),
                                              pixels, AVATAR_FRAME_PIXELS);
    if (err == ESP_OK) {
        uint32_t crc = esp_crc32_le(0, (const uint8_t *)pixels, AVATAR_FRAME_BYTES);
        if (crc != frame->expected_crc) {
            ESP_LOGE(TAG, "%s frame CRC mismatch: got=%08lx expected=%08lx", frame->name,
                     (unsigned long)crc, (unsigned long)frame->expected_crc);
            err = ESP_ERR_INVALID_CRC;
        }
    }

    portENTER_CRITICAL(&s_phase_lock);
    frame->loading = false;
    if (err == ESP_OK) {
        frame->pixels = pixels;
        frame->decoded = true;
        frame->image.header.always_zero = 0;
        frame->image.header.w = AVATAR_FRAME_WIDTH;
        frame->image.header.h = AVATAR_FRAME_HEIGHT;
        frame->image.header.cf = LV_IMG_CF_TRUE_COLOR;
        frame->image.data_size = AVATAR_FRAME_BYTES;
        frame->image.data = (const uint8_t *)pixels;
    }
    portEXIT_CRITICAL(&s_phase_lock);

    if (err != ESP_OK) {
        heap_caps_free(pixels);
        ESP_LOGW(TAG, "%s phase frame unavailable (%s); using base portrait", frame->name,
                 esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "%s phase frame decoded into PSRAM (%u bytes)", frame->name,
             (unsigned)AVATAR_FRAME_BYTES);
    return true;
}

/* LISTEN直接使用完整闭眼立绘；其余对话阶段使用与眼/嘴固定坐标严格对齐的
 * S1.1稳定底图。旧LISTEN/THINK/SPEAK RLE帧继续保留在项目内。 */
static const lv_img_dsc_t *avatar_source_for_phase(julia_avatar_dialog_phase_t phase)
{
    if (phase == JULIA_AVATAR_DIALOG_LISTENING) {
        return &avatar_asset_julia_s0_1_night_sleep;
    }
    return &avatar_asset_julia_s1_1_near_standby;
}

/* LISTEN 的闭眼表情已经完整烘焙在底图中，因此常态隐藏独立眼/嘴层；但 S4
 * 播放唤醒回应时 talking=true，必须临时显示嘴层并继续由 PCM 驱动。 */
static void avatar_apply_phase_eyes(julia_avatar_dialog_phase_t phase)
{
    bool full_closed_portrait = phase == JULIA_AVATAR_DIALOG_LISTENING;
    bool talking;
    portENTER_CRITICAL(&s_state_lock);
    talking = s_talking;
    portEXIT_CRITICAL(&s_state_lock);
    uint8_t eye_main_state = phase == JULIA_AVATAR_DIALOG_IDLE ? 1U : 4U;
    avatar_eyes_set_idle_closed(false);
    avatar_eyes_set_state(eye_main_state);
    avatar_eyes_set_visible(!full_closed_portrait);
    avatar_mouth_set_visible(!full_closed_portrait || talking);
}

/* 把某对话框相位应用到底图。解开锁后由 WSS 任务调用，也可能在 avatar_l1 任务中触发。
 * 设计要点：
 *   - dozing 时直接跳过（睡眠立绘覆盖相位帧）；
 *   - 取源可能触发 RLE 解码（耗时），期间更晚的相位请求可能已把 s_dialog_phase 改掉，
 *     因此在取源后再校验"仍是当前相位"，避免过期请求覆盖最新相位；
 *   - 所有 LVGL 对象访问都在 lvgl_port_lock 内；锁超时视为未应用，可安全重试。 */
static bool avatar_apply_dialog_phase(julia_avatar_dialog_phase_t phase)
{
    portENTER_CRITICAL(&s_phase_lock);
    bool dozing = s_dozing;
    portEXIT_CRITICAL(&s_phase_lock);
    if (dozing) return false;
    const lv_img_dsc_t *source = avatar_source_for_phase(phase);
    /* Decoding can take long enough for a later transport command to supersede
     * this request. Never let a stale request overwrite the latest phase. */
    if (!s_base || !dialog_phase_is_current(phase)) return false;
    if (!lvgl_port_lock(pdMS_TO_TICKS(250))) {
        ESP_LOGW(TAG, "LVGL lock timeout applying %s phase", dialog_phase_name(phase));
        return false;
    }
    bool applied = false;
    if (dialog_phase_is_current(phase)) {
        lv_img_set_src(s_base, source);
        lv_obj_invalidate(s_base);
        portENTER_CRITICAL(&s_phase_lock);
        if (s_dialog_phase == phase) {
            s_applied_dialog_phase = phase;
            applied = true;
        }
        portEXIT_CRITICAL(&s_phase_lock);
    }
    lvgl_port_unlock();
    if (applied && dialog_phase_is_current(phase)) {
        avatar_apply_phase_eyes(phase);
    }
    return applied;
}

/* 整体切换到/退出睡眠立绘：进入睡眠时换到生成的睡眠图并隐藏眼/嘴层；
 * 退出时回到当前相位帧并恢复眼/嘴层。锁超时回滚 s_dozing，保证状态与实际画面一致。 */
void julia_avatar_set_dozing(bool active)
{
    portENTER_CRITICAL(&s_phase_lock);
    bool previous_dozing = s_dozing;
    bool changed = previous_dozing != active;
    s_dozing = active;
    julia_avatar_dialog_phase_t phase = s_dialog_phase;
    portEXIT_CRITICAL(&s_phase_lock);
    if (!changed || !s_base) return;

    const lv_img_dsc_t *source = active
                                     ? &avatar_asset_julia_s0_1_night_sleep
                                     : avatar_source_for_phase(phase);
    if (!lvgl_port_lock(pdMS_TO_TICKS(250))) {
        portENTER_CRITICAL(&s_phase_lock);
        s_dozing = previous_dozing;
        portEXIT_CRITICAL(&s_phase_lock);
        ESP_LOGW(TAG, "LVGL lock timeout switching doze=%u", active ? 1U : 0U);
        return;
    }
    lv_img_set_src(s_base, source);
    lv_obj_t *layers[] = {
        avatar_eyes_left_object(), avatar_eyes_right_object(), avatar_mouth_object(),
    };
    for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); ++i) {
        if (layers[i] == NULL) continue;
        if (active) lv_obj_add_flag(layers[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(layers[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(layers[i]);
    }
    lv_obj_invalidate(s_base);
    lvgl_port_unlock();
    if (!active) {
        portENTER_CRITICAL(&s_phase_lock);
        s_applied_dialog_phase = phase;
        portEXIT_CRITICAL(&s_phase_lock);
        avatar_apply_phase_eyes(phase);
    }
    ESP_LOGI(TAG, "portrait=%s", active ? "sleep" : dialog_phase_name(phase));
}

/* 整数平方根（用于求 RMS）：逐位逼近，避免在音频回调里用浮点 sqrt。
 * 返回 value 的整数平方根。 */
static uint32_t integer_sqrt_u64(uint64_t value)
{
    uint64_t bit = 1ULL << 62;
    uint64_t result = 0;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

/* 由一帧 PCM 计算嘴型档位(0..3)：先算 RMS，再做 EMA 平滑，然后按"上升快、下降慢"
 * 的非对称门限单向升/降档。非对称 + 单步升降可避免在门限附近抖动（chatter），
 * 与 fused 的 julia_lipsync 行为一致。 */
static uint8_t mouth_level_for_frame(const int16_t *samples, size_t count)
{
    uint64_t energy = 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t sample = samples[i];
        energy += (uint64_t)(sample * sample);
    }
    uint32_t rms = count ? integer_sqrt_u64(energy / count) : 0;

    /* Same asymmetric thresholds as the fused lipsync module: fast attack,
     * slower release, and one-level steps avoid chatter around a boundary. */
    s_smoothed_rms = (s_smoothed_rms * 5U + rms * 3U) / 8U;
    static const uint16_t rise[] = {300, 950, 2300};
    static const uint16_t fall[] = {180, 650, 1650};
    uint8_t level = s_mouth_level;
    if (level < 3U && s_smoothed_rms >= rise[level]) {
        ++level;
    } else if (level > 0U && s_smoothed_rms < fall[level - 1U]) {
        --level;
    }
    s_mouth_level = level;
    return level;
}

/* 喂入一帧下行扬声器 PCM：计算嘴型档位并记录"最近一次音频时刻"。由 voice_service
 * 的 WSS 任务调用。只做整字段更新（锁内），不在音频回调里碰 LVGL。 */
void julia_avatar_feed_pcm(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0U) {
        return;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    portENTER_CRITICAL(&s_state_lock);
    /* Playback now feeds from another task; serialize the RMS smoother with
     * talking_start/stop and never let a cancelled chunk reopen the mouth. */
    if (!s_talking) {
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    uint8_t level = mouth_level_for_frame(samples, sample_count);
    s_target_mouth_level = level;
    s_last_pcm_ms = now_ms;
    portEXIT_CRITICAL(&s_state_lock);
}

/* 语音下行开始：置位 talking，清平滑器与目标档位。安全：可在 UI 初始化前调用
 * （此时只更新静态字段，不触碰 LVGL）。 */
void julia_avatar_talking_start(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_talking = true;
    s_smoothed_rms = 0;
    s_mouth_level = 0;
    s_target_mouth_level = 0;
    s_last_pcm_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    portEXIT_CRITICAL(&s_state_lock);
    /* S4 使用 LISTENING 闭眼底图，常态会隐藏独立嘴层；开播时显式恢复嘴层。 */
    if (s_ready) avatar_mouth_set_visible(true);
}

/* 语音下行结束：清除说话状态并把嘴立刻闭合（不等下一个 40ms 节拍）。 */
void julia_avatar_talking_stop(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_talking = false;
    s_smoothed_rms = 0;
    s_mouth_level = 0;
    s_target_mouth_level = 0;
    portEXIT_CRITICAL(&s_state_lock);
    /* Close immediately on SPKE/session teardown rather than waiting for the
     * next 40 ms lip-sync tick. */
    if (s_ready) {
        avatar_mouth_set_shape(AVATAR_MOUTH_IDLE, 0);
        portENTER_CRITICAL(&s_phase_lock);
        julia_avatar_dialog_phase_t phase = s_dialog_phase;
        bool dozing = s_dozing;
        portEXIT_CRITICAL(&s_phase_lock);
        avatar_mouth_set_visible(!dozing && phase != JULIA_AVATAR_DIALOG_LISTENING);
    }
}

/* 设置对话框相位。may be called from WSS/command tasks；相位未变或已应用则忽略
 * 重复请求（needs_apply 判断），避免重复把同一张底图 set 一遍。 */
void julia_avatar_set_dialog_phase(julia_avatar_dialog_phase_t phase)
{
    if (phase < JULIA_AVATAR_DIALOG_IDLE || phase > JULIA_AVATAR_DIALOG_SPEAKING) {
        ESP_LOGW(TAG, "Ignoring invalid UI phase %d", (int)phase);
        return;
    }
    julia_avatar_dialog_phase_t previous;
    bool needs_apply;
    portENTER_CRITICAL(&s_phase_lock);
    previous = s_dialog_phase;
    if (previous != phase) s_dialog_phase = phase;
    needs_apply = previous != phase || s_applied_dialog_phase != phase;
    portEXIT_CRITICAL(&s_phase_lock);

    if (previous != phase) {
        ESP_LOGI(TAG, "UI phase: %s -> %s", dialog_phase_name(previous), dialog_phase_name(phase));
    }
    if (needs_apply) avatar_apply_dialog_phase(phase);
}

julia_avatar_dialog_phase_t julia_avatar_get_dialog_phase(void)
{
    julia_avatar_dialog_phase_t phase;
    portENTER_CRITICAL(&s_phase_lock);
    phase = s_dialog_phase;
    portEXIT_CRITICAL(&s_phase_lock);
    return phase;
}

/* 微动：对根对象做轻微缩放(呼吸)与细角度点头。该逻辑在
 * AVATAR_ENABLE_FULL_FRAME_MOTION=0 时被整体裁掉（见文件上方宏说明——QSPI 面板
 * 对整帧变换会产生撕裂/闪烁，故仅保留眼/嘴局部动画）。 */
static void update_micro_motion(uint32_t now_ms)
{
#if AVATAR_ENABLE_FULL_FRAME_MOTION
    if (!s_motion_root) {
        return;
    }

    /* A tiny zoom pulse reads as breathing without exposing the screen edge. */
    uint32_t breath_phase = now_ms % AVATAR_BREATH_PERIOD_MS;
    uint32_t half = AVATAR_BREATH_PERIOD_MS / 2U;
    uint32_t triangle = breath_phase <= half ? breath_phase : AVATAR_BREATH_PERIOD_MS - breath_phase;
    lv_coord_t zoom = (lv_coord_t)(256U + (triangle * 2U + half / 2U) / half);

    /* Periodic sub-degree forward/back motion gives a restrained idle nod. */
    uint32_t nod_phase = now_ms % AVATAR_NOD_PERIOD_MS;
    int16_t angle = 0;
    if (nod_phase < AVATAR_NOD_DURATION_MS) {
        uint32_t quarter = AVATAR_NOD_DURATION_MS / 4U;
        if (nod_phase < quarter) {
            angle = (int16_t)(-(int32_t)nod_phase * 6 / (int32_t)quarter);
        } else if (nod_phase < quarter * 2U) {
            angle = (int16_t)(-6 + (int32_t)(nod_phase - quarter) * 6 / (int32_t)quarter);
        } else if (nod_phase < quarter * 3U) {
            angle = (int16_t)((int32_t)(nod_phase - quarter * 2U) * 4 / (int32_t)quarter);
        } else {
            angle = (int16_t)(4 - (int32_t)(nod_phase - quarter * 3U) * 4 / (int32_t)quarter);
        }
    }

    lv_obj_set_style_transform_zoom(s_motion_root, zoom, LV_PART_MAIN);
    lv_obj_set_style_transform_angle(s_motion_root, angle, LV_PART_MAIN);
#else
    (void)now_ms;
#endif
}

/* L1 micro-motion 任务（优先级 3，栈 4096，PSRAM）：每 40ms 读一次嘴型状态，
 * 若非说话或超过 PCM 保持期则目标档位置 0（闭嘴），否则用目标档位驱动嘴型层；
 * 同时跑 update_micro_motion()。所有 LVGL 对象访问在 lvgl_port_lock 内。 */
static void avatar_task(void *argument)
{
    (void)argument;
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        bool talking;
        uint8_t target;
        uint32_t last_pcm;
        julia_avatar_dialog_phase_t phase;
        bool dozing;
        portENTER_CRITICAL(&s_state_lock);
        talking = s_talking;
        target = s_target_mouth_level;
        last_pcm = s_last_pcm_ms;
        portEXIT_CRITICAL(&s_state_lock);
        portENTER_CRITICAL(&s_phase_lock);
        phase = s_dialog_phase;
        dozing = s_dozing;
        portEXIT_CRITICAL(&s_phase_lock);

        if (!talking || (uint32_t)(now_ms - last_pcm) > AVATAR_PCM_HOLD_MS) {
            target = 0;
        }

        if (lvgl_port_lock(pdMS_TO_TICKS(20))) {
            update_micro_motion(now_ms);
            /* S6→S4 的底图切换和 SPKS 可能并发；每个节拍重新校正显隐，避免
             * talking_start 的一次性 LVGL 锁失败让整段唤醒回应都没有嘴型。 */
            bool mouth_visible = !dozing &&
                                 (phase != JULIA_AVATAR_DIALOG_LISTENING || talking);
            avatar_mouth_set_visible(mouth_visible);
            /* avatar_mouth 自身按当前 shape 去重；这里不再维护第二份缓存，避免
             * talking_stop 强制闭嘴后，新一轮相同档位被错误跳过。 */
            avatar_mouth_set_shape((avatar_mouth_shape_t)target, s_smoothed_rms);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(AVATAR_UPDATE_MS));
    }
}

/* 构建并启动 L1 立绘链：在 LVGL 锁内建屏幕/根对象/底图（静态立绘）/眼/嘴，
 * 应用已请求的相位，同步刷新首帧（此时背光尚未开——见 main.c：首帧渲染完成后才开
 * 背光），最后创建 avatar_l1 任务。幂等（s_ready 已置真则直接返回 OK）。 */
esp_err_t julia_avatar_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) {
        return ESP_ERR_TIMEOUT;
    }

    /* 屏幕底色 + 全幅父容器。 */
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_motion_root = lv_obj_create(screen);
    lv_obj_set_size(s_motion_root, 360, 360);
    lv_obj_set_pos(s_motion_root, 0, 0);
    lv_obj_set_style_pad_all(s_motion_root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_motion_root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_motion_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(s_motion_root, 180, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(s_motion_root, 240, LV_PART_MAIN);
    lv_obj_clear_flag(s_motion_root, LV_OBJ_FLAG_SCROLLABLE);

    s_base = lv_img_create(s_motion_root);
    lv_img_set_src(s_base, &avatar_asset_julia_s1_1_near_standby);
    lv_obj_set_pos(s_base, 0, 0);
    lv_obj_clear_flag(s_base, LV_OBJ_FLAG_SCROLLABLE);

    avatar_eyes_init(s_motion_root);
    avatar_mouth_init(s_motion_root);

    /* 状态叠字固定在屏幕坐标系，不挂到微动根对象，避免随立绘缩放或点头移动。 */
    s_status_label = lv_label_create(screen);
    status_label_place();
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_status_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_SCROLLABLE);
    char status_snapshot[sizeof(s_status_text)];
    portENTER_CRITICAL(&s_phase_lock);
    memcpy(status_snapshot, s_status_text, sizeof(status_snapshot));
    portEXIT_CRITICAL(&s_phase_lock);
    lv_label_set_text(s_status_label, status_snapshot);
    status_label_place();

    s_offline_label = lv_label_create(screen);
    lv_label_set_text(s_offline_label, "offline");
    lv_obj_set_pos(s_offline_label, STATUS_LABEL_X, OFFLINE_LABEL_Y);
    lv_obj_set_width(s_offline_label, STATUS_LABEL_WIDTH);
    lv_obj_set_style_text_color(s_offline_label, lv_palette_main(LV_PALETTE_RED),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_offline_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_offline_label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(s_offline_label, LV_OBJ_FLAG_SCROLLABLE);
    if (!s_offline) lv_obj_add_flag(s_offline_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_offline_label);
    ESP_LOGI(TAG, "status label ready x=%d y=%d width=%d text=%s",
             lv_obj_get_x(s_status_label), lv_obj_get_y(s_status_label),
             lv_obj_get_width(s_status_label), status_snapshot);
    lv_obj_invalidate(screen);
    lvgl_port_unlock();

    /* A phase can be requested before display initialisation; honour it once
     * the one and only avatar object tree exists. */
    avatar_apply_dialog_phase(julia_avatar_get_dialog_phase());

    esp_err_t refresh_err = lvgl_port_refr_now_sync(pdMS_TO_TICKS(1000));
    if (refresh_err != ESP_OK) {
        ESP_LOGW(TAG, "first portrait refresh reported: %s", esp_err_to_name(refresh_err));
    }

    if (xTaskCreateWithCaps(avatar_task, "avatar_l1", 4096, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "Julia portrait ready: stable frame + blink + 4-level RMS mouth");
    return ESP_OK;
}

bool julia_avatar_is_ready(void)
{
    return s_ready;
}

static esp_err_t boot_eye_frame(avatar_eyes_frame_t frame, uint32_t hold_ms)
{
    avatar_eyes_show(frame);
    esp_err_t err = lvgl_port_refr_now_sync(pdMS_TO_TICKS(500));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot eye frame %u refresh failed: %s", (unsigned)frame,
                 esp_err_to_name(err));
        return err;
    }
    if (hold_ms > 0U) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }
    return ESP_OK;
}

esp_err_t julia_avatar_play_boot_sequence(void)
{
    if (s_boot_sequence_played) {
        return ESP_OK;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Hold off the random blink task for the complete deterministic sequence.
     * The backlight is still at 0 here, so the first visible frame is closed. */
    avatar_eyes_set_idle_closed(true);
    ESP_RETURN_ON_ERROR(lvgl_port_refr_now_sync(pdMS_TO_TICKS(500)),
                        TAG, "refresh closed boot frame");

    esp_err_t fade_err = julia_backlight_fade_to(100, 300);
    if (fade_err == ESP_OK) {
        fade_err = julia_backlight_wait_fade(500);
    }
    if (fade_err != ESP_OK) {
        ESP_LOGW(TAG, "Boot backlight fade failed: %s; using full brightness",
                 esp_err_to_name(fade_err));
        julia_backlight_set(100);
    }
    esp_err_t sequence_err = ESP_OK;
    for (unsigned i = 0; i < BOOT_BLINK_COUNT; ++i) {
        if (boot_eye_frame(AVATAR_EYES_OPEN, BOOT_BLINK_OPEN_MS) != ESP_OK) {
            sequence_err = ESP_FAIL;
        }
        if (boot_eye_frame(AVATAR_EYES_CLOSED, BOOT_BLINK_CLOSED_MS) != ESP_OK) {
            sequence_err = ESP_FAIL;
        }
    }

    /* Releasing idle_closed also applies the final open frame and lets the
     * existing 3-8 second random blink task resume normally. */
    avatar_eyes_set_idle_closed(false);
    if (lvgl_port_refr_now_sync(pdMS_TO_TICKS(500)) != ESP_OK) {
        sequence_err = ESP_FAIL;
    }
    s_boot_sequence_played = true;
    ESP_LOGI(TAG, "Boot eye sequence complete: %u rapid blinks in about 3 seconds",
             (unsigned)BOOT_BLINK_COUNT);
    return sequence_err;
}
