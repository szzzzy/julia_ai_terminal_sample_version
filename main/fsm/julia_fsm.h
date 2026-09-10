/**
 * @file julia_fsm.h
 * @brief Julia 行为状态机的状态、事件与迁移接口。
 *
 * 状态机回答“设备现在应当做什么”：开机、陪伴、对话、待机、静默、睡眠、
 * 故障或升级。采集声音、播放回答、绘制表情和维持网络连接由对应模块执行。
 * 对话状态进一步区分正在听用户说话、等待服务器回答和正在播放回答。S7.1
 * 只承担一次断联提示：S3/S5/S6 提示后恢复原状态，S1/S2/S4 放弃旧会话后落到 S3；
 * 持续离线由正交服务状态表达，不复制成每个主状态的子状态。
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
    JULIA_MAIN_STATE_S7_FAULT,         /**< 异常提示；具体语义由 S7.1／S7.2 表示。 */
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

/** S7 的两个明确阶段：可恢复断联提示，以及需要记录并复位的严重故障。 */
typedef enum {
    JULIA_S7_SUB_STATE_NONE = 0,
    JULIA_S7_SUB_STATE_S7_1_DISCONNECTED, /**< 可恢复断联提示；结束后按 s7_return_state 返回。 */
    JULIA_S7_SUB_STATE_S7_2_FAULT,        /**< 核心能力不可用，保存故障记录并受控复位。 */
    JULIA_S7_SUB_STATE_COUNT,
} julia_s7_sub_state_t;

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
    EVT_MOTION_WAKE,              /**< S6 中确认明显搬动，恢复到 S3 等待唤醒词。 */
    EVT_INTENT_GOODNIGHT,         /**< 用户表达晚安，结束交流并进入睡眠。 */
    EVT_INTENT_DISMISS,           /**< 用户明确结束交流，进入静默状态。 */
    EVT_MQTT_DISCONNECTED,        /**< 控制消息连接断开，当前交流无法完整继续。 */
    EVT_WSS_DISCONNECTED,         /**< 语音数据连接断开，当前交流无法完整继续。 */
    EVT_MQTT_CONNECTED,           /**< MQTT 控制连接已经恢复。 */
    EVT_WSS_CONNECTED,            /**< WSS 语音连接已经恢复。 */
    EVT_SERVICE_CONNECT_TIMEOUT,  /**< 启动后业务连接未在期限内全部就绪。 */
    EVT_DISCONNECT_NOTICE_TIMEOUT, /**< 断联提示结束，按来源策略返回稳定状态。 */
    EVT_OTA_AVAILABLE,            /**< 已接受一项可执行的固件升级任务。 */
    EVT_OTA_SUCCEEDED,            /**< 新固件已校验并设为下次启动版本。 */
    EVT_OTA_TASK_FAILED,          /**< 本次升级已放弃，继续运行当前固件并等待唤醒。 */
    EVT_PREPARE_TERMINAL_REPLY,   /**< 终止语义到达 S2 时，先回 S4 播放本地回应。 */
    EVT_VOICE_SESSION_RESET,     /**< 新语音会话不继承旧 S1/S2/S4，重新等待唤醒。 */
    EVT_REQUIRE_WAKE,            /**< 云端请求结束 S1 陪伴；活动交互期间拒绝。 */
    EVT_COUNT,
} fsm_event_t;

typedef struct julia_fsm julia_fsm_t;
/** enter/exit 在迁移调用栈内同步执行，不得阻塞或递归修改同一 FSM。 */
typedef void (*julia_fsm_state_cb_t)(julia_fsm_t *fsm,
                                     julia_main_state_t main_state,
                                     julia_s2_sub_state_t s2_sub_state,
                                     fsm_event_t event);

struct julia_fsm {
    julia_main_state_t main_state;
    julia_s2_sub_state_t s2_sub_state;
    julia_s7_sub_state_t s7_sub_state;
    /** S7.1 提示结束后的落点；会话绑定状态断联时固定为 S3。 */
    julia_main_state_t s7_return_state;
    julia_fsm_state_cb_t on_enter;
    julia_fsm_state_cb_t on_exit;
    void *user_ctx;
};

/** 将状态初始化为“正在开机”，不启动任何后台工作。 */
void julia_fsm_init(julia_fsm_t *fsm);
/**
 * 同步处理一个事件；只有完成合法迁移才返回 true。对象本身不加锁，调用方必须
 * 串行化访问；运行固件由 julia_fsm_runtime 的唯一 Task 保证该约束。
 */
bool julia_fsm_handle_event(julia_fsm_t *fsm, fsm_event_t event, void *data);
/** 检查设备状态与对话阶段是否互相匹配。 */
bool julia_fsm_state_is_valid(julia_main_state_t main_state,
                              julia_s2_sub_state_t s2_sub_state);
/**
 * 检查完整状态组合是否合法。对话阶段只允许出现在 S2，断联提示只允许出现在 S7；
 * 这样可以阻止“设备显示待机但内部仍标记断联”等互相矛盾的状态。
 */
bool julia_fsm_state_is_valid_full(julia_main_state_t main_state,
                                   julia_s2_sub_state_t s2_sub_state,
                                   julia_s7_sub_state_t s7_sub_state);
/** 判断某次状态变化是否符合产品流程，不实际修改状态。 */
bool julia_fsm_can_transition(julia_main_state_t from_main_state,
                              julia_s2_sub_state_t from_s2_sub_state,
                              julia_main_state_t to_main_state,
                              julia_s2_sub_state_t to_s2_sub_state);
/**
 * 判断包含 S7 子状态在内的完整状态变化是否符合产品流程。S7.2 只能复位到 S0，
 * S7.1 提示结束后可回到 S3/S5/S6 中记录的稳定落点，或升级为 S7.2。
 */
bool julia_fsm_can_transition_full(julia_main_state_t from_main_state,
                                   julia_s2_sub_state_t from_s2_sub_state,
                                   julia_s7_sub_state_t from_s7_sub_state,
                                   julia_main_state_t to_main_state,
                                   julia_s2_sub_state_t to_s2_sub_state,
                                   julia_s7_sub_state_t to_s7_sub_state);
/**
 * 同步执行普通迁移并调用 exit/enter；无效、重复或 S7.1 历史落点不符时返回 false。
 */
bool julia_fsm_transition_to(julia_fsm_t *fsm,
                             julia_main_state_t to_main_state,
                             julia_s2_sub_state_t to_s2_sub_state,
                             fsm_event_t reason);
/**
 * 同步执行含 S7 子状态的迁移。进入 S7.1 时会记录稳定返回点，调用方不得绕过该
 * 入口直接改写状态字段，否则提示结束后的恢复目标不再可信。
 */
bool julia_fsm_transition_to_full(julia_fsm_t *fsm,
                                  julia_main_state_t to_main_state,
                                  julia_s2_sub_state_t to_s2_sub_state,
                                  julia_s7_sub_state_t to_s7_sub_state,
                                  fsm_event_t reason);
const char *julia_fsm_main_state_name(julia_main_state_t state);
const char *julia_fsm_s2_sub_state_name(julia_s2_sub_state_t state);
const char *julia_fsm_s7_sub_state_name(julia_s7_sub_state_t state);
const char *julia_fsm_event_name(fsm_event_t event);
