/**
 * @file julia_fsm_runtime.h
 * @brief 提供行为 FSM 与正交云端可用性的串行运行时。
 */
#pragma once

#include "esp_err.h"
#include "julia_fault.h"
#include "julia_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 状态改变后的通知函数；只能快速记录或投递消息，不能等待网络或硬件。 */
typedef void (*julia_fsm_state_observer_t)(julia_main_state_t main_state,
                                            julia_s2_sub_state_t s2_sub_state,
                                            fsm_event_t event,
                                            void *ctx);

/** 与行为状态机正交的云端业务可用性；离线标签只由该状态控制。 */
typedef enum {
    JULIA_SERVICE_CONNECTING = 0, /**< 尚未完成初始 MQTT/WSS 汇合，不提前报故障。 */
    JULIA_SERVICE_ONLINE,         /**< MQTT 关键订阅和 WSS 认证会话均已就绪。 */
    JULIA_SERVICE_OFFLINE,        /**< 已确认不可用；全部链路恢复前保持锁存。 */
} julia_service_state_t;

/**
 * 初始化设备行为管理。显示、声音和语音服务都可用时，开机完成后直接进入
 * 等待唤醒状态；关键能力不可用时仍保持开机状态，由应用报告严重故障。
 *
 * 同时创建事件队列、状态任务和各状态计时器。云端从 CONNECTING 开始，超过配置
 * 期限仍未完成 MQTT/WSS 汇合时才进入一次 S7.1。重复调用不会创建第二套实例。
 */
esp_err_t julia_fsm_runtime_init(bool boot_dependencies_ready);
/** 注册一个状态变化通知接收方；可在设备行为管理启动前调用。 */
void julia_fsm_runtime_set_state_observer(julia_fsm_state_observer_t observer,
                                          void *ctx);
/**
 * 从普通 Task/回调非阻塞投递事件；队列满时返回 ESP_ERR_NO_MEM。不得从 ISR 调用，
 * 连接状态即使丢失一次事件也会由运行时快照核对收敛。
 */
esp_err_t julia_fsm_runtime_post(fsm_event_t event);
/**
 * 优先报告严重故障。设备会保存故障记录、显示故障状态并按配置尝试复位；
 * 同类故障短时间重复超过上限后停止自动复位，等待人工处理。
 */
esp_err_t julia_fsm_runtime_raise_fault(julia_fault_reason_t reason, esp_err_t error);
/** 返回最近一次已经生效的设备状态。 */
julia_main_state_t julia_fsm_runtime_get_state(void);
/** 返回当前对话阶段；不在对话中时返回“无对话阶段”。 */
julia_s2_sub_state_t julia_fsm_runtime_get_s2_sub_state(void);
/** 返回当前 S7 阶段；不在 S7 时返回 NONE。 */
julia_s7_sub_state_t julia_fsm_runtime_get_s7_sub_state(void);
/** 线程安全地读取聚合业务状态快照，不等待网络或 LVGL。 */
julia_service_state_t julia_fsm_runtime_get_service_state(void);

#ifdef __cplusplus
}
#endif
