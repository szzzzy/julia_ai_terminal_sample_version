/**
 * @file    avatar_micro_motion.h
 * @brief   未参与当前构建的旧分层微动作参考接口。
 *
 * 当前立绘由 julia_avatar 驱动，未建立本接口要求的旧 UI 图层绑定与 update 节拍。
 *
 * 职责边界（本模块只负责“何时动、往哪动”，不负责画立绘）：
 *   - 立绘静态底图、眼睛/嘴部资源切换与部件绘制在 avatar_parts/{avatar_face,avatar_eyes,
 *     avatar_mouth}；总控（状态机投递、RMS→嘴型、转场决策）在 julia_ui.c；
 *   - 本模块接收“当前子状态 + 对话相位”，在已绑定的 LVGL 图层上施加呼吸/头颈微晃/瞳孔随动/
 *     点头/眼睛帧切换，以及主状态转场时的姿势（头/颈角度、眼睛不透明度、嘴型缺省帧）。
 *
 * 上游：调用方（julia_ui.c 的 state_worker_task 调 set_state/transition_main，
 *        julia_ui_set_dialog_phase 调 set_dialog_phase，主循环调 update_avatar）。
 * 下游：直接写入绑定图层对象（本文件的 avatar_layer_bindings_t），不感知 LCD/面板细节。
 *
 * 线程模型：
 *   - update_avatar() 由主循环（或独立任务）按约 40ms 周期调用；写对象前用 lvgl_port_lock 取锁，
 *     取不到则跳过本帧。它读 s.state/s.phase/s.nodding 等共享状态时不持锁（见实现 NOTE）。
 *   - 呼吸/头颈的重复动画经 LVGL 的 lv_anim 在 LVGL 任务内执行，姿势字段（pose_*）的写入发生在
 *     lvgl_port 锁内（state 切换同样持锁），因此与动画回调串行。
 *
 * 时间驱动：所有“何时动”均由调用方传入的 now_ms（esp_timer_get_time()/1000）驱动；本模块不自建
 * 定时器，也不分配托管内存，单帧计算为 O(1)。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "julia_fsm.h"

/* 可独立变换的现有 LVGL 图层。允许为 NULL，模块会安全跳过缺失图层。
 * 调用方（julia_ui_init）从 avatar_parts 取回这些对象再填充；
 * 每一层都可在任意时刻缺省（如 rig 尚未验证时 hair/eyelid 为 NULL）。 */
typedef struct {
    lv_obj_t *container;     /* 根容器：承载呼吸缩放（zoom）与主状态转场的整体缩放。 */
    lv_obj_t *neck;          /* 颈部容器：头颈微晃的角度（绕 pivot 旋转）。 */
    lv_obj_t *body;          /* 身体容器：当前实现未使用（保留字段）。 */
    lv_obj_t *head;          /* 头部容器：头颈微晃的角度 + 主状态转场角度。 */
    lv_obj_t *hair_front;    /* 前发：当前实现未使用（保留字段）。 */
    lv_obj_t *hair_back;     /* 后发：当前实现未使用（保留字段）。 */
    lv_obj_t *left_eye;      /* 左眼：状态驱动的眼睛帧（open/half/closed）。 */
    lv_obj_t *right_eye;     /* 右眼：同左眼。 */
    lv_obj_t *left_eyelid;   /* 左眼睑：当前实现未使用（保留字段）。 */
    lv_obj_t *right_eyelid;  /* 右眼睑：当前实现未使用（保留字段）。 */
    lv_obj_t *left_pupil;    /* 左瞳孔：随动/微动目标，参与 gaze 扫描与点头位移。 */
    lv_obj_t *right_pupil;   /* 右瞳孔：同左瞳孔。 */
    lv_obj_t *mouth;         /* 嘴部：点头位移 + 主状态转场缺省帧切换。 */
} avatar_layer_bindings_t;

/* 泊松触发率使用“每分钟次数 x100”，避免配置表使用浮点数。
 * NOTE：这是 fused 三层微动作（Layer 2 泊松互斥动作）的配置骨架。当前 L0/L1 移植实现里，
 * 多数触率字段并未被 avatar_micro_motion.c 引用——实际调度用 schedule_for_state/update_gaze
 * 里硬编码的随机窗口；本结构仅保留对外语义，字段是否启用需结合调用方确认。 */
typedef struct {
    uint16_t blink_rate_x100;      /* 眨眼触率（次/分钟 ×100）。 */
    uint16_t wink_rate_x100;       /* 单眼 wink 触率（次/分钟 ×100）。 */
    uint16_t mouth_rate_x100;      /* 嘴部小动作触率（次/分钟 ×100）。 */
    uint16_t yawn_rate_x100;       /* 哈欠触率（次/分钟 ×100）。 */
    uint16_t far_gaze_rate_x100;   /* 远望/出神触率（次/分钟 ×100）。 */
    uint16_t shoulder_rate_x100;   /* 肩膀耸肩触率（次/分钟 ×100）。 */
    uint16_t gaze_hold_min_ms;     /* 目光停留最短时长 ms。 */
    uint16_t gaze_hold_max_ms;     /* 目光停留最长时长 ms。 */
    uint16_t breath_period_ms;     /* 呼吸周期 ms。 */
    uint8_t breath_amplitude_px;   /* 呼吸缩放幅度（像素级换算用）。 */
    uint8_t enabled;               /* 整模块使能开关。 */
} avatar_motion_config_t;

/* 绑定图层并启动模块。layers 可为 NULL（此时全部图层视为缺失，模块空转）。
 * 前置：调用方已完成 lvgl_port 初始化、容器/部件对象已创建；本函数写 s 全局但不持 LVGL 锁，
 * 应在 lvgl_port_lock 已持有（julia_ui_init 内）的上下文中调用。 */
void avatar_micro_motion_init(const avatar_layer_bindings_t *layers);
/* 更新子状态（S0~S5），重排各随机事件的触发计划并复位点头/哈欠等一次性动作；
 * 由 julia_ui 的 state_worker_task 持 LVGL 锁调用。 */
void avatar_micro_motion_set_state(julia_sub_state_t state);
/* 更新对话相位（0=IDLE,1=LISTENING,2=THINKING,3=SPEAKING），驱动 listen/think/speak 的微动差异。
 * 由 julia_ui_set_dialog_phase 持 LVGL 锁调用。 */
void avatar_micro_motion_set_dialog_phase(uint8_t phase);
/* 挂起/恢复微动（不清除绑定、不销毁对象）。suspended 时不执行任何图层写入。 */
void avatar_micro_motion_suspend(bool suspended);
/* 返回指定子状态的运动配置；当前实现忽略 state，返回同一份静态配置。 */
const avatar_motion_config_t *avatar_micro_motion_config(julia_sub_state_t state);

/* 主循环按 ~40ms 调用的单帧驱动器：读取内部状态后在其上加 gaze/点头位移并写回图层。
 * 前置：模块已 init、已持有（或能取到）lvgl_port 锁。无动态内存、无文件 I/O，
 * 单帧计算为 O(1)，并且是本模块唯一的 WDT 喂狗点（esp_task_wdt_reset）。 */
void update_avatar(uint32_t now_ms);

/* 语音唤醒、触摸等用户事件调用：立即重排随机计划并复位 idle 计时（用于熄灭/亮度回落）。
 * 不持 LVGL 锁，只写调度字段与通知 julia_display_theme。 */
void on_user_interaction(void);

/* 以下 3 个 init 用于给 container/neck/head 挂上无限重复的呼吸/微晃动画回调。
 * 仅设置对象样式与启动动画，不持有 LVGL 锁（应在外层锁内调用）。 */
void avatar_breathe_init(lv_obj_t *container);
void avatar_head_init(lv_obj_t *container);
void avatar_neck_init(lv_obj_t *container);
/* 暂停/恢复微动：以计数方式支持多个调用方叠加暂停（如说话期间 pause、结束后 resume）。 */
void avatar_motion_pause(void);
void avatar_motion_resume(void);
/* 全局暂停/恢复所有动画（含呼吸/头颈重复动画），用于转场或显示稳定性热修复的一键冻结。 */
void avatar_motion_pause_all(void);
void avatar_motion_resume_all(void);
/* 查询是否处于全局动画暂停状态。 */
bool avatar_motion_all_paused(void);
/* 主状态转场：按目标状态设置姿势（头/颈角度、zoom、眼睛不透明度、缺省嘴型帧），
 * 由 julia_ui.c 的 state_transition_apply 在 LVGL 锁内调用。 */
void avatar_motion_transition_main(julia_main_state_t state, uint16_t duration_ms);

#ifdef AVATAR_DEBUG
void avatar_motion_debug_trigger(uint8_t action);
void avatar_motion_debug_print(void);
void avatar_motion_debug_print_psram_peak(void);
#endif
