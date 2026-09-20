#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @file julia_sync_request.h
 * @brief 同步事件请求（post_sync）的引用计数与截止时间对象，供 behavior/julia_fsm_runtime.c
 *        判断"这次请求是否还值得提交/回收"。
 *
 * 所有权与并发：对象来自 fsm_runtime 的静态池 `s_requests[FSM_EVENT_QUEUE_DEPTH + 2]`，
 * 全部字段只在 runtime 的请求自旋锁（`s_request_lock`）下读写，本文件不提供锁。
 * 引用有两个：调用方一个、已入队消息/owner 一个；`julia_sync_request_complete()` 只递减一次。
 *
 * 不变量：超时的调用方**不得回收 owner 仍持有的存储**——对象归还池子只由 owner 侧在完成
 * 处理时决定，调用方超时只放弃等待。
 */

/** 一次同步请求的状态。除 refs 外都只在锁内读写。 */
typedef struct {
    unsigned refs;          /**< 剩余引用数（调用方 + 已入队/owner），由 complete() 递减一次。 */
    int64_t deadline_us;    /**< 绝对截止时刻，单位微秒（esp_timer 单调时钟）。 */
    uint32_t generation;    /**< 该请求归属的 WSS generation；0 表示不校验代次。 */
    bool cancelled;         /**< owner 已取消该请求；取消后调用方不再等待。 */
    bool committed;         /**< FSM 已在截止时间前提交该请求的状态变更。 */
    bool done;              /**< complete() 已执行，结果已写回。 */
    bool applied;           /**< 结果：本次请求是否真的改变状态（与 done 同时写入）。 */
} julia_sync_request_t;

/** 请求是否仍然有效：未被取消且尚未到截止时刻。 */
static inline bool julia_sync_request_live(const julia_sync_request_t *r, int64_t now)
{
    return !r->cancelled && now < r->deadline_us;
}

/**
 * 写回结果并释放引用。要求：由 owner 在完成处理时调用且只调用一次；重复调用会让 refs 变成
 * 负数，并破坏"调用方与消息各持一份引用"的约定。
 */
static inline void julia_sync_request_complete(julia_sync_request_t *r, bool applied)
{
    r->applied = applied;
    r->done = true;
    --r->refs;
}
