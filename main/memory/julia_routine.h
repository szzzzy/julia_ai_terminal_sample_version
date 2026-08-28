#pragma once

/**
 * @file    julia_routine.h
 * @brief   例行检测（routine / baseline）模块——记录活动规律并检测“日常偏差”。
 *
 * 数据来源：各任务按活动类型上报——语音对话任务（DIALOG）、唤醒词（WAKE）、
 * 按钮/人机交互（BUTTON）、传感器/IMU（SENSOR，由 julia_context 上报）。
 * 输出：julia_routine_is_deviation() 由 context 任务每 500ms 轮询，检测到偏差时
 * 触发 julia_voice_handle_event(EVT_ROUTINE_BREAK) 进入“例行偏差”子状态。
 *
 * 配置概览（具体见 julia_routine.c）：
 *  - 以“天”为窗口、按“小时”分桶；ROUTINE_DAYS=7 天滚动，桶存交互次数/活跃分钟。
 *  - 基线持久化在 SD 卡 routine_v1.bin（双份拷贝 + generation 老化回退）。
 *  - 偏差判据：当前小时在本窗口活跃天数占比过低(<15%)，且当前连续活跃 >=30 分钟，
 *    且距上次触发 >=1 小时（冷却）。
 *
 * NOTE：julia_routine_init() 在本工程中暂未见调用点（见报告）；若未初始化，
 *       各入口因 s_lock 为空会静默 no-op（on_activity/is_deviation 返回 false），
 *       偏差检测不会生效。
 */

#include <stdbool.h>
#include "esp_err.h"

/* 活动来源。值为上报者约定的“活动种类”，影响桶内统计口径：
 *  DIALOG/WAKE 会累计交互次数（interactions），SENSOR 仅计活跃分钟。 */
typedef enum {
    JULIA_ACTIVITY_DIALOG = 0,
    JULIA_ACTIVITY_WAKE,
    JULIA_ACTIVITY_BUTTON,
    JULIA_ACTIVITY_SENSOR,
} activity_kind_t;

/* 初始化例行检测。前置：SD 已挂载；加载/初始化基线并保存到 SD（双份拷贝）。
 * 必须在任何上报/查询之前调用一次。 */
esp_err_t julia_routine_init(void);

/* 上报一次活动（来自 DIALOG/WAKE/BUTTON/SENSOR 之一）。仅在时钟已同步(>=2024)
 * 时记账；按当前小时更新桶的交互次数/活跃分钟，并刷新“连续活跃”起始点。
 * 只置脏标记，不立即落盘（由后台 flush 按 10 分钟周期落盘）。 */
void julia_routine_on_activity(activity_kind_t kind);

/* 检测“日常偏差”：对当前小时的活跃比例、连续活跃时长、冷却三者做判定，
 * 成立则触发 EVT_ROUTINE_BREAK 并返回 true。由 julia_context 任务每 500ms 轮询。
 * 失败/未初始化/时钟未同步时返回 false。 */
bool julia_routine_is_deviation(void);

/* 立即把当前基线落盘（写快照 + generation + CRC）。 */
esp_err_t julia_routine_flush(void);
