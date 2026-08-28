/**
 * @file    avatar_micro_action.h
 * @brief   三层微动作系统对“业务代码”暴露的稳定接口（仅两个面向主循环的函数）。
 *
 * 设计意图：业务层只 include 本文件即可驱动微动作，不必关心 avatar_micro_motion 的图层绑定、
 * 状态通知与转场接口。真正的实现与“Layer 1/2/3”分层描述见 avatar_micro_action.c 与
 * avatar_micro_motion.c。
 *
 * 调用约定：
 *   - update_avatar() 由主循环按固定周期调用（实现端为 UPDATE_MS=40ms；本文件注释历史写 50ms，
 *     存在差异，见仓库里 avatar_micro_action.c/avatar_micro_motion.h 的说明）；
 *   - 检测到语音/触摸/按键等用户交互时调用 on_user_interaction()。
 *
 * 线程模型：两者都运行在调用方（主循环/应用任务）里；update_avatar 写图层前自行取
 * lvgl_port 锁，on_user_interaction 不触碰 LVGL、只重置调度。详见 avatar_micro_motion.c 文件头。
 */
#pragma once

#include <stdint.h>

/*
 * 三层微动作系统的稳定对外接口。
 * update_avatar() 必须由主循环每 50ms 调用一次；语音、触摸或按键检测到
 * 用户交互时调用 on_user_interaction()，立即清空 boredom 并恢复清醒姿态。
 *
 * NOTE：注释中“每 50ms”与实现端 update_avatar 内的 UPDATE_MS=40 不一致；以实现端 40ms 为准。
 * 另外文件头声称的“boredom / 三层动作”是 fused 完整版描述，当前 L0/L1 移植只实现了
 * 呼吸/头颈微晃/瞳孔随动/点头/状态换帧，未实现 boredom 分级与泊松互斥动作（见 .c）。
 */
void update_avatar(uint32_t now_ms);
void on_user_interaction(void);

