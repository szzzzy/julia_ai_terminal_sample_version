/**
 * @file julia_fsm_runtime.h
 * @brief 持有应用唯一的 FSM 实例，并串行处理事件。
 */
#pragma once

#include "esp_err.h"
#include "julia_fault.h"
#include "julia_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 状态提交观察者；回调运行在 FSM 任务中，只能执行零等待操作。 */
typedef void (*julia_fsm_state_observer_t)(julia_main_state_t main_state,
                                            julia_s2_sub_state_t s2_sub_state,
                                            fsm_event_t event,
                                            void *ctx);

/**
 * 初始化运行时。关键交互依赖已就绪时直接按允许迁移图从 S0 进入 S1；
 * 未就绪时保留在 S0，等待应用通过严重故障接口进入 S7。
 *
 * 同时创建 16 槽消息队列、FSM 任务和 S3 驻留计时器。重复调用幂等。
 */
esp_err_t julia_fsm_runtime_init(bool boot_dependencies_ready);
/** 注册单个状态提交观察者；可在运行时初始化之前调用。 */
void julia_fsm_runtime_set_state_observer(julia_fsm_state_observer_t observer,
                                          void *ctx);
/** 零等待投递普通行为事件；队列未创建或已满时返回错误。 */
esp_err_t julia_fsm_runtime_post(fsm_event_t event);
/**
 * 向 FSM 任务队首投递严重故障。任务保存 NVS 快照、进入 S7 并呈现三秒；
 * 同类短时故障未超过三次时复位，超过上限则保持 S7 等待售后。
 */
esp_err_t julia_fsm_runtime_raise_fault(julia_fault_reason_t reason, esp_err_t error);
/** 在线程安全的短临界区内返回最近已提交的主状态。 */
julia_main_state_t julia_fsm_runtime_get_state(void);
/** 返回最近已提交的 S2 子状态；主状态不是 S2 时为 NONE。 */
julia_s2_sub_state_t julia_fsm_runtime_get_s2_sub_state(void);

#ifdef __cplusplus
}
#endif
