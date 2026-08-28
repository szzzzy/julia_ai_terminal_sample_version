/**
 * @file    julia_context.h
 * @brief   设备行为中枢的"情境感知"服务：启动 500ms 传感器/时间/情境任务。
 *
 * 职责边界：
 *   - 本模块负责把原始传感器（QMI8658 IMU）、时间（RTC + SNTP）、音频活动、
 *     用户交互记忆 等"外部事实"翻译成 FSM 事件（见 julia_fsm 的 fsm_event_t），
 *     并驱动 FSM 作相应的状态迁移。它不负责状态机本身的逻辑（那在 fsm 模块），
 *     也不负责语音会话的具体交互（那在 voice 模块）。
 *   - 不与 UI 层交互；只通过 julia_voice_handle_event() 间接驱动设备行为。
 *
 * 依赖：julia_voice（事件入口/状态/繁忙查询）、julia_memory（长离隔判定）、
 *      julia_routine（日常活动统计与偏差判定）、QMI8658/PCF85063 驱动、NVS（掉电保存）。
 *
 * 使用方式：在应用早期（如 app_main）调用一次 julia_context_init()，随后由内部
 * 创建的后台任务自循环，不再需要其它生命周期管理。
 */
#pragma once
#include "esp_err.h"
esp_err_t julia_context_init(void);
