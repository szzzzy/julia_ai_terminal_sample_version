/**
 * @file    ota_boot_flow.h
 * @brief   新固件首次启动时先完成本地检查；不合格就恢复上一可用版本。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在屏幕、语音和网络业务启动前确认当前固件是否可以继续运行。
 *
 * 如果当前是刚升级的新固件，则执行本地健康检查；通过后正式确认，失败时立即
 * 请求回滚。已经由 bootloader 回滚的情况会与之前任务对账并报告服务器。
 * 没有可恢复版本或连本地状态都无法确认时进入安全模式，不继续启动业务。
 *
 * 服务器会先看到“新固件等待确认”，随后看到成功或回滚。下载失败和暂缓重启发生
 * 在升级任务阶段，不由本函数产生。
 *
 * @note 本函数由 app_main 最先调用（早于网络与业务服务）；其内部可能因 GPIO
 *       诊断阻塞约 5 s，且不可恢复错误会调用 ota_boot_health_enter_safe_mode()
 *       永久停留，因此只能在普通任务上下文调用。
 */
void ota_boot_flow_run(void);

#ifdef __cplusplus
}
#endif
