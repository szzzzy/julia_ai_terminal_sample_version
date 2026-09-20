/**
 * @file    boot_coordinator.h
 * @brief   并发执行开机期的显示/音频/资源三条本地分支，并在必要资源就绪时放行云门控。
 *
 * 分工：本模块只做启动阶段的编排与门槛，不实现分支内部逻辑（分支定义在 app/main.c），
 * 不改变启动后的行为策略（FSM 归 behavior/julia_fsm_runtime.c），也不管理 Wi-Fi 连接
 * 与联网服务重试（归 network/network_lifecycle.c）；resources_ready 由调用方提供，
 * 本模块不理解它的内部动作。
 *
 * 取舍与前提：三条分支并发是为了让本地初始化与联网过程重叠，代价是分支之间不能共享
 * 同一把驱动锁，也不能各自并发创建同一总线，因此共享外设（例如 I2C）必须在调用本模块
 * 之前建好。启动预算与时序口径见 docs/BOOT_INITIALIZATION.md。
 */

#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 分支阶段函数；run 与 finish 都在所属分支的 worker Task 中调用。
 *
 * 不要假设自己运行在 app_main 或网络任务上：三条分支是并发的，因此这里不得重试创建
 * 共享总线，也不得删除本分支之外的任务。
 */
typedef esp_err_t (*boot_branch_fn_t)(void);

/**
 * @brief 一条启动分支；run 是必选主体，finish 是可选的显示尾段。
 */
typedef struct {
    /* 分支名：同时作为 worker Task 名与启动日志字段，必须为有效的非 NULL 字符串。 */
    const char *name;
    /* 分支主体；worker 启动后立即调用，不得为 NULL。 */
    boot_branch_fn_t run;
    /* 该分支 Task 的栈大小（字节）；三条分支的栈各自分配，随 worker 结束释放。 */
    uint32_t stack_bytes;
    /* 可选尾段，本工程仅 branches[0]（显示）使用：run 成功后先上报资源里程碑，再由
     * 同一 worker 执行该尾段（开机动画）；因此资源门槛不等尾段结束。只允许一个分支
     * 设置它，否则完成队列的槽位数量不再够用。 */
    boot_branch_fn_t finish;
} boot_branch_t;

/** 一个分支的汇合结果；只由协调者填写。 */
typedef struct {
    /* 结果已到达：可能是成功、分支失败，也可能是任务创建失败；false 表示超时时该分支
     * 尚未给出终态。 */
    bool completed;
    /* 仅在 completed 为 true 时有意义；未完成时保持进入函数时的 ESP_ERR_INVALID_STATE。 */
    esp_err_t error;
} boot_result_t;

/**
 * @brief 并发执行三条本地分支，等必要资源就绪后调用一次 resources_ready。
 *
 * 调用顺序：应在共享总线、语音装配和 network_lifecycle 服务登记完成之后调用；本函数
 * 阻塞等待，最长 timeout_ms，返回后由调用方逐项检查 results[]。
 *
 * 门槛语义：设置 finish 的分支（本工程仅 branches[0]）在 run 成功后先上报一次资源里程碑
 * 再执行尾段；协调者收齐三条里程碑后只在本函数上下文中调用一次 resources_ready，因此该
 * 回调可能与开机动画并行，不得假设画面已经交出。
 *
 * 超时语义：期限取 esp_timer 单调时钟的绝对时刻，按剩余时间阻塞等待而不忙轮询；命中期限
 * 时未完成分支的 results 保持 {false, ESP_ERR_INVALID_STATE}。
 *
 * 关键不变量：协调者与每个成功创建的 worker 各持一份上下文引用；超时不删除仍可能持有
 * 驱动锁的任务，晚到结果由 worker 自行发布并回收，调用方不得抢占画面、销毁任务或释放
 * 分支资源。
 *
 * @param[in]  branches        三条分支描述；run 不得为 NULL，name/stack_bytes 必须有效。
 * @param[out] results         长度 3 的结果数组；进入函数时会被整体覆盖。
 * @param[in]  timeout_ms      本地初始化与汇合的期限（ms）；调用方现传
 *                             CONFIG_JULIA_LOCAL_INIT_TIMEOUT_MS（文档记录默认 20000）。
 * @param[in]  resources_ready 云门控回调，可为 NULL；只在协调者上下文调用一次，其返回的
 *                             错误会原样返回给调用方。
 *
 * @return ESP_OK 三条分支都已给出终态结果，成败需逐个查看 results[i]；单个分支失败本身
 *         不会改变本返回值。
 * @return ESP_ERR_TIMEOUT 期限到达时仍有分支没有终态结果。
 * @return ESP_ERR_NO_MEM 上下文或完成队列分配失败。
 *
 * @note 不得从分支函数内部调用。每次调用都会新建 Task 并真正执行分支，因此一个启动序列
 *       只应调用一次。
 */
esp_err_t boot_coordinator_run(const boot_branch_t branches[3],
                               boot_result_t results[3], uint32_t timeout_ms,
                               boot_branch_fn_t resources_ready);
