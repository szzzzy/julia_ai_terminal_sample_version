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
 * 外设启动前完成镜像识别、NVS 保护、netif/event loop 与回滚记录对账。
 * 仅 pending 镜像的可选 GPIO 诊断在此执行，避免运行期重配复用引脚。
 * 其余验收由 complete 在本地结果明确后执行；基础设施失败进入安全模式。
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

/** 线程安全：镜像确认和对账结束前拒绝下一次升级。 */
bool ota_boot_flow_pending(void);

#ifdef __cplusplus
}
#endif
