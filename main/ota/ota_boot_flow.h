/**
 * @file    ota_boot_flow.h
 * @brief   OTA 镜像启动验收与回滚流程。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在业务服务启动前完成 OTA 启动验收和状态对账。
 *
 * 函数初始化 NVS、网络接口和默认事件循环，检查 PENDING_VERIFY 镜像，按本地
 * 健康检查结果确认或回滚固件，并清理已经确认版本的恢复记录。不可恢复的错误
 * 会进入 OTA 安全模式，因此正常情况下函数完成后即可继续初始化业务服务。
 *
 * 生命周期（启动侧）：此前引擎已把镜像写入并切换启动分区，设备重启后运行镜像
 * 进入本函数。此时状态可能是 booted_pending_verify → succeeded（本地健康检查通过
 * 并 confirm），或检测到无效镜像 → rolled_back（自检/产品检查失败、确认失败或
 * 回滚不可用）。failed / deferred 属于引擎侧的收纳状态，不会在启动验收分支产生；
 * 正常返回后应用继续读取 ota_state_store 记录并与运行版本对账（reconcile）。
 *
 * @note 本函数由 app_main 最先调用（早于网络与业务服务）；其内部可能因 GPIO
 *       诊断阻塞约 5 s，且不可恢复错误会调用 ota_boot_health_enter_safe_mode()
 *       永久停留，因此只能在普通任务上下文调用。
 */
void ota_boot_flow_run(void);

#ifdef __cplusplus
}
#endif
