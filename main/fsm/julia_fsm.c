/**
 * @file    julia_fsm.c
 * @brief   Julia 两层状态机的驱动实现（主状态 + 子状态，事件驱动）。
 *
 * 职责边界：
 *   - 负责"由事件得出下一个子状态"并执行迁移（原子地更新 sub_state/main_state、
 *     先 on_exit 后 on_enter）；事件的来源、副作用（如语音/UI 动作）以及并发
 *     保护均不属于本模块，由调用方 julia_voice.c 处理。
 *   - 不依赖具体硬件 / 传感器 / 时间，只做纯逻辑查表 + 分发，因此可独立单元测试。
 *
 * 两级派发逻辑（julia_fsm_handle_event）：
 *   1) 先问 handle_global_event()。返回非哨兵值说明该事件"具有全局优先级"，
 *      无论当前处于哪个子状态都会被强制迁移（见该函数注释）。
 *   2) 否则交给当前子状态对应的 handle_state_* 处理器。处理器返回下一个子状态，
 *      或返回 JULIA_SUB_STATE_COUNT 表示"该事件在此状态被忽略"（不迁移）。
 *   3) 仅当下一个子状态有效且不同于当前状态时才调用 julia_fsm_transition()。
 *
 * 状态生命周期：
 *   - on_exit 在"离开旧状态"时触发，on_enter 在"进入新状态"时触发，二者总是成对
 *     且在迁移过程中先后执行；相同状态之间的"迁移"被短路，不触发任何回调。
 *
 * 共同处理器（子状态复用）语义：
 *   同一主状态下的多个子状态对若干事件有相同的响应，故抽成
 *   handle_sleep_common / handle_initiative_common / handle_rejection_common /
 *   handle_dialog_common；各 handle_state_* 只在自身特有的事件上做区分，
 *   其余事件统一交给共同处理器。这是本文件最核心的"少写分支"设计。
 */
#include "julia_fsm.h"

#include <stdio.h>
#include "esp_log.h"


#define TAG "JULIA_FSM"

/* 每个子状态的静态信息：所属主状态 + 显示名（用于日志/诊断）。 */
typedef struct {
    julia_main_state_t main_state;
    const char *name;
} julia_sub_state_info_t;

/* 状态处理器：输入当前 fsm、事件与可选数据，返回下一子状态；返回 COUNT 表示忽略。 */
typedef julia_sub_state_t (*julia_state_handler_t)(julia_fsm_t *fsm, fsm_event_t evt, void *data);

/*
 * 子状态 -> 主状态 的映射表。
 * 这是"两层状态机"的关键：main_state 从不被单独赋值，而是由当前 sub_state 查得，
 * 从数据层面保证两者永远一致，避免出现"主状态 A 却挂着一个子状态 B"的不合法组合。
 */
static const julia_sub_state_info_t s_sub_state_info[JULIA_SUB_STATE_COUNT] = {
    [JULIA_SUB_STATE_S0_1_NIGHT_SLEEP] = {JULIA_MAIN_STATE_S0_SLEEP, "S0.1"},
    [JULIA_SUB_STATE_S0_2_DAY_AWAY] = {JULIA_MAIN_STATE_S0_SLEEP, "S0.2"},
    [JULIA_SUB_STATE_S0_3_MANUAL_SLEEP] = {JULIA_MAIN_STATE_S0_SLEEP, "S0.3"},
    [JULIA_SUB_STATE_S1_1_NEAR_STANDBY] = {JULIA_MAIN_STATE_S1_STANDBY, "S1.1"},
    [JULIA_SUB_STATE_S1_2_FAR_STANDBY] = {JULIA_MAIN_STATE_S1_STANDBY, "S1.2"},
    [JULIA_SUB_STATE_S1_3_CHARGING_STANDBY] = {JULIA_MAIN_STATE_S1_STANDBY, "S1.3"},
    [JULIA_SUB_STATE_S2_1_OBSERVE] = {JULIA_MAIN_STATE_S2_COMPANION, "S2.1"},
    [JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY] = {JULIA_MAIN_STATE_S2_COMPANION, "S2.2"},
    [JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION] = {JULIA_MAIN_STATE_S2_COMPANION, "S2.3"},
    [JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER] = {JULIA_MAIN_STATE_S3_INITIATIVE, "S3.1"},
    [JULIA_SUB_STATE_S3_2_ROUTINE_BREAK] = {JULIA_MAIN_STATE_S3_INITIATIVE, "S3.2"},
    [JULIA_SUB_STATE_S3_3_USER_CALL] = {JULIA_MAIN_STATE_S3_INITIATIVE, "S3.3"},
    [JULIA_SUB_STATE_S3_4_RECOVERY_PROBE] = {JULIA_MAIN_STATE_S3_INITIATIVE, "S3.4"},
    [JULIA_SUB_STATE_S4_1_LIGHT_DIALOG] = {JULIA_MAIN_STATE_S4_DIALOG, "S4.1"},
    [JULIA_SUB_STATE_S4_2_DEEP_TALK] = {JULIA_MAIN_STATE_S4_DIALOG, "S4.2"},
    [JULIA_SUB_STATE_S4_3_MULTI_TURN] = {JULIA_MAIN_STATE_S4_DIALOG, "S4.3"},
    [JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE] = {JULIA_MAIN_STATE_S4_DIALOG, "S4.4"},
    [JULIA_SUB_STATE_S5_1_USER_REJECT] = {JULIA_MAIN_STATE_S5_SILENT, "S5.1"},
    [JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY] = {JULIA_MAIN_STATE_S5_SILENT, "S5.2"},
    [JULIA_SUB_STATE_S5_3_USER_LEFT] = {JULIA_MAIN_STATE_S5_SILENT, "S5.3"},
};

/* 主状态名表：仅用于日志/诊断，不在迁移逻辑中参与任何判断。 */
static const char *s_main_state_names[JULIA_MAIN_STATE_COUNT] = {
    [JULIA_MAIN_STATE_S0_SLEEP] = "S0",
    [JULIA_MAIN_STATE_S1_STANDBY] = "S1",
    [JULIA_MAIN_STATE_S2_COMPANION] = "S2",
    [JULIA_MAIN_STATE_S3_INITIATIVE] = "S3",
    [JULIA_MAIN_STATE_S4_DIALOG] = "S4",
    [JULIA_MAIN_STATE_S5_SILENT] = "S5",
};

/* 事件名表：为日志把整型事件翻译成可读字符串；数组下标即事件值。 */
static const char *s_event_names[] = {
    [EVT_NONE] = "EVT_NONE",
    [EVT_USER_LEAVE] = "EVT_USER_LEAVE",
    [EVT_USER_RETURN] = "EVT_USER_RETURN",
    [EVT_USER_CALL] = "EVT_USER_CALL",
    [EVT_EMOTION_DETECTED] = "EVT_EMOTION_DETECTED",
    [EVT_ROUTINE_BREAK] = "EVT_ROUTINE_BREAK",
    [EVT_SILENCE_TIMEOUT] = "EVT_SILENCE_TIMEOUT",
    [EVT_USER_REJECT] = "EVT_USER_REJECT",
    [EVT_USER_PERFUNCTORY] = "EVT_USER_PERFUNCTORY",
    [EVT_USER_LEFT_DIALOG] = "EVT_USER_LEFT_DIALOG",
    [EVT_LOW_BATTERY] = "EVT_LOW_BATTERY",
    [EVT_CHARGE_START] = "EVT_CHARGE_START",
    [EVT_CHARGE_DONE] = "EVT_CHARGE_DONE",
    [EVT_NIGHT_TIME] = "EVT_NIGHT_TIME",
    [EVT_MANUAL_SLEEP] = "EVT_MANUAL_SLEEP",
    [EVT_DAY_AWAY] = "EVT_DAY_AWAY",
    [EVT_BEDTIME] = "EVT_BEDTIME",
    [EVT_SHARED_ACTIVITY_START] = "EVT_SHARED_ACTIVITY_START",
    [EVT_SHARED_ACTIVITY_STOP] = "EVT_SHARED_ACTIVITY_STOP",
    [EVT_START_DIALOG] = "EVT_START_DIALOG",
    [EVT_DEEP_TALK_DETECTED] = "EVT_DEEP_TALK_DETECTED",
    [EVT_MULTI_TURN_DETECTED] = "EVT_MULTI_TURN_DETECTED",
    [EVT_INTERRUPT] = "EVT_INTERRUPT",
    [EVT_RECOVERY_ATTEMPT] = "EVT_RECOVERY_ATTEMPT",
    [EVT_WAKEUP] = "EVT_WAKEUP",
};

static julia_sub_state_t handle_state_s0_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s0_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s0_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s1_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s1_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s1_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s2_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s2_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s2_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s3_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s3_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s3_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s3_4(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s4_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s4_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s4_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s4_4(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s5_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s5_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s5_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);

/* 子状态 -> 处理器 的分发表：用[下标]指定初始化，下标即子状态枚举值，
 * 缺省项为 NULL，由 handle_event 里的 NULL 检查兜底。 */
static const julia_state_handler_t s_state_handlers[JULIA_SUB_STATE_COUNT] = {
    [JULIA_SUB_STATE_S0_1_NIGHT_SLEEP] = handle_state_s0_1,
    [JULIA_SUB_STATE_S0_2_DAY_AWAY] = handle_state_s0_2,
    [JULIA_SUB_STATE_S0_3_MANUAL_SLEEP] = handle_state_s0_3,
    [JULIA_SUB_STATE_S1_1_NEAR_STANDBY] = handle_state_s1_1,
    [JULIA_SUB_STATE_S1_2_FAR_STANDBY] = handle_state_s1_2,
    [JULIA_SUB_STATE_S1_3_CHARGING_STANDBY] = handle_state_s1_3,
    [JULIA_SUB_STATE_S2_1_OBSERVE] = handle_state_s2_1,
    [JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY] = handle_state_s2_2,
    [JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION] = handle_state_s2_3,
    [JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER] = handle_state_s3_1,
    [JULIA_SUB_STATE_S3_2_ROUTINE_BREAK] = handle_state_s3_2,
    [JULIA_SUB_STATE_S3_3_USER_CALL] = handle_state_s3_3,
    [JULIA_SUB_STATE_S3_4_RECOVERY_PROBE] = handle_state_s3_4,
    [JULIA_SUB_STATE_S4_1_LIGHT_DIALOG] = handle_state_s4_1,
    [JULIA_SUB_STATE_S4_2_DEEP_TALK] = handle_state_s4_2,
    [JULIA_SUB_STATE_S4_3_MULTI_TURN] = handle_state_s4_3,
    [JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE] = handle_state_s4_4,
    [JULIA_SUB_STATE_S5_1_USER_REJECT] = handle_state_s5_1,
    [JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY] = handle_state_s5_2,
    [JULIA_SUB_STATE_S5_3_USER_LEFT] = handle_state_s5_3,
};

/*
 * 缺省进入/退出回调：仅打印日志，让状态机在没有绑定业务回调时也能被观察。
 * julia_voice_init() 会用 voice_on_enter 覆盖 on_enter（把状态同步到 UI、切换对话阶段）。
 */
static void default_on_enter(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t evt)
{
    (void)fsm;
    ESP_LOGI(TAG, "enter %s by %s", julia_fsm_sub_state_name(state), julia_fsm_event_name(evt));
}

static void default_on_exit(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t evt)
{
    (void)fsm;
    ESP_LOGI(TAG, "exit %s by %s", julia_fsm_sub_state_name(state), julia_fsm_event_name(evt));
}

/*
 * 执行一次状态迁移。前提是 next_state 已由两级派发确定为有效目标且不等于当前状态
 * （julia_fsm_handle_event 已保证 next_state < COUNT）。
 *
 * 执行顺序（有先后依赖，不可调换）：
 *   1) 若目标与当前相同 -> 直接返回，连回调都不触发（"自迁移"在此被短路）。
 *   2) on_exit(旧状态) —— 先"离开"，让旧状态有机会做收尾。
 *   3) 更新 sub_state 与 main_state（main_state 由映射表查得，保证两层一致）。
 *   4) on_enter(新状态) —— 再"进入"，让新状态有机会做初始化。
 *
 * 副作用：调用方绑定的 on_enter/on_exit 会被触发，可能连带 UI / 对话阶段联动。
 * NOTE：需结合调用方确认——on_exit 之后、on_enter 之前状态字段已更新，若回调
 * 内部再读 fsm->sub_state，读到的是"已进入新状态"还是"还在旧状态"取决于调用方
 * 对缓冲的理解；当前 julia_voice 的回调不读该字段，故此边界未暴露问题。
 */
static void julia_fsm_transition(julia_fsm_t *fsm, julia_sub_state_t next_state, fsm_event_t evt)
{
    julia_sub_state_t prev_state = fsm->sub_state;
    julia_main_state_t prev_main_state = fsm->main_state;

    if (prev_state == next_state) {
        return;
    }

    if (fsm->on_exit != NULL) {
        fsm->on_exit(fsm, prev_state, evt);
    }

    fsm->sub_state = next_state;
    fsm->main_state = s_sub_state_info[next_state].main_state;

    /* NOTE：死代码段（潜在问题，仅报告不修改）——本分支计算了 summary 却从未被
     * 使用（既没有写入日志，也没有保存），等于一次无意义的 snprintf。若本意是打
     * "主状态迁移"日志，应改为使用下面的 sub_state 日志之外再单独打印主状态变化。 */
    if (fsm->main_state != prev_main_state) {
        char summary[16];
        snprintf(summary, sizeof(summary), "S%d->S%d", prev_main_state, fsm->main_state);
    }

    ESP_LOGI(TAG, "[FSM] %s -> %s (原因: %s)",
             julia_fsm_sub_state_name(prev_state),
             julia_fsm_sub_state_name(next_state),
             julia_fsm_event_name(evt));

    if (fsm->on_enter != NULL) {
        fsm->on_enter(fsm, next_state, evt);
    }
}

/*
 * 全局事件处理器：这些事件无论当前处于哪个子状态都会强制迁移到固定目标。
 *
 * 为什么采用"全局优先"而非交给各状态分别处理？因为这些事件是"硬性事实"，
 * 与当前行为档位无关、不能因为"正在对话/正在陪伴"就被忽略：
 *   - EVT_EMOTION_DETECTED -> S3_1：检测到用户情绪波动，立即转为主动共情，
 *     对话/陪伴等任何状态都让位于情绪回应。
 *   - EVT_CHARGE_START -> S1_3：插上充电器，立刻进入充电待机（不被对话打断）。
 *   - EVT_CHARGE_DONE / EVT_WAKEUP -> S1_1：充满电或唤醒，统一回到近场待机这个
 *     "可被再次启动"的中性起点。
 *   - EVT_LOW_BATTERY / EVT_MANUAL_SLEEP -> S0_3：低电量或用户手动要求入睡，
 *     一律进入手动休眠（省电 + 尊重用户指令）。
 *   - EVT_NIGHT_TIME -> S0_1：进入夜间睡眠；EVT_DAY_AWAY -> S0_2：判定为白天离家，
 *     二者都直接把状态拉到对应睡眠档位（见 julia_context 的判定逻辑）。
 *
 * 返回 COUNT 表示"该事件不是全局事件"，交由当前状态处理器决定。
 * NOTE：需结合调用方确认——EVT_WAKEUP 在此被当作"回到 S1_1"，但 julia_voice.c 的
 * return_to_standby() 会先发 SILENCE_TIMEOUT 再发 WAKEUP，二者都收敛到 S1_1，
 * 语义上 WAKEUP 是否应区分"唤醒来源"（如主动叫醒 vs 回待机）尚无证据。
 */
static julia_sub_state_t handle_global_event(julia_fsm_t *fsm, fsm_event_t evt)
{
    (void)fsm;

    switch (evt) {
    case EVT_EMOTION_DETECTED:
        return JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER;
    case EVT_CHARGE_START:
        return JULIA_SUB_STATE_S1_3_CHARGING_STANDBY;
    case EVT_CHARGE_DONE:
    case EVT_WAKEUP:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    case EVT_LOW_BATTERY:
    case EVT_MANUAL_SLEEP:
        return JULIA_SUB_STATE_S0_3_MANUAL_SLEEP;
    case EVT_NIGHT_TIME:
        return JULIA_SUB_STATE_S0_1_NIGHT_SLEEP;
    case EVT_DAY_AWAY:
        return JULIA_SUB_STATE_S0_2_DAY_AWAY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/*
 * 睡眠共同处理器：S0.1 夜间睡眠 / S0.2 白天离家 / S0.3 手动休眠 三档共用。
 *
 * 睡眠档位对外界几乎"无响应"，仅当用户出现或呼叫、或收到唤醒事件时才苏醒，
 * 统一回到近场待机 S1_1 这一中性起点。其余事件（含低级打断如静默超时）一律忽略，
 * 因为睡眠状态下任何"陪伴/主动/对话"入口都应以"先醒过来"为前提。
 */
static julia_sub_state_t handle_sleep_common(fsm_event_t evt)
{
    switch (evt) {
    case EVT_USER_RETURN:
    case EVT_USER_CALL:
    case EVT_WAKEUP:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/*
 * 主动共同处理器：S3.1 情绪触发 / S3.2 日常打断 / S3.3 用户呼唤 / S3.4 恢复试探 共用。
 *
 * 这些子状态都是 Julia 自发的"主动行为"，用户一旦回应就应切换：
 *   - EVT_START_DIALOG -> S4_1：用户开口了，主动行为自然过渡到浅层对话。
 *   - EVT_USER_REJECT / EVT_USER_PERFUNCTORY -> S5_1 / S5_2：用户通过负反馈叫停，
 *     主动行为立即让位于"静默/敷衍"档位，避免顶撞用户。
 *   - EVT_SILENCE_TIMEOUT -> S1_1：主动行为也没换来交流（长时间静默），回落待机。
 *
 * 其余事件（如继续触发主动）在主动态下被忽略——避免同一行为反复自我触发。
 */
static julia_sub_state_t handle_initiative_common(fsm_event_t evt)
{
    switch (evt) {
    case EVT_START_DIALOG:
        return JULIA_SUB_STATE_S4_1_LIGHT_DIALOG;
    case EVT_USER_REJECT:
        return JULIA_SUB_STATE_S5_1_USER_REJECT;
    case EVT_USER_PERFUNCTORY:
        return JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY;
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/*
 * 拒绝共同处理器：S5.1 用户拒绝 / S5.2 敷衍应对 共用（S5.3 用户离开 单独处理）。
 *
 * 处于"静默/被拒绝"档位时的关键语义：Julia 需要克制，绝不主动再开口，
 * 因此绝大多数事件在此被忽略。仅在两种情况下脱出：
 *   - EVT_SILENCE_TIMEOUT -> S1_1：被拒绝后的冷处理也结束了（用户长时间没再说话），
 *     回到可自由行动的近场待机。
 *   - EVT_RECOVERY_ATTEMPT / EVT_USER_CALL -> S3_4：系统尝试"恢复试探"，或用户
 *     主动再次呼叫，允许 Julia 谨慎地重新试探建立互动（仍带试探性质，非直接对话）。
 */
static julia_sub_state_t handle_rejection_common(fsm_event_t evt)
{
    switch (evt) {
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    case EVT_RECOVERY_ATTEMPT:
    case EVT_USER_CALL:
        return JULIA_SUB_STATE_S3_4_RECOVERY_PROBE;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/*
 * 对话共同处理器：S4.1~S4.4 的缺省分支，各对话态先处理自身特有事件，其余交给这里。
 *
 * 对话档位是"正在与用户交互"，因此这里的退出路径都围绕"对话怎么结束"：
 *   - EVT_USER_LEFT_DIALOG -> S5_3：对话途中用户离开 -> 用户离开静默档。
 *   - EVT_USER_REJECT / EVT_USER_PERFUNCTORY -> S5_1 / S5_2：用户用负反馈结束对话，
 *     进入静默/敷衍，避免继续纠缠。
 *   - EVT_SILENCE_TIMEOUT -> S1_1：对话后长时间静默（对话自然结束），回近场待机。
 *
 * 其余事件（如 CHARGE_START 之外的对话推进事件）在对话态被忽略；对话推进由
 * 各 S4.x 处理器自己的特例（DEEP_TALK/MULTI_TURN/INTERRUPT 等）负责。
 */
static julia_sub_state_t handle_dialog_common(fsm_event_t evt)
{
    switch (evt) {
    case EVT_USER_LEFT_DIALOG:
        return JULIA_SUB_STATE_S5_3_USER_LEFT;
    case EVT_USER_REJECT:
        return JULIA_SUB_STATE_S5_1_USER_REJECT;
    case EVT_USER_PERFUNCTORY:
        return JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY;
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/* S0.1 / S0.2 / S0.3：三个睡眠档位完全复用 sleep_common，行为一致（见其注释）。
 * 三档之间的区分只由"谁进入"决定（NIGHT_TIME / DAY_AWAY / LOW_BATTERY_MANUAL），
 * 进入后对外部事件的响应完全相同，故无各自特例。 */
static julia_sub_state_t handle_state_s0_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_sleep_common(evt);
}

static julia_sub_state_t handle_state_s0_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_sleep_common(evt);
}

static julia_sub_state_t handle_state_s0_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_sleep_common(evt);
}

/*
 * S1.1 近场待机：各种"从中性点启动"的最重要出口状态，几乎承接了所有入口事件。
 *   - EVT_USER_LEAVE -> S1_2：用户离开，退到远场待机（收紧到"人不在场"档）。
 *   - EVT_EMOTION_DETECTED -> S3_1 / EVT_ROUTINE_BREAK -> S3_2 / EVT_USER_CALL -> S3_3：
 *     分别转入"主动"的三个来源（情绪触发 / 日常打断 / 用户呼唤）。
 *   - EVT_BEDTIME -> S2_3：到 22 点睡前档，进入睡前陪伴。
 *   - EVT_SHARED_ACTIVITY_START -> S2_2：开始共享活动 -> 陪伴中的共享活动。
 *   - EVT_SILENCE_TIMEOUT -> S2_1：待机久了没人互动，进入"陪伴-观察"去主动探看。
 * 其余事件在这里被忽略（例如再次 BEDTIME 或 SHARED_ACTIVITY_STOP 均无意义）。
 */
static julia_sub_state_t handle_state_s1_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_USER_LEAVE:
        return JULIA_SUB_STATE_S1_2_FAR_STANDBY;
    case EVT_EMOTION_DETECTED:
        return JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER;
    case EVT_ROUTINE_BREAK:
        return JULIA_SUB_STATE_S3_2_ROUTINE_BREAK;
    case EVT_USER_CALL:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    case EVT_BEDTIME:
        return JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION;
    case EVT_SHARED_ACTIVITY_START:
        return JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY;
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S2_1_OBSERVE;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/*
 * S1.2 远场待机：用户不在近旁、只保持"最低限度"的警惕。
 *   - EVT_USER_RETURN -> S1_1：用户回到身边，升为近场待机。
 *   - EVT_RECOVERY_ATTEMPT -> S3_4：系统尝试恢复试探 -> 主动-恢复试探。
 *   - EVT_SILENCE_TIMEOUT -> S2_1：久无动静但仍希望陪伴 -> 观察档。
 *   - EVT_USER_CALL -> S3_3：远场也被叫到 -> 主动-用户呼唤。
 * 其余事件（如 SHARED_ACTIVITY_START / BEDTIME）在远场被忽略——人不在，
 * 这些陪伴型入口没有意义。
 */
static julia_sub_state_t handle_state_s1_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_USER_RETURN:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    case EVT_RECOVERY_ATTEMPT:
        return JULIA_SUB_STATE_S3_4_RECOVERY_PROBE;
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S2_1_OBSERVE;
    case EVT_USER_CALL:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/*
 * S1.3 充电待机：硬件上正在充电，Julia 处于待机但以省电优先。
 * 事件面被刻意收窄：仅响应用户呼叫（S3_3）或用户出现（S1_1），
 * 其余陪伴/主动类事件都被忽略——充电时不做费电的分神行为。
 */
static julia_sub_state_t handle_state_s1_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_USER_CALL:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    case EVT_USER_RETURN:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/*
 * S2.1 观察：陪伴下的"静静看"，是陪伴态的默认档。
 *   - EVT_SHARED_ACTIVITY_START -> S2_2：进入共享活动。
 *   - EVT_BEDTIME -> S2_3：到睡前档。
 *   - EVT_USER_CALL -> S3_3：用户呼唤 -> 主动。
 *   - EVT_USER_LEAVE -> S1_2：用户离开 -> 远场待机。
 * 其余事件（如 START_DIALOG）被忽略：观察档不会主动起对话——对话应来自用户开口
 * 或主动档，而非观察档自发起话。
 */
static julia_sub_state_t handle_state_s2_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_SHARED_ACTIVITY_START:
        return JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY;
    case EVT_BEDTIME:
        return JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION;
    case EVT_USER_CALL:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    case EVT_USER_LEAVE:
        return JULIA_SUB_STATE_S1_2_FAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/*
 * S2.2 共享活动：用户与 Julia 在共同做一件事（如一起看/一起玩）。
 *   - EVT_SHARED_ACTIVITY_STOP -> S2_1：活动结束，回到观察档。
 *   - EVT_START_DIALOG -> S4_3：活动期间用户开口 -> 直接进入多轮对话。
 *   - EVT_USER_LEAVE -> S1_2：用户走了 -> 远场待机。
 * 其余事件（如再次 SHARED_ACTIVITY_START、BEDTIME）被忽略——活动已在进行，
 * 重复的"开始"事件没有意义。
 */
static julia_sub_state_t handle_state_s2_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_SHARED_ACTIVITY_STOP:
        return JULIA_SUB_STATE_S2_1_OBSERVE;
    case EVT_START_DIALOG:
        return JULIA_SUB_STATE_S4_3_MULTI_TURN;
    case EVT_USER_LEAVE:
        return JULIA_SUB_STATE_S1_2_FAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/*
 * S2.3 睡前陪伴：接近睡觉时间、Julia 仍陪在身边但逐渐趋向安静。
 *   - EVT_SILENCE_TIMEOUT -> S0_1：陪伴也没换来交流（睡前安静），顺势进入夜间睡眠。
 *   - EVT_USER_CALL / EVT_START_DIALOG -> S4_1：用户睡前还想聊聊 -> 浅层对话。
 * 其它事件被忽略：睡前档刻意弱化主动/活动，避免睡前还被打扰。
 */
static julia_sub_state_t handle_state_s2_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S0_1_NIGHT_SLEEP;
    case EVT_USER_CALL:
    case EVT_START_DIALOG:
        return JULIA_SUB_STATE_S4_1_LIGHT_DIALOG;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/* S3.1~S3.4：四个"主动"子状态完全复用 initiative_common（见其注释）。
 * 主动态的差异只体现在"进入来源"（情绪/打断/呼唤/试探），进入后对用户回应的
 * 处理路径一致，故无各自特例。 */
static julia_sub_state_t handle_state_s3_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_initiative_common(evt);
}

static julia_sub_state_t handle_state_s3_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_initiative_common(evt);
}

static julia_sub_state_t handle_state_s3_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_initiative_common(evt);
}

static julia_sub_state_t handle_state_s3_4(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_initiative_common(evt);
}

/*
 * S4.1 浅对话：对话强度的起步档。
 *   - EVT_DEEP_TALK_DETECTED -> S4_2：对话加深 -> 深谈。
 *   - EVT_MULTI_TURN_DETECTED -> S4_3：进入多轮 -> 多轮对话。
 *   - EVT_INTERRUPT -> S3_3：用户打断后直接让出播报并回到监听。
 *   其余交给 dialog_common（对话如何结束）。
 */
static julia_sub_state_t handle_state_s4_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_DEEP_TALK_DETECTED:
        return JULIA_SUB_STATE_S4_2_DEEP_TALK;
    case EVT_MULTI_TURN_DETECTED:
        return JULIA_SUB_STATE_S4_3_MULTI_TURN;
    case EVT_INTERRUPT:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    default:
        return handle_dialog_common(evt);
    }
}

/*
 * S4.2 深谈：对话已进入较深入的话题。
 *   - EVT_MULTI_TURN_DETECTED -> S4_3：继续加轮次 -> 多轮对话。
 *   - EVT_INTERRUPT -> S3_3：用户打断后直接回到监听。
 *   其余交给 dialog_common。这里不再支持"降回浅对话"（无对应事件），
 *   说明一旦深谈只能顺着走或结束，不会自行变浅。
 */
static julia_sub_state_t handle_state_s4_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_MULTI_TURN_DETECTED:
        return JULIA_SUB_STATE_S4_3_MULTI_TURN;
    case EVT_INTERRUPT:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    default:
        return handle_dialog_common(evt);
    }
}

/*
 * S4.3 多轮对话：对话的"最活跃"档，除了被打断不再升级，其余交给 dialog_common
 * （用户离开/拒绝/敷衍/静默都是结束路径）。DEEP_TALK/MULTI_TURN 在此被忽略，
 * 因为已经是最深档，重复触发没有意义。打断作为瞬时事件直接进入 S3.3 LISTEN，
 * 不在 S4.4 停留；停止扬声器、取消旧回答等动作由语音层在投递事件前完成。
 */
static julia_sub_state_t handle_state_s4_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_INTERRUPT:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    default:
        return handle_dialog_common(evt);
    }
}

/*
 * S4.4 打断处理：为以后需要等待服务端取消确认等异步恢复过程保留。
 * 当前最小打断链路不进入此状态，而是由 S4.1~S4.3 直接转到 S3.3。
 *   - EVT_START_DIALOG -> S4_1：用户重新开始 -> 回到浅对话。
 *   其余交给 dialog_common（若用户顺势离开/拒绝/静默则退出对话）。
 */
static julia_sub_state_t handle_state_s4_4(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_START_DIALOG:
        return JULIA_SUB_STATE_S4_1_LIGHT_DIALOG;
    default:
        return handle_dialog_common(evt);
    }
}

/* S5.1 / S5.2：静默-拒绝与静默-敷衍 复用 rejection_common（见其注释）。 */
static julia_sub_state_t handle_state_s5_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_rejection_common(evt);
}

static julia_sub_state_t handle_state_s5_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_rejection_common(evt);
}

/*
 * S5.3 用户离开：用户离场后的静默档，与 S5.1/S5.2 的"克制"略有不同。
 *   - EVT_USER_RETURN -> S4_1：用户回来且之前有过对话 -> 用保存的对话历史续聊
 *     （此处依赖调用方 julia_memory 保存的上下文，见现有英文注释）。
 *   - EVT_SILENCE_TIMEOUT -> S1_2：用户始终没再出现 -> 退到远场待机。
 *   其余事件（含 RECOVERY_ATTEMPT）被忽略——用户都走了，试探没有意义，
 *   只能等用户自己回来。
 */
static julia_sub_state_t handle_state_s5_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    switch (evt) {
    case EVT_USER_RETURN:
        /* The dialogue owner can use the saved conversation history to resume. */
        return JULIA_SUB_STATE_S4_1_LIGHT_DIALOG;
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S1_2_FAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

/*
 * 名称查询接口：把枚举翻译成可读字符串，供日志/诊断使用。
 * 越界或空名一律返回占位符，避免崩溃；这些函数不参与状态迁移逻辑。
 */
const char *julia_fsm_main_state_name(julia_main_state_t state)
{
    if (state >= JULIA_MAIN_STATE_COUNT || s_main_state_names[state] == NULL) {
        return "UNKNOWN_MAIN";
    }
    return s_main_state_names[state];
}

const char *julia_fsm_sub_state_name(julia_sub_state_t state)
{
    if (state >= JULIA_SUB_STATE_COUNT || s_sub_state_info[state].name == NULL) {
        return "UNKNOWN_SUB";
    }
    return s_sub_state_info[state].name;
}

const char *julia_fsm_event_name(fsm_event_t evt)
{
    if ((size_t)evt >= sizeof(s_event_names) / sizeof(s_event_names[0]) || s_event_names[evt] == NULL) {
        return "UNKNOWN_EVT";
    }
    return s_event_names[evt];
}

/*
 * @brief 初始化 FSM。前置条件：fsm 指向有效内存。
 * @note  进入状态固定为 S1.1 近场待机（而非睡眠态），因为设备上电默认应是
 *        "可被唤醒、可被启动"的中性档；睡眠需由时间/低电量等事件主动进入。
 *        初始化末尾会调用一次 on_enter（事件为 EVT_NONE），因此调用方若在
 *        init 之后再绑定业务 on_enter，需注意这次调用用的是缺省回调
 *        （julia_voice_init 即在 init 之后才覆盖 on_enter，故不会误触发 UI）。
 */
void julia_fsm_init(julia_fsm_t *fsm)
{
    if (fsm == NULL) {
        return;
    }

    fsm->main_state = JULIA_MAIN_STATE_S1_STANDBY;
    fsm->sub_state = JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    fsm->on_enter = default_on_enter;
    fsm->on_exit = default_on_exit;
    fsm->user_ctx = NULL;

    if (fsm->on_enter != NULL) {
        fsm->on_enter(fsm, fsm->sub_state, EVT_NONE);
    }
}

/*
 * @brief 事件入口：两级派发，返回是否有状态迁移发生。
 * @param fsm   状态机实例，须已 init；不能为 NULL 且 sub_state 必须合法。
 * @param evt   输入事件。
 * @param data  透传给状态处理器的不透明数据（当前各处理器均未使用，传 NULL 即可）。
 * @return true 发生了一次有效迁移；false 事件被忽略（无迁移）。
 *
 * 调用上下文：由 julia_voice_handle_event() 在持有 s_fsm_lock 的前提下调用，
 * 因此同一实例不会并发进入本函数；不要在中断里调用。
 *
 * 处理流程：先查全局事件（强制迁移），否则查当前子状态处理器；两次都返回
 * COUNT 表示忽略（打印日志后返回 false）。迁移动作在 julia_fsm_transition 中完成。
 *
 * NOTE：data 目前在所有 handle_state_* 与 common 处理中都未被读取（全被 (void)data），
 * 但签名故意保留，便于将来携带"触发来源/数据"到具体状态处理。
 */
bool julia_fsm_handle_event(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    if (fsm == NULL || fsm->sub_state >= JULIA_SUB_STATE_COUNT) {
        return false;
    }

    julia_sub_state_t next_state = handle_global_event(fsm, evt);
    if (next_state == JULIA_SUB_STATE_COUNT) {
        julia_state_handler_t handler = s_state_handlers[fsm->sub_state];
        if (handler == NULL) {
            ESP_LOGW(TAG, "[FSM] no handler for state %s", julia_fsm_sub_state_name(fsm->sub_state));
            return false;
        }
        next_state = handler(fsm, evt, data);
    }

    if (next_state < JULIA_SUB_STATE_COUNT) {
        julia_fsm_transition(fsm, next_state, evt);
        return true;
    }

    ESP_LOGI(TAG, "[FSM] %s ignored event %s",
             julia_fsm_sub_state_name(fsm->sub_state),
             julia_fsm_event_name(evt));
    return false;
}
