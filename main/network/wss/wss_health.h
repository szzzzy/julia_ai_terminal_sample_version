#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * @file wss_health.h
 * @brief 会话任务的"是否仍在推进"快照与两级恢复判据（头文件内联，唯一调用方是
 *        main/network/wss/wss_transport.c）。
 *
 * 边界：本文件只提供纯计算与状态容器，不建连接、不记录故障，也不重启设备。
 * 时间基准：since_us / progress_us / budget_us 都是 esp_timer 的单调微秒（启动以来），
 * 与墙钟无关。所有字段由 wss_transport.c 在 s_start_lock 下整体更新，读取方必须整体
 * 拷贝一份再判断，避免读到半更新状态。
 *
 * 不变量：**离线本身不算停滞**。只有 phase 非 STOPPED、且距最近进展超过本阶段预算时，
 * 才判为 stalled；断网重连不会被误当成任务卡死。
 *
 * 阶段与预算（由 wss_transport.c 的 wss_phase 调用点给出，单位毫秒）：
 * STOPPED=0（未运行，永不判停滞）；CONNECT=60000；HANDSHAKE=30000；RX=30000；
 * TX/KEEPALIVE=15000；CALLBACK=30000；FSM_WAIT=20000；CLEANUP=30000；
 * BACKOFF=本次退避时长 + 30000 裕量。
 */
typedef enum {
    WSS_PHASE_STOPPED, WSS_PHASE_CONNECT, WSS_PHASE_HANDSHAKE,
    WSS_PHASE_RX, WSS_PHASE_TX, WSS_PHASE_CALLBACK, WSS_PHASE_FSM_WAIT,
    WSS_PHASE_CLEANUP, WSS_PHASE_BACKOFF, WSS_PHASE_KEEPALIVE
} wss_phase_t;

/* 一次按值拷贝的健康快照：phase=当前阶段；since_us=进入该阶段的时刻（µs，单调时钟）；
 * progress_us=最近一次确认"还在推进"的时刻（µs），0 表示尚未有过进展；budget_us=本阶段
 * 允许无进展的最长时间（µs）；generation=当前会话的数据归属编号，0 表示尚无有效会话；
 * attempts/failures=连接尝试与失败次数，仅作诊断计数。 */
typedef struct {
    wss_phase_t phase;
    int64_t since_us;
    int64_t progress_us;
    int64_t budget_us;
    uint32_t generation;
    uint32_t attempts;
    uint32_t failures;
} wss_health_t;

/** phase 非 STOPPED、且距最近进展超过本阶段预算时判为停滞。 */
static inline bool wss_health_stalled(const wss_health_t *h, int64_t now)
{
    return h->phase != WSS_PHASE_STOPPED && h->progress_us > 0 &&
           now - h->progress_us > h->budget_us;
}

/* 两级恢复判据：返回 0 表示无需动作，1 表示刚发现停滞（请求 owner 清理会话），
 * 2 表示停滞已持续到升级阈值（记录 CORE_TASK_STALLED 并可能受控重启）。
 * 离线不是停滞，只有任务不再推进才可能升级。stalled_since 由调用方保存，非停滞时必须
 * 清 0，因此本函数只能由单一监测者（wss_transport.c 的 monitor 任务）调用。
 * 120 s（120000000 µs）为升级阈值，其取值依据未确认，需上板确认。 */
static inline unsigned wss_health_recovery_level(bool stalled, int64_t now,
                                                int64_t *stalled_since)
{
    if (!stalled) { *stalled_since = 0; return 0; }
    if (!*stalled_since) { *stalled_since = now; return 1; }
    return now - *stalled_since >= 120000000LL ? 2 : 0;
}
