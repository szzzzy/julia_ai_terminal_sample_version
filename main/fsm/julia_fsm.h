/**
 * @file julia_fsm.h
 * @brief Julia 行为状态机的状态、事件与迁移接口。
 *
 * 状态机回答“设备现在应当做什么”：开机、陪伴、对话、待机、静默、睡眠、
 * 故障或升级。采集声音、播放回答、绘制表情和维持网络连接由对应模块执行。
 * 对话状态进一步区分正在听用户说话、等待服务器回答和正在播放回答；其它状态
 * 不使用对话阶段。
 *
 * 事件入口只消费当前工程已经实际投递的事件，不为尚未实现的业务预造事件。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    JULIA_MAIN_STATE_S0_BOOT = 0,       /**< 开机初始化。 */
    JULIA_MAIN_STATE_S1_COMPANION,     /**< 对话结束后的免唤醒陪伴。 */
    JULIA_MAIN_STATE_S2_DIALOG,        /**< 对话；具体阶段由 S2 子状态表示。 */
    JULIA_MAIN_STATE_S3_STANDBY,       /**< 默认待机，等待唤醒词。 */
    JULIA_MAIN_STATE_S4_INTERACTION,   /**< 发起交互。 */
    JULIA_MAIN_STATE_S5_SILENT,        /**< 静默。 */
    JULIA_MAIN_STATE_S6_SLEEP,         /**< 睡眠。 */
    JULIA_MAIN_STATE_S7_FAULT,         /**< 严重故障记录、呈现与复位。 */
    JULIA_MAIN_STATE_S8_OTA,           /**< OTA；当前仅定义状态与允许迁移边。 */
    JULIA_MAIN_STATE_COUNT,
} julia_main_state_t;

/** 仅归 JULIA_MAIN_STATE_S2_DIALOG 所有的子状态集合。 */
typedef enum {
    JULIA_S2_SUB_STATE_NONE = 0,           /**< 当前不在 S2。 */
    JULIA_S2_SUB_STATE_S2_1_LISTENING,     /**< 听。 */
    JULIA_S2_SUB_STATE_S2_2_THINKING,      /**< 想。 */
    JULIA_S2_SUB_STATE_S2_3_SPEAKING,      /**< 说。 */
    JULIA_S2_SUB_STATE_COUNT,
} julia_s2_sub_state_t;

/** 能够改变设备行为的已实现事件。 */
typedef enum {
    EVT_NONE = 0,                 /**< 表示没有外部原因，仅用于初始化。 */
    EVT_USER_LEAVE,
    EVT_USER_CALL,
    EVT_SILENCE_TIMEOUT,
    EVT_NIGHT_TIME,
    EVT_STANDBY_TIMEOUT,          /**< 等待唤醒超过设定时长，准备进入睡眠。 */
    EVT_SILENT_TIMEOUT,           /**< 保持静默超过设定时长，返回普通待机。 */
    EVT_BEDTIME,                  /**< 到达睡前提醒时间；当前只记录，不改变状态。 */
    EVT_START_DIALOG,
    EVT_MULTI_TURN_DETECTED,
    EVT_INTERRUPT,
    EVT_WAKEUP,                   /**< 本地或服务器已经确认用户说出唤醒词。 */
    EVT_INTENT_GOODNIGHT,         /**< 用户表达晚安，结束交流并进入睡眠。 */
    EVT_INTENT_DISMISS,           /**< 用户明确结束交流，进入静默状态。 */
    EVT_MQTT_DISCONNECTED,        /**< 控制消息连接断开，当前交流无法完整继续。 */
    EVT_WSS_DISCONNECTED,         /**< 语音数据连接断开，当前交流无法完整继续。 */
    EVT_OTA_AVAILABLE,            /**< 已接受一项可执行的固件升级任务。 */
    EVT_OTA_SUCCEEDED,            /**< 新固件已校验并设为下次启动版本。 */
    EVT_OTA_TASK_FAILED,          /**< 本次升级已放弃，继续运行当前固件并等待唤醒。 */
    EVT_COUNT,
} fsm_event_t;

typedef struct julia_fsm julia_fsm_t;
typedef void (*julia_fsm_state_cb_t)(julia_fsm_t *fsm,
                                     julia_main_state_t main_state,
                                     julia_s2_sub_state_t s2_sub_state,
                                     fsm_event_t event);

struct julia_fsm {
    julia_main_state_t main_state;
    julia_s2_sub_state_t s2_sub_state;
    julia_fsm_state_cb_t on_enter;
    julia_fsm_state_cb_t on_exit;
    void *user_ctx;
};

/** 将状态初始化为“正在开机”，不启动任何后台工作。 */
void julia_fsm_init(julia_fsm_t *fsm);
/** 按当前设备状态处理一个业务事件；状态确实改变时返回 true。 */
bool julia_fsm_handle_event(julia_fsm_t *fsm, fsm_event_t event, void *data);
/** 检查设备状态与对话阶段是否互相匹配。 */
bool julia_fsm_state_is_valid(julia_main_state_t main_state,
                              julia_s2_sub_state_t s2_sub_state);
/** 判断某次状态变化是否符合产品流程，不实际修改状态。 */
bool julia_fsm_can_transition(julia_main_state_t from_main_state,
                              julia_s2_sub_state_t from_s2_sub_state,
                              julia_main_state_t to_main_state,
                              julia_s2_sub_state_t to_s2_sub_state);
/** 执行一次符合产品流程的状态变化；无效或重复变化返回 false。 */
bool julia_fsm_transition_to(julia_fsm_t *fsm,
                             julia_main_state_t to_main_state,
                             julia_s2_sub_state_t to_s2_sub_state,
                             fsm_event_t reason);
const char *julia_fsm_main_state_name(julia_main_state_t state);
const char *julia_fsm_s2_sub_state_name(julia_s2_sub_state_t state);
const char *julia_fsm_event_name(fsm_event_t event);
