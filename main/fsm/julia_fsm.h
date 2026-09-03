/**
 * @file julia_fsm.h
 * @brief Julia 行为状态机的状态、事件与迁移接口。
 *
 * 状态机只描述设备行为，不负责采集音频、播放声音、绘制画面或管理网络。
 * 第一层包含 S0～S8 共九个主状态；只有 S2 拥有 S2.1“听”、
 * S2.2“想”、S2.3“说”三个子状态。当前主状态不是 S2 时，
 * s2_sub_state 必须为 JULIA_S2_SUB_STATE_NONE。
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

/** 当前构建中已有生产者、且由行为状态机消费的事件。 */
typedef enum {
    EVT_NONE = 0,                 /**< 初始化或直接迁移时使用，不由外部投递。 */
    EVT_USER_LEAVE,
    EVT_USER_CALL,
    EVT_SILENCE_TIMEOUT,
    EVT_NIGHT_TIME,
    EVT_STANDBY_TIMEOUT,          /**< S3 连续驻留达到配置时长。 */
    EVT_SILENT_TIMEOUT,           /**< S5 连续驻留达到配置时长。 */
    EVT_BEDTIME,                  /**< 现有睡前调度事件；当前不改变行为状态。 */
    EVT_START_DIALOG,
    EVT_MULTI_TURN_DETECTED,
    EVT_INTERRUPT,
    EVT_WAKEUP,                   /**< 语音链路确认唤醒词。 */
    EVT_INTENT_GOODNIGHT,         /**< MQTT：晚安意图，S4/S2 进入 S6。 */
    EVT_INTENT_DISMISS,           /**< MQTT：结束沟通意图，S4/S2 进入 S5。 */
    EVT_MQTT_DISCONNECTED,        /**< MQTT 会话断开，S1/S2/S4 进入 S3。 */
    EVT_WSS_DISCONNECTED,         /**< WSS transport 结束，S1/S2/S4 进入 S3。 */
    EVT_OTA_AVAILABLE,            /**< OTA 引擎已接受升级任务。 */
    EVT_OTA_SUCCEEDED,            /**< OTA 镜像已提交，即将复位。 */
    EVT_OTA_TASK_FAILED,          /**< OTA 任务失败，放弃本次升级并回到 S3。 */
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

/** 初始化为 S0/NONE；不创建任务、队列或锁。 */
void julia_fsm_init(julia_fsm_t *fsm);
/** 使用当前已接入事件选择目标状态；发生迁移时返回 true。 */
bool julia_fsm_handle_event(julia_fsm_t *fsm, fsm_event_t event, void *data);
/** 检查主状态与 S2 子状态是否构成合法组合。 */
bool julia_fsm_state_is_valid(julia_main_state_t main_state,
                              julia_s2_sub_state_t s2_sub_state);
/** 只检查规划迁移图，不修改状态，也不调用回调。 */
bool julia_fsm_can_transition(julia_main_state_t from_main_state,
                              julia_s2_sub_state_t from_s2_sub_state,
                              julia_main_state_t to_main_state,
                              julia_s2_sub_state_t to_s2_sub_state);
/** 按规划图执行显式迁移；非法目标和同状态迁移返回 false。 */
bool julia_fsm_transition_to(julia_fsm_t *fsm,
                             julia_main_state_t to_main_state,
                             julia_s2_sub_state_t to_s2_sub_state,
                             fsm_event_t reason);
const char *julia_fsm_main_state_name(julia_main_state_t state);
const char *julia_fsm_s2_sub_state_name(julia_s2_sub_state_t state);
const char *julia_fsm_event_name(fsm_event_t event);
