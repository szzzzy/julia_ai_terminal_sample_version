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
 */
void ota_boot_flow_run(void);

#ifdef __cplusplus
}
#endif
