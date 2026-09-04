/**
 * @file    avatar_micro_motion.c
 * @brief   未参与当前构建的分层微动作参考实现。
 *
 * 当前运行时使用 julia_avatar.c 的受限动画路径；本文件依赖未接入的旧 UI 对象模型，
 * 下述调度和锁关系不能套用到现有立绘。
 *
 * 模块边界：
 *   - 本文件是“何时动、往哪动”的引擎；它通过绑定图层直接写 LVGL 对象的 transform/position。
 *     立绘静态底图与眼睛/嘴部资源切换（bin→lv_img_dsc_t）在 avatar_parts/ 与本文件的
 *     scheduled frame 切换里共同完成；总控（FSM 状态投递、RMS→嘴型、转场决策）在 julia_ui.c。
 *   - 与 julia_avatar.c 的“整屏 root 动画”互为两套方案：本文件只动局部图层（眼睛/瞳孔/嘴/容器），
 *     避免整幅 360×360 画布在无 TE 的 QSPI 面板上持续撕裂（见 julia_avatar.c
 *     AVATAR_ENABLE_FULL_FRAME_MOTION=0 的说明）。
 *
 * 时间驱动：主循环以 ~40ms（UPDATE_MS）调用 update_avatar(now_ms)。now_ms 由调用方取
 * esp_timer_get_time()/1000 传入；本模块不自建 FreeRTOS 定时器，只用绝对毫秒时间戳比较。
 *
 * 线程模型：
 *   - update_avatar() 在调用方（主循环/普通任务）里跑；写图层前用 lvgl_port_lock(pdMS_TO_TICKS(4))
 *     取锁，取不到就丢弃本帧（绝不阻塞调用方）。它读 s.state/s.phase/nodding 等字段时不持锁，
 *     而这些字段由持 LVGL 锁的 julia_ui state_worker_task 写入——存在理论上的并发读，
 *     见 update_avatar 的 NOTE。
 *   - 呼吸/头颈无限重复动画经 LVGL lv_anim 在 LVGL 任务里执行，其姿态字段（pose_*）在
 *     avatar_motion_transition_main（持 LVGL 锁）写入，故与动画回调串行。
 *   - 本文件自身不创建任何任务（无调度器），只做纯计算并写对象。
 *
 * 上游/下游：
 *   - 上游：avatar_micro_motion.h 声明、julia_ui.c（init/set_state/set_dialog_phase/
 *     transition_main/avatar_show_all）、主循环（update_avatar/on_user_interaction）。
 *   - 下游：avatar_parts 部件对象、avatar_layer_assets.h（avatar_layer_eye/mouth 资源句柄）、
 *     julia_display_theme.h（reset_idle_timer）、lvgl_port.h（锁与 flush 指标）、
 *     esp_heap_caps（内存诊断）、esp_wdt（喂狗）。
 */
#include "avatar_micro_motion.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "lvgl_port.h"
#include "avatar_layer_assets.h"
#include "julia_display_theme.h"

/* 主循环调用 update_avatar 的最小节拍：低于该间隔的调用被合并（帧数据不会更精细）。 */
#define UPDATE_MS 40U
/* 内存/性能日志节拍：每隔此行打印一次免费内存与 flush 帧率，避免刷屏。 */
#define MOTION_LOG_MS 1000U
#define MOTION_TAG "MOTION"
#define EVENT_TAG "AVATAR_EVENT"
/* Q10 定点数：1.0 对应 1024，用于把缓动系数与步进精确到 1/1024，避免浮点。 */
#define Q10_ONE 1024
/* 显示稳定性热修复开关（见 avatar_motion_resume_all/transition_main 的分支注释）。 */
#define DISPLAY_STABILITY_HOTFIX 1
#ifndef AVATAR_LAYER_DEBUG
#define AVATAR_LAYER_DEBUG 0
#endif

/* 瞳孔随动的阶段状态机。
 *   GAZE_CENTER：瞳孔在原点，等待 next_gaze 触发；
 *   GAZE_OUT：   缓慢滑向目标偏移（ease_out_q10）；
 *   GAZE_HOLD：  停留在目标偏移（随机 800~2000ms）；
 *   GAZE_BACK：  缓慢滑回原点（ease_in_out_q10 的反向）。 */
typedef enum { GAZE_CENTER, GAZE_OUT, GAZE_HOLD, GAZE_BACK } gaze_stage_t;
/* 各图层在 init 时刻记录下的“初始位置”，作为微动的基准原点。 */
typedef struct { lv_coord_t x; lv_coord_t y; } origin_t;

/* ---- 引擎唯一实例（模块级单例）。所有函数只操作它，不分配文件/动态内存。 ---- */
static struct {
    /* 绑定的图层对象（允许 NULL，写前判空）。 */
    avatar_layer_bindings_t layer;
    /* 各图层初始位置（left/right eye、left/right pupil、mouth、hair_front），做微动基准。 */
    origin_t le, re, lp, rp, mouth, hair;
    /* 当前子状态（S0~S5）与对话相位（0=IDLE,1=LISTEN,2=THINK,3=SPEAK）。 */
    julia_sub_state_t state;
    uint8_t phase;
    /* 外部叠加的“暂停计数”：talking 期间 pause/resume 成对增减，>0 视为高优先级锁定。 */
    uint8_t external_pause_count;
    uint32_t last_update;      /* 上一次真正执行 tick 的时刻（最新 now_ms）。 */
    uint32_t last_log;         /* 上一次打性能日志的时刻。 */
    uint64_t last_flush_count; /* 上一次读到的 LVGL flush 计数，用于计算 flush_fps。 */
    uint32_t flush_fps;        /* 近 1s 内 LVGL flush 帧率（诊断用）。 */
    /* 随机事件的时间表（绝对毫秒）。
     *   next_gaze：下一次启动瞳孔扫描；next_nod：下一次点头；
     *   next_think_gaze：THINK 相位下一次换目标；next_rare：哈欠/发梢等低频动作。 */
    uint32_t next_gaze, gaze_started, gaze_duration;
    uint32_t next_nod, nod_started;
    uint32_t next_think_gaze;
    uint32_t next_rare, rare_started;
    size_t minimum_free_psram;      /* 观测到的 PSRAM 最低空闲字节（泄漏诊断）。 */
    int8_t gaze_x, gaze_y;          /* 当前 gaze 目标偏移（有符号像素）。 */
    gaze_stage_t gaze_stage;        /* 瞳孔扫描的阶段。 */
    bool initialized, suspended, nodding, rare_hair;
    bool breathe_running, head_running, neck_running; /* 呼吸/头/颈动画各自是否在跑。 */
    /* 主状态转场姿势：在重复微动基础上叠加的一次性偏移（角度/缩放/呼吸幅度）。 */
    int16_t pose_head_angle, pose_neck_angle;
    uint16_t pose_zoom;
    uint8_t pose_breath_amplitude;
    uint8_t left_eye_frame, right_eye_frame; /* 当前眼睛帧索引，用于跳过重复 set_src。 */
    julia_main_state_t transition_target;    /* 上一次主状态转场的终点（转场完成回调用）。 */
} s;
/* 全局动画开关（_all 系列）：0 表示允许一切微动，1 表示所有图层动画冻结。 */
static volatile bool s_all_paused;

/* 从候选帧资源中挑一张“有效”的图：先校验 candidate，非法则回退 fallback。
 * 有效性由 avatar_layer_asset_valid() 判定（data 指针/宽高/format/大小与描述一致）。
 * 失败路径：两者都无效时返回 NULL；调用方需在接受 NULL 的前提下才继续（如嘴型切换）。 */
static const lv_img_dsc_t *valid_layer_or_fallback(const char *name,
                                                    const lv_img_dsc_t *candidate,
                                                    const lv_img_dsc_t *fallback)
{
    if (avatar_layer_asset_valid(candidate)) return candidate;
    ESP_LOGW("AVATAR_LAYER", "%s frame invalid data=%p; falling back", name,
             candidate ? candidate->data : NULL);
    return avatar_layer_asset_valid(fallback) ? fallback : NULL;
}

/* 复位单个图片对象的所有旋转/缩放/枢轴，避免残留上一帧的 transform。 */
static void reset_image_transform(lv_obj_t *image)
{
    if (!image) return;
    lv_anim_del(image, NULL);
    lv_obj_set_style_transform_angle(image, 0, 0);
    lv_obj_set_style_transform_zoom(image, 256, 0);
    lv_obj_set_style_transform_pivot_x(image, 0, 0);
    lv_obj_set_style_transform_pivot_y(image, 0, 0);
    lv_img_set_angle(image, 0);
    lv_img_set_zoom(image, 256);
    lv_img_set_pivot(image, 0, 0);
}

/* 复位所有会被显示稳定性微动影响的容器/图层变换（DISPLAY_STABILITY_HOTFIX 用）。
 * 目的：把整幅画布“打回”静止的基准姿态，保证不残留任何造成面板撕裂的变换残留。 */
static void reset_stability_transforms(void)
{
    lv_obj_t *containers[] = {s.layer.container, s.layer.neck, s.layer.head};
    for (unsigned i = 0; i < sizeof(containers) / sizeof(containers[0]); ++i) {
        if (!containers[i]) continue;
        lv_obj_set_style_transform_angle(containers[i], 0, 0);
        lv_obj_set_style_transform_zoom(containers[i], 256, 0);
        lv_obj_set_style_transform_pivot_x(containers[i], 0, 0);
        lv_obj_set_style_transform_pivot_y(containers[i], 0, 0);
    }
    reset_image_transform(s.layer.left_eye);
    reset_image_transform(s.layer.right_eye);
    reset_image_transform(s.layer.left_pupil);
    reset_image_transform(s.layer.right_pupil);
    reset_image_transform(s.layer.mouth);
}

/* 在 [low, high] 闭区间内取整随机数（含端点）。用 esp_random() 而非 rand()，保证熵源与可重复性。
 * 边界：high<=low 时直接返回 low，避免取模除零/负数。 */
static uint32_t random_between(uint32_t low, uint32_t high)
{
    if (high <= low) return low;
    return low + esp_random() % (high - low + 1U);
}

/* 读取对象当前坐标作为微动基准点；对象为 NULL 时返回 {0,0}（后续 move 会因 obj 判空跳过）。 */
static origin_t origin(lv_obj_t *obj)
{
    origin_t result = {0, 0};
    if (obj) { result.x = lv_obj_get_x(obj); result.y = lv_obj_get_y(obj); }
    return result;
}

/* 把耗时进度归一化到 Q10 [0..1024]：elapsed>=duration 或 duration==0 都饱和到 1.0。
 * 返回（且函数签名声明）int32_t，但其值域仅覆盖 [0..1024]，供缓动系数使用。 */
static int32_t clamp_q10(uint32_t elapsed, uint32_t duration)
{
    if (!duration || elapsed >= duration) return Q10_ONE;
    return (int32_t)(elapsed * Q10_ONE / duration);
}

/* 标准 ease-in-out（Q10，三次对称）：起/止缓动对称、中心穿过 0.5。 */
static int32_t ease_in_out_q10(int32_t t)
{
    if (t <= 0) return 0;
    if (t >= Q10_ONE) return Q10_ONE;
    if (t < Q10_ONE / 2) return (2 * t * t) / Q10_ONE;
    int32_t inverse = Q10_ONE - t;
    return Q10_ONE - (2 * inverse * inverse) / Q10_ONE;
}

/* ease-out（Q10）：快起慢收，用于瞳孔“快速滑出、缓慢到位”。 */
static int32_t ease_out_q10(int32_t t)
{
    int32_t inverse = Q10_ONE - t;
    return Q10_ONE - (inverse * inverse) / Q10_ONE;
}

/* 在基准点 at 上叠加 Q10 位移（dx/dy），并 set_pos 到新位置。
 * 副作用：若新旧坐标不同才写对象，避免同一帧重复 set_pos 造成无效重绘。 */
static void move(lv_obj_t *obj, origin_t at, int32_t dx_q10, int32_t dy_q10)
{
    if (!obj) return;
    lv_coord_t x = at.x + (lv_coord_t)(dx_q10 / Q10_ONE);
    lv_coord_t y = at.y + (lv_coord_t)(dy_q10 / Q10_ONE);
    if (lv_obj_get_x(obj) != x || lv_obj_get_y(obj) != y) lv_obj_set_pos(obj, x, y);
}

/* ---- 状态谓词：判定当前处于哪类行为档位，从而决定放行哪些微动 ---- */
/* 睡眠(S0)：夜间/白天离席/手动休眠。睡眠时应闭眼、隐藏瞳孔。 */
static bool sleeping(void) { return s.state <= JULIA_SUB_STATE_S0_3_MANUAL_SLEEP; }
/* 待机(S1)且相位=IDLE：唯一允许“纯 idle 呼吸/微晃”的档位（s.phase==0 保证非对话）。 */
static bool idle_state(void)
{
    return s.state >= JULIA_SUB_STATE_S1_1_NEAR_STANDBY &&
           s.state <= JULIA_SUB_STATE_S1_3_CHARGING_STANDBY && s.phase == 0;
}
/* 对话相位=LISTENING。 */
static bool listening(void) { return s.phase == 1; }
/* 高优先级锁定：外部暂停计数>0，或正在点头，或正在 SPEAK。此时压低一切“无关微动”。 */
static bool high_priority(void) { return s.external_pause_count || s.nodding || s.phase == 3; }
/* 是否允许低幅微动（缩放/角度/凝视）。需同时满足：已 init、未全局暂停、未挂起、非高优先级，
 * 且处于待机或聆听档位。这是“为什么同一时刻只做一种微动”的闸门：说话/转场/暂停时全部收敛。 */
static bool low_motion_allowed(void)
{
    return s.initialized && !s_all_paused && !s.suspended && !high_priority() &&
           (idle_state() || listening());
}

/* 供日志打印当前“动作名”，帮助串口观察引擎正在执行哪种微动。 */
static const char *active_action(void)
{
    if (s_all_paused || s.suspended) return "suspended";
    if (s.phase == 3) return "speak";
    if (s.nodding) return "nod";
    if (s.external_pause_count) return "blink";
    if (s.gaze_stage != GAZE_CENTER) return "scan";
    return idle_state() || listening() ? "breathe" : "still";
}

/* 打印一次内存/FPS 诊断日志，并更新 PSRAM 最低空闲记录（用于捕获泄漏趋势）。 */
static void log_memory(const char *edge, const char *function)
{
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (!s.minimum_free_psram || psram < s.minimum_free_psram) s.minimum_free_psram = psram;
    ESP_LOGI(MOTION_TAG, "%s=%s action=%s heap=%u psram=%u largest=%u flush_fps=%u",
             edge, function, active_action(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)psram,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)s.flush_fps);
}

/* 呼吸缩放回调：在基准 zoom(256) 上按当前呼吸幅度叠加正负 delta。
 * 说话(LISTEN)时幅度减半，避免喧宾夺主。低幅微动被允许时才叠加。 */
static void set_transform_zoom(void *object, int32_t value)
{
    lv_obj_t *container = object;
    if (!container) return;
    int32_t zoom = s.pose_zoom ? s.pose_zoom : 256;
    if (low_motion_allowed()) {
        int32_t delta = (value - 256) * s.pose_breath_amplitude / 4;
        zoom += listening() ? delta / 2 : delta;
    }
    lv_obj_set_style_transform_zoom(container, (lv_coord_t)zoom, 0);
}

/* 头部角度回调：叠加微晃 value 与转场姿势 pose_head_angle；低幅微动关闭时只用姿势。 */
static void set_head_angle(void *object, int32_t value)
{
    lv_obj_t *container = object;
    if (container) lv_obj_set_style_transform_angle(container,
        (lv_coord_t)(s.pose_head_angle + (low_motion_allowed() ? value : 0)), 0);
}

/* 颈部角度回调：与头部类似，叠加 pose_neck_angle。 */
static void set_neck_angle(void *object, int32_t value)
{
    lv_obj_t *container = object;
    if (container) lv_obj_set_style_transform_angle(container,
        (lv_coord_t)(s.pose_neck_angle + (low_motion_allowed() ? value : 0)), 0);
}

/* 启动一个“去程 out_ms、回程 back_ms、延迟 delay_ms、无限重复”的缓动动画。
 * 副作用：绑定 lv_anim 到 LVGL 任务，回调里直接读写容器样式；不持锁（应在 LVGL 锁内调用）。 */
static void start_repeating_anim(lv_obj_t *object, lv_anim_exec_xcb_t callback,
                                 int32_t from, int32_t to, uint32_t out_ms,
                                 uint32_t back_ms, uint32_t delay_ms)
{
    if (!object || !callback) return;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, callback);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_time(&animation, out_ms);
    lv_anim_set_playback_time(&animation, back_ms);
    lv_anim_set_delay(&animation, delay_ms);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_start(&animation);
}

/* 呼吸动画：给 container 挂一个循环缩放（zoom 256→260→回），枢轴固定在 (180,270)（胸口位置），
 * 呈现“微微起伏”的呼吸感。去程 1500ms、回程随机 1500~2500ms，制造非机械的呼吸节奏。
 * 副作用：会启动无限重复的 lv_anim；应由调用方在 LVGL 锁内调用。 */
void avatar_breathe_init(lv_obj_t *container)
{
    log_memory("enter", __func__);
    s.layer.container = container;
    if (container) {
        lv_obj_set_style_transform_pivot_x(container, 180, 0);
        lv_obj_set_style_transform_pivot_y(container, 270, 0);
        start_repeating_anim(container, set_transform_zoom, 256, 260, 1500,
                             random_between(1500, 2500), 0);
        s.breathe_running = true;
    }
    log_memory("exit", __func__);
}

/* 头部微晃：head 容器绕 (180,225) 在 -20°~+20° 之间缓动旋转，周期 2×2500ms（去回各 2.5s）。
 * 与呼吸共用一个“低频”骨架，营造头部轻摆的待机感。 */
void avatar_head_init(lv_obj_t *container)
{
    log_memory("enter", __func__);
    s.layer.head = container;
    if (container) {
        lv_obj_set_style_transform_pivot_x(container, 180, 0);
        lv_obj_set_style_transform_pivot_y(container, 225, 0);
        start_repeating_anim(container, set_head_angle, -20, 20, 2500, 2500, 0);
        s.head_running = true;
    }
    log_memory("exit", __func__);
}

/* 颈部微晃：neck 容器绕 (180,245) 在 -10°~+10° 之间旋转，去回各 2.5s，起始延迟 500ms。
 * 幅度比头部小、相位略滞后，形成头部先行、颈部随后的层次感。 */
void avatar_neck_init(lv_obj_t *container)
{
    log_memory("enter", __func__);
    s.layer.neck = container;
    if (container) {
        lv_obj_set_style_transform_pivot_x(container, 180, 0);
        lv_obj_set_style_transform_pivot_y(container, 245, 0);
        start_repeating_anim(container, set_neck_angle, -10, 10, 2500, 2500, 500);
        s.neck_running = true;
    }
    log_memory("exit", __func__);
}

/* 外部“单次暂停/恢复”：以计数器支持多个调用方叠加（例如说话、转场各自 pause 一次）。 */
void avatar_motion_pause(void)
{
    /* 防溢出：不计到 UINT8_MAX 之后，避免回绕导致永远暂停。 */
    if (s.external_pause_count < UINT8_MAX) ++s.external_pause_count;
}

void avatar_motion_resume(void)
{
    if (s.external_pause_count) --s.external_pause_count;
}

/* 重排所有随机事件的时间表，并复位一次性动作标志（点头/发梢）。用于状态或相位切换后，
 * 让新档位从“干净、未知起点”重新规划下一次触发时刻。 */
static void schedule_for_state(uint32_t now)
{
    s.gaze_stage=GAZE_CENTER; s.gaze_x=0; s.gaze_y=0;
    s.next_gaze=now+random_between(2500,6000);
    s.next_nod=now+random_between(2000,3500);
    s.next_think_gaze=now+random_between(4000,7000);
    s.next_rare=now+random_between(15000,40000);
    s.nodding=false; s.rare_hair=false;
}

/* 绑定图层并初始化引擎。这里是唯一清零 s 的地方；绑定之后即进入“已初始化但挂起”的待启动态，
 * 等待 avatar_show_all() 调用 avatar_motion_resume_all() 放行动画。
 * 副作用：读取各图层的初始坐标作为微动基准，并复位稳定性变换（若 HOTFIX 开启）。 */
void avatar_micro_motion_init(const avatar_layer_bindings_t *layers)
{
    memset(&s,0,sizeof(s));
    s_all_paused = true;
    if (layers) s.layer=*layers;
    s.le=origin(s.layer.left_eye); s.re=origin(s.layer.right_eye);
    s.lp=origin(s.layer.left_pupil); s.rp=origin(s.layer.right_pupil);
    s.mouth=origin(s.layer.mouth); s.hair=origin(s.layer.hair_front);
    s.state=JULIA_SUB_STATE_S1_1_NEAR_STANDBY; s.initialized=true; s.suspended=true;
    s.pose_zoom=256; s.pose_breath_amplitude=4;
    s.left_eye_frame = s.right_eye_frame = UINT8_MAX; /* 哨兵：首帧强制换图。 */
    schedule_for_state(0);
#if DISPLAY_STABILITY_HOTFIX
    reset_stability_transforms();
#endif
    log_memory("enter", __func__);
    ESP_LOGI(MOTION_TAG, "initialized paused; waiting for boot reveal");
    log_memory("exit", __func__);
}

/* 更新子状态并重排随机计划。由 julia_ui 的 state_worker_task 在 LVGL 锁内调用。
 * 输入：JULIA_SUB_STATE_*；越界（>=COUNT）会被忽略以保持状态机一致。 */
void avatar_micro_motion_set_state(julia_sub_state_t state)
{
    log_memory("enter", __func__);
    if (state < JULIA_SUB_STATE_COUNT) s.state=state;
    schedule_for_state(s.last_update);
    ESP_LOGI(EVENT_TAG,"t=%lu state=%d transition_ms=300",(unsigned long)s.last_update,state);
    log_memory("exit", __func__);
}

/* 更新对话相位并重排随机计划。phase 越界(>3)会被忽略。
 * 相位→微动映射见 update_avatar()：0=IDLE(呼吸/微晃)、1=LISTEN(点头)、2=THINK(仰视凝视)、3=SPEAK(锁定)。 */
void avatar_micro_motion_set_dialog_phase(uint8_t phase)
{
    log_memory("enter", __func__);
    if (phase <= 3) s.phase=phase;
    schedule_for_state(s.last_update);
    ESP_LOGI(EVENT_TAG,"t=%lu phase=%u transition_ms=300",(unsigned long)s.last_update,phase);
    log_memory("exit", __func__);
}

/* 挂起/恢复微动。挂起时不执行任何图层写入；这是转场/息屏期间冻结画面的软开关。 */
void avatar_micro_motion_suspend(bool suspended) { s.suspended=suspended; }

/* 全局暂停：一键冻结一切微动，并停掉呼吸/头/颈重复动画。用于转场进入冻结帧或显示稳定性模式。 */
void avatar_motion_pause_all(void)
{
    s_all_paused = true;
    s.suspended = true;
    if (s.layer.container) lv_anim_del(s.layer.container, set_transform_zoom);
    if (s.layer.neck) lv_anim_del(s.layer.neck, set_neck_angle);
    if (s.layer.head) lv_anim_del(s.layer.head, set_head_angle);
    s.breathe_running = s.head_running = s.neck_running = false;
    ESP_LOGI(MOTION_TAG, "all animations paused");
}

/* 全局恢复。注意 HOTFIX 开启时该函数是有意“恢复但继续保持暂停”：
 * 见 DISPLAY_STABILITY_HOTFIX=1。开启态下我们只复位变换并保持全部动画冻结，
 * 以规避无 TE 的 QSPI 面板整屏动画撕裂（这是显示稳定性热修复）。 */
void avatar_motion_resume_all(void)
{
    if (!s_all_paused) return;
#if DISPLAY_STABILITY_HOTFIX
    reset_stability_transforms();
    s_all_paused = true;
    s.suspended = true;
    s.breathe_running = s.head_running = s.neck_running = false;
    ESP_LOGI(MOTION_TAG, "display stability mode: continuous avatar motion remains paused");
    return;
#else
    s_all_paused = false;
    s.suspended = false;
#if !DISPLAY_STABILITY_HOTFIX
    if (s.layer.container) {
        start_repeating_anim(s.layer.container, set_transform_zoom, 256, 260,
                             1500, random_between(1500, 2500), 0);
        s.breathe_running = true;
    }
    if (s.layer.neck) {
        start_repeating_anim(s.layer.neck, set_neck_angle, -10, 10, 2500, 2500, 500);
        s.neck_running = true;
    }
    if (s.layer.head) {
        start_repeating_anim(s.layer.head, set_head_angle, -20, 20, 2500, 2500, 0);
        s.head_running = true;
    }
#endif
    schedule_for_state(s.last_update);
    ESP_LOGI(MOTION_TAG, "all animations resumed");
#endif
}

bool avatar_motion_all_paused(void) { return s_all_paused; }

/* ---- 主状态转场：瞬间设定姿势并做一次可选的“混合”动画 ----
 * transition_* 是 lv_anim 的执行回调（head/neck 角度、zoom、透明度）。
 * 其中 head/neck/zoom 在 HOTFIX 开启时被跳过（见下），仅保留眼睛/瞳孔透明度混合。 */
static __attribute__((unused)) void transition_head(void *object, int32_t value)
{ if (object) lv_obj_set_style_transform_angle(object, (lv_coord_t)value, 0); }
static __attribute__((unused)) void transition_neck(void *object, int32_t value)
{ if (object) lv_obj_set_style_transform_angle(object, (lv_coord_t)value, 0); }
static __attribute__((unused)) void transition_zoom(void *object, int32_t value)
{ if (object) lv_obj_set_style_transform_zoom(object, (lv_coord_t)value, 0); }
static void transition_opa(void *object, int32_t value)
{ if (object) lv_obj_set_style_opa(object, (lv_opa_t)value, 0); }

/* 启动一个带 ready 回调的单次转场动画：先删掉对象上同类回调的旧动画，避免叠加冲突。
 * duration_ms<=0 时视为 1ms（立即完成）。 */
static void transition_anim(lv_obj_t *object, lv_anim_exec_xcb_t callback,
                            int32_t from, int32_t to, uint16_t duration_ms,
                            lv_anim_ready_cb_t ready)
{
    if (!object) return;
    lv_anim_del(object, callback);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, callback);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_time(&animation, duration_ms);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    if (ready) lv_anim_set_ready_cb(&animation, ready);
    lv_anim_start(&animation);
}

/* 转场完成回调：若目标落在 S1~S4（非 S0/S5 休眠静默），则恢复全局动画。
 * 这把“转场冻结”与“转场后复活”绑定在最后一帧，避免中途提前放行。 */
static void main_transition_ready(lv_anim_t *animation)
{
    (void)animation;
    if (s.transition_target >= JULIA_MAIN_STATE_S1_STANDBY &&
        s.transition_target <= JULIA_MAIN_STATE_S4_DIALOG) avatar_motion_resume_all();
    ESP_LOGI(EVENT_TAG, "main transition complete target=S%u", s.transition_target);
}

/* 主状态转场：把整幅姿态一次性切到目标状态对应的姿势。
 * 输入：目标主状态（S0~S5）+ 时长。前置：已 init、state 越界则忽略；duration=0 时强制 1ms 立即完成。
 * 副作用：先全局暂停，改 pose_head/neck_angle、pose_zoom、pose_breath_amplitude；
 * 按状态决定眼睛/瞳孔透明度（大部分状态 255 不透明，S0/S5 为 0 隐藏）；
 * 再对既定图层做一次缓动混合；最后切嘴型缺省帧（S3 主动用 half，其余用 closed，S4 对话不在此切）。
 * 注意：HOTFIX 开启时 head/neck/zoom 三个角度/缩放的转场被有意跳过（整屏动画会撕裂），
 * 只保留眼睛透明度过场，姿势仍立即设置 —— 这是显示稳定性热修复。 */
void avatar_motion_transition_main(julia_main_state_t state, uint16_t duration_ms)
{
    /* 各状态的姿势表：head 角度、neck 角度、zoom、eye 透明度、呼吸幅度。
     * 索引对应 S0~S5；例如 S0 睡眠 head=-150 前倾低头、eye=0 全隐；S3 主动 head=80 抬头。 */
    static const int16_t head[] = {-150, 0, 30, 80, 0, -50};
    static const int16_t neck[] = {-50, 0, 10, 30, 0, -20};
    static const uint16_t zoom[] = {256, 256, 256, 258, 256, 256};
    static const uint8_t eyes[] = {0, 255, 255, 255, 255, 0};
    static const uint8_t amplitude[] = {0, 4, 2, 2, 2, 0};
    if (!s.initialized || state >= JULIA_MAIN_STATE_COUNT) return;
    if (!duration_ms) duration_ms = 1;
    int32_t from_head = s.layer.head ? lv_obj_get_style_transform_angle(s.layer.head, 0) : 0;
    int32_t from_neck = s.layer.neck ? lv_obj_get_style_transform_angle(s.layer.neck, 0) : 0;
    int32_t from_zoom = s.layer.container ? lv_obj_get_style_transform_zoom(s.layer.container, 0) : 256;
    int32_t from_opa = s.layer.left_eye ? lv_obj_get_style_opa(s.layer.left_eye, 0) : 255;
    avatar_motion_pause_all();
    s.transition_target = state;
    s.pose_head_angle = head[state]; s.pose_neck_angle = neck[state];
    s.pose_zoom = zoom[state]; s.pose_breath_amplitude = amplitude[state];
#if !DISPLAY_STABILITY_HOTFIX
    transition_anim(s.layer.container, transition_zoom, from_zoom, zoom[state], duration_ms, NULL);
    transition_anim(s.layer.neck, transition_neck, from_neck, neck[state], duration_ms, NULL);
#else
    (void)from_neck;
    (void)from_zoom;
#endif
    transition_anim(s.layer.left_eye, transition_opa, from_opa, eyes[state], duration_ms,
                    main_transition_ready);
    transition_anim(s.layer.right_eye, transition_opa, from_opa, eyes[state], duration_ms, NULL);
    transition_anim(s.layer.left_pupil, transition_opa, from_opa, eyes[state], duration_ms, NULL);
    transition_anim(s.layer.right_pupil, transition_opa, from_opa, eyes[state], duration_ms, NULL);
    /* 嘴型缺省帧：按状态切到 half(S3) 或 closed(其余)；S4 对话态在此不动嘴（由 RMS 驱动）。 */
    const lv_img_dsc_t *mouth_default = avatar_layer_mouth(AVATAR_MOUTH_CLOSED);
    const lv_img_dsc_t *mouth_source = valid_layer_or_fallback("mouth",
        avatar_layer_mouth(
            state == JULIA_MAIN_STATE_S3_INITIATIVE ? AVATAR_MOUTH_HALF : AVATAR_MOUTH_CLOSED),
        mouth_default);
    if (s.layer.mouth && mouth_source && state != JULIA_MAIN_STATE_S4_DIALOG)
        lv_img_set_src(s.layer.mouth, mouth_source);
#if !DISPLAY_STABILITY_HOTFIX
    transition_anim(s.layer.head, transition_head, from_head, head[state], duration_ms, NULL);
#else
    (void)from_head;
#endif
    ESP_LOGI(EVENT_TAG, "main transition target=S%u duration=%u head=%d neck=%d eyes=%u zoom=%u",
             state, duration_ms, head[state], neck[state], eyes[state], zoom[state]);
}
/* 用户交互：重排随机计划（让动作“重启”而非延续旧节奏），并回读 idle 计时。
 * 这是唤醒/触摸调用入口，不持 LVGL 锁，只重置调度与通知 theme，且不回写图层。 */
void on_user_interaction(void)
{
    schedule_for_state(s.last_update);
    reset_idle_timer();
}

/* 运动配置（当前为单一静态实例）。NOTE：字段只填充了 blink/gaze/breath 相关项，
 * 其余（wink/yawn/shoulder 等触率）未赋值、也未被本文件引用 —— 它们是 fused 三层微动作的
 * 配置骨架，本移植没有实现对应动作，保留是为了接口稳定。是否启用需结合调用方确认。 */
static const avatar_motion_config_t motion_config={
    .blink_rate_x100=1500,.gaze_hold_min_ms=800,.gaze_hold_max_ms=2000,
    .breath_period_ms=4000,.breath_amplitude_px=1,.enabled=1
};
/* 返回运动配置。当前实现忽略 state（所有子状态返回同一份），参数仅用于接口预留。 */
const avatar_motion_config_t *avatar_micro_motion_config(julia_sub_state_t state)
{ (void)state; return &motion_config; }

/* 瞳孔随动状态机：在中心→出视→保持→回中之间驱动瞳孔偏移 (gx,gy)。
 * 触发：GAZE_CENTER 时到 next_gaze 才随机取一个偏移（x∈[-5,5]、y∈[-4,4]，且至少一轴 |.|≥3，
 * 避免“几乎不动”的无效凝视）；出视用 ease_out、回中用 ease_in_out，各阶段时长都硬编码。
 * 返回值：通过 gx、gy 写出当前偏移（Q10 已放大），供 move() 叠加。 */
static void update_gaze(uint32_t now, int32_t *gx, int32_t *gy)
{
    if (s.gaze_stage==GAZE_CENTER && now>=s.next_gaze) {
        do { s.gaze_x=(int8_t)((int)(esp_random()%11U)-5); s.gaze_y=(int8_t)((int)(esp_random()%9U)-4); }
        while (abs(s.gaze_x)<3 && abs(s.gaze_y)<3);
        s.gaze_stage=GAZE_OUT; s.gaze_started=now; s.gaze_duration=180;
        ESP_LOGI(EVENT_TAG,"t=%lu gaze start x=%d y=%d",(unsigned long)now,s.gaze_x,s.gaze_y);
    }
    uint32_t elapsed=now-s.gaze_started;
    int32_t t=clamp_q10(elapsed,s.gaze_duration);
    if (s.gaze_stage==GAZE_OUT) {
        int32_t eased=ease_out_q10(t); *gx=s.gaze_x*eased; *gy=s.gaze_y*eased;
        if (t==Q10_ONE) { s.gaze_stage=GAZE_HOLD; s.gaze_started=now; s.gaze_duration=random_between(800,2000); }
    } else if (s.gaze_stage==GAZE_HOLD) {
        *gx=s.gaze_x*Q10_ONE; *gy=s.gaze_y*Q10_ONE;
        if (elapsed>=s.gaze_duration) { s.gaze_stage=GAZE_BACK; s.gaze_started=now; s.gaze_duration=420; }
    } else if (s.gaze_stage==GAZE_BACK) {
        int32_t inverse=Q10_ONE-ease_in_out_q10(t); *gx=s.gaze_x*inverse; *gy=s.gaze_y*inverse;
        if (t==Q10_ONE) { s.gaze_stage=GAZE_CENTER; s.next_gaze=now+random_between(2500,6000); ESP_LOGI(EVENT_TAG,"t=%lu gaze center",(unsigned long)now); }
    }
}

/* 按左右眼与帧号切换眼睛的资源图（bin→lv_img_dsc_t）。
 * 帧号：0=open、1=half、2=closed（与 avatar_layer_eye 的映射一致）。
 * 副作用：仅在帧号变化时 set_src（用 *current 哨兵去重，初始 UINT8_MAX 保证首帧必换）。
 * 失败路径：资源无效则回退到 open 帧；对象或资源为 NULL 时静默跳过。 */
static void set_eye_frame(bool left, uint8_t frame, lv_obj_t *object)
{
    uint8_t *current = left ? &s.left_eye_frame : &s.right_eye_frame;
    if (*current == frame) return;
    const lv_img_dsc_t *source=valid_layer_or_fallback(left ? "eye_left" : "eye_right",
        avatar_layer_eye(left,frame), avatar_layer_eye(left,0));
    if (object && source) { lv_img_set_src(object,source); *current=frame; }
}

/* 单帧微动驱动器：主循环按 ~40ms 调用。
 * 前置：模块已 init；现在_ms 由调用方以 esp_timer_get_time()/1000 传入，单调递增。
 * 进入守卫：未 init、全局暂停、挂起或距上次不足 UPDATE_MS 都直接返回（合并过密调用）。
 * 副作用：
 *   - 首个非空返回点喂狗（esp_task_wdt_reset()），因此本函数必须由被 WDT 监听的常规任务调用；
 *   - 周期性读取 lvgl_port_get_flush_metrics() 计算 flush_fps（诊断）；
 *   - 依据状态/相位设置眼睛帧、瞳孔显隐，计算 gaze 与点头位移 (gx,gy,feature_y)；
 *   - 之后对瞳孔/眼睛/嘴部做一次 `lvgl_port_lock(pdMS_TO_TICKS(4))` 保护的 set_pos 刷新，
 *     取不到锁则丢弃本帧（绝不阻塞）。
 *
 * NOTE：update_avatar 在锁外读取 s.state/s.phase/s.nodding/s.gaze_*，而这些字段由持 LVGL 锁的
 * julia_ui state_worker_task（set_state/set_dialog_phase）写入。二者并不同步：写端持锁、读端不持锁，
 * 存在理论上的并发读。当前读取对象均为小整数/布尔，最坏产生一帧的“新旧值混合”，不会崩溃；
 * 是否归属同一任务、是否有必要加读侧锁，需结合调用方（julia_ui）的线程模型确认。
 *
 * 状态/相位→动作映射（同一时刻只做一种微动，其余收敛）：
 *   - 睡眠(S0)：眼睛强制 closed、瞳孔隐藏；
 *   - 相位 IDLE(0)：待机帧眼睛=open、瞳孔放开，并运行 gaze 扫描；
 *   - 相位 LISTEN(1)：点头（feature_y 正弦式先下后上）+ 默认眼睛/open；
 *   - 相位 THINK(2)：仰视凝视（gaze_x=±4、gaze_y=-4）；
 *   - 相位 SPEAK(3)：低幅微动被 high_priority() 关闭，只保留已设置的姿势。
 */
void update_avatar(uint32_t now)
{
    esp_task_wdt_reset();
    if (!s.initialized || s_all_paused || s.suspended || now-s.last_update<UPDATE_MS) return;
    s.last_update=now;
    if (now-s.last_log>=MOTION_LOG_MS) {
        uint32_t elapsed=now-s.last_log;
        uint64_t flush_count=0;
        lvgl_port_get_flush_metrics(&flush_count,NULL,NULL);
        if(s.last_flush_count && elapsed) s.flush_fps=(uint32_t)((flush_count-s.last_flush_count)*1000ULL/elapsed);
        s.last_flush_count=flush_count; s.last_log=now;
        log_memory("tick",__func__);
    }
    /* ---- 计算本帧的位移量（不持锁，纯数值计算） ---- */
    int32_t gx=0,gy=0,feature_y=0;
    if (sleeping()) {
        set_eye_frame(true,2,s.layer.left_eye); set_eye_frame(false,2,s.layer.right_eye);
        if(s.layer.left_pupil)lv_obj_add_flag(s.layer.left_pupil,LV_OBJ_FLAG_HIDDEN);
        if(s.layer.right_pupil)lv_obj_add_flag(s.layer.right_pupil,LV_OBJ_FLAG_HIDDEN);
    } else if (s.phase==0) {
        if (!high_priority()) {
            set_eye_frame(true,0,s.layer.left_eye); set_eye_frame(false,0,s.layer.right_eye);
        }
        if(s.layer.left_pupil)lv_obj_clear_flag(s.layer.left_pupil,LV_OBJ_FLAG_HIDDEN);
        if(s.layer.right_pupil)lv_obj_clear_flag(s.layer.right_pupil,LV_OBJ_FLAG_HIDDEN);
        if(!high_priority()) update_gaze(now,&gx,&gy);
    } else if (s.phase==1) {
        if(!s.nodding && now>=s.next_nod) { s.nodding=true; s.nod_started=now; ESP_LOGI(EVENT_TAG,"t=%lu nod start",(unsigned long)now); }
        if(s.nodding) {
            uint32_t elapsed=now-s.nod_started;
            int32_t t=clamp_q10(elapsed,700);
            feature_y=(t<225 ? (4*Q10_ONE*ease_out_q10(t*Q10_ONE/225))/Q10_ONE :
                       (4*Q10_ONE*(Q10_ONE-ease_in_out_q10((t-225)*Q10_ONE/799)))/Q10_ONE);
            if(t==Q10_ONE){s.nodding=false;s.next_nod=now+random_between(2000,3500);ESP_LOGI(EVENT_TAG,"t=%lu nod center",(unsigned long)now);}
        }
    } else if(s.phase==2) {
        if(now>=s.next_think_gaze){s.gaze_x=(esp_random()&1U)?4:-4;s.gaze_y=-4;s.next_think_gaze=now+random_between(4000,7000);ESP_LOGI(EVENT_TAG,"t=%lu think gaze x=%d y=-4",(unsigned long)now,s.gaze_x);}
        gx=s.gaze_x*Q10_ONE;gy=s.gaze_y*Q10_ONE;
    }
    /* ---- 持锁写回图层；取不到锁就放弃本帧坐标刷新 ---- */
    if(!lvgl_port_lock(pdMS_TO_TICKS(4))) return;
    move(s.layer.left_pupil,s.lp,gx,gy+feature_y); move(s.layer.right_pupil,s.rp,gx,gy+feature_y);
    move(s.layer.left_eye,s.le,0,feature_y); move(s.layer.right_eye,s.re,0,feature_y);
    move(s.layer.mouth,s.mouth,0,feature_y);
    lvgl_port_unlock();
}

#ifdef AVATAR_DEBUG
/* 调试触发器：把某个随机事件的“下次触发”提前到当前时刻，便于人工复现该动作。 */
void avatar_motion_debug_trigger(uint8_t action)
{
    if(action==0)s.next_gaze=s.last_update;
    else if(action==1)s.next_nod=s.last_update;
    else if(action==2)s.next_rare=s.last_update;
}
/* 打印引擎当前关键状态：子状态/相位/当前动作名/各动画运行标志/暂停计数。 */
void avatar_motion_debug_print(void)
{
    ESP_LOGI(MOTION_TAG,"state=%d phase=%u action=%s breathe=%d head=%d neck=%d paused=%u",
             s.state,s.phase,active_action(),s.breathe_running,s.head_running,s.neck_running,s.external_pause_count);
}
/* 打印 PSRAM 最低空闲记录与当前空闲/最大块，用于捕获长时间运行的内存泄漏趋势。 */
void avatar_motion_debug_print_psram_peak(void)
{
    ESP_LOGI(MOTION_TAG,"psram_min=%u free=%u largest=%u",(unsigned)s.minimum_free_psram,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
#endif
