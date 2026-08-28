/**
 * @file    julia_fsm.h
 * @brief   Julia 设备行为中枢：两层状态机的类型与接口声明。
 *
 * 职责边界：
 *   - 本模块只维护"设备处于什么行为状态"这一事实，并定义事件如何驱动状态
 *     迁移。它不感知传感器、时间、语音等具体数据来源，也不做任何欠账持久化。
 *   - 实际运行实例由调用方（julia_voice.c 的 s_fsm）持有并负责并发保护；
 *     本模块本身是纯逻辑、无锁、无阻塞，只同步地处理单一事件。
 *
 * 依赖关系：
 *   - 上游生成事件的是 julia_context（传感器/时间驱动）与 julia_voice（唤醒词、
 *     ASR/TTS、对话阶段）等任务；下游消费"当前状态"的是 julia_ui（立绘/表情）
 *     与 julia_context（据此决定下次该发什么事件）。
 *
 * 结构说明：
 *   - 主状态 S0~S5 是粗粒度"大状态"，子状态是每个大状态下的细粒度行为，
 *     二者通过 s_sub_state_info 的映射表维持一一对应（见 julia_fsm.c）。
 *   - 事件与状态采用"全局事件优先，其次当前状态处理器"的两级派发。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 主状态（粗粒度大状态）：设备当前所处的大类行为阶段。
 *
 * 由低到高大致对应从"完全休眠"到"主动交互"再到"回归沉寂"的活跃度曲线，
 * 但注意 S1 与 S2 的关系：S1 是等待/无人时的待机，S2 是有人时的陪伴，
 * 二者依据用户离开/出现互相切换。
 */
typedef enum {
    JULIA_MAIN_STATE_S0_SLEEP = 0,
    JULIA_MAIN_STATE_S1_STANDBY,
    JULIA_MAIN_STATE_S2_COMPANION,
    JULIA_MAIN_STATE_S3_INITIATIVE,
    JULIA_MAIN_STATE_S4_DIALOG,
    JULIA_MAIN_STATE_S5_SILENT,
    JULIA_MAIN_STATE_COUNT,
} julia_main_state_t;

/**
 * @brief 子状态（细粒度行为）：落在某个主状态下的具体行为档位。
 *
 * 命名规则为 S<主状态号>.<序号>，与主状态严格一一对应（见 s_sub_state_info）。
 * 进入 / 迁出条件由各 handle_state_* 处理器 + 共同处理器决定；这里的常量
 * 只代表"状态标识"，不承载迁移逻辑。
 */
typedef enum {
    JULIA_SUB_STATE_S0_1_NIGHT_SLEEP = 0,
    JULIA_SUB_STATE_S0_2_DAY_AWAY,
    JULIA_SUB_STATE_S0_3_MANUAL_SLEEP,
    JULIA_SUB_STATE_S1_1_NEAR_STANDBY,
    JULIA_SUB_STATE_S1_2_FAR_STANDBY,
    JULIA_SUB_STATE_S1_3_CHARGING_STANDBY,
    JULIA_SUB_STATE_S2_1_OBSERVE,
    JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY,
    JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION,
    JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER,
    JULIA_SUB_STATE_S3_2_ROUTINE_BREAK,
    JULIA_SUB_STATE_S3_3_USER_CALL,
    JULIA_SUB_STATE_S3_4_RECOVERY_PROBE,
    JULIA_SUB_STATE_S4_1_LIGHT_DIALOG,
    JULIA_SUB_STATE_S4_2_DEEP_TALK,
    JULIA_SUB_STATE_S4_3_MULTI_TURN,
    JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE,
    /* S5.1 用户拒绝：用户叫停后的负反馈冷处理；S5.2 敷衍应对、S5.3 用户离开同属静默档，
     * 具体进入/迁出条件见 julia_fsm.c 的 handle_rejection_common / S5.3 处理器。 */
    JULIA_SUB_STATE_S5_1_USER_REJECT,
    JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY,
    JULIA_SUB_STATE_S5_3_USER_LEFT,
    JULIA_SUB_STATE_COUNT,
} julia_sub_state_t;

/**
 * @brief FSM 事件集合（约 23 种），是驱动状态迁移的唯一外部输入。
 *
 * 事件来源分几路：
 *   - 传感器/时间（julia_context）：USER_LEAVE/USER_RETURN、NIGHT_TIME、
 *     DAY_AWAY、BEDTIME、SILENCE_TIMEOUT、ROUTINE_BREAK；
 *   - 语音（julia_voice）：USER_CALL、START_DIALOG、DEEP_TALK/MULTI_TURN、
 *     USER_LEFT_DIALOG、INTERRUPT、EMOTION_DETECTED；
 *   - 电源/硬件：LOW_BATTERY、CHARGE_START、CHARGE_DONE、MANUAL_SLEEP、WAKEUP；
 *   - 交互反馈：USER_REJECT、USER_PERFUNCTORY、RECOVERY_ATTEMPT、
 *     SHARED_ACTIVITY_START/STOP。
 *
 * 事件优先级的语义见 julia_fsm.c 的 handle_global_event()：被该函数处理的事件
 * 会无视当前状态、强制迁移；其余事件才交给当前子状态处理器（可能被"忽略"）。
 * EVT_NONE 仅作初始化/缺省占位，不触发任何迁移。
 */
typedef enum {
    EVT_NONE = 0,
    EVT_USER_LEAVE,
    EVT_USER_RETURN,
    EVT_USER_CALL,
    EVT_EMOTION_DETECTED,
    EVT_ROUTINE_BREAK,
    EVT_SILENCE_TIMEOUT,
    EVT_USER_REJECT,
    EVT_USER_PERFUNCTORY,
    EVT_USER_LEFT_DIALOG,
    EVT_LOW_BATTERY,
    EVT_CHARGE_START,
    EVT_CHARGE_DONE,
    EVT_NIGHT_TIME,
    EVT_MANUAL_SLEEP,
    EVT_DAY_AWAY,
    EVT_BEDTIME,
    EVT_SHARED_ACTIVITY_START,
    EVT_SHARED_ACTIVITY_STOP,
    EVT_START_DIALOG,
    EVT_DEEP_TALK_DETECTED,
    EVT_MULTI_TURN_DETECTED,
    EVT_INTERRUPT,
    EVT_RECOVERY_ATTEMPT,
    EVT_WAKEUP,
} fsm_event_t;

typedef struct julia_fsm julia_fsm_t;
typedef void (*julia_fsm_state_cb_t)(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t evt);

/*
 * 状态机实例。字段说明：
 *   - main_state：当前主状态；由 sub_state 经由映射表同步得出，从不单独赋值。
 *   - sub_state：当前子状态，是迁移的真正目标/来源。
 *   - on_enter：进入某子状态时的回调（日志/UI/对话阶段联动）。
 *   - on_exit：迁出某子状态时的回调，先于目标状态进入被调用。
 *   - user_ctx：透传给回调的调用方上下文（当前实现未使用）。
 */
struct julia_fsm {
    julia_main_state_t main_state;
    julia_sub_state_t sub_state;
    julia_fsm_state_cb_t on_enter;
    julia_fsm_state_cb_t on_exit;
    void *user_ctx;
};

/*
 * 并发模型：本模块不假设由哪个任务调用，实例本身不含锁。共享实例的调用方
 * （julia_voice.c）须自行用互斥锁串行化 julia_fsm_handle_event() 与
 * julia_fsm 状态读取，否则 on_enter/on_exit 与状态字段的读写会竞态。
 */
void julia_fsm_init(julia_fsm_t *fsm);
bool julia_fsm_handle_event(julia_fsm_t *fsm, fsm_event_t evt, void *data);
const char *julia_fsm_main_state_name(julia_main_state_t state);
const char *julia_fsm_sub_state_name(julia_sub_state_t state);
const char *julia_fsm_event_name(fsm_event_t evt);
