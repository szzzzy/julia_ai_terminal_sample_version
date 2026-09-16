/**
 * @file    ota_boot_flow.h
 * @brief   新固件首次启动时先完成本地检查；不合格就恢复上一可用版本。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在屏幕、语音和网络业务启动前确认当前固件是否可以继续运行。
 *
 * 如果当前是刚升级的新固件，则执行本地健康检查；通过后仍保持待确认，失败时立即
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

/**
 * @brief app_main 完成关键显示、音频、语音和 FSM 初始化后调用一次。
 *
 * @param[in] app_healthy 关键本地服务是否全部就绪；只允许依据本地初始化结果判断，
 *            不得以 Wi-Fi/MQTT/DNS 等远程可用性为条件——弱网不代表镜像不健康。
 *
 * app_healthy=false 时待验证镜像立即上报回滚并请求 rollback；已确认镜像仍走应用原有
 * 故障处理。本函数只对 PENDING_VERIFY 启动生效，重复调用是空操作。
 *
 * @note 漏调的后果：镜像一直是 PENDING_VERIFY，本次启动不会产生 succeeded 上报，
 *       下一次复位时 bootloader 会判其无效并回滚；因此必须在本文件约定的时机调用。
 */
void ota_boot_flow_complete(bool app_healthy);

#ifdef __cplusplus
}
#endif
