/**
 * @file    voice_push_demo.h
 * @brief   设备主动推送演示：显式传输项目测试音频到 WSS 服务器。
 *
 * 与 OTA 检查（设备主动 publish ota_check）不同，WSS 语音通道的协议默认是
 * 服务端驱动（FILE_SEND/MIC_START）。本模块演示设备主动写入：WSS 客户端
 * 启动后，把 voice_push_demo.c 内固定的一批测试音频文件名按
 * "SD:/<文件名>" 逐一入队，由 voice_service 的 WSS 会话在连接就绪后推给
 * 服务器，全程不需要任何服务端命令。文件名与素材目录没有自动对应关系。
 *
 * 注意：演示与服务器下发的 FILE_SEND 共用同一个预约槽（voice_service_file_busy()
 * 同时反映两者），因此启用后：
 *   - 服务器在同一时刻发起的 FILE_SEND 会被 ERROR file_busy 拒绝；
 *   - 每个文件传输期间 MIC 上行被暂停并清空，正常语音对话会被打断。
 *
 * 本演示默认编译关闭（CONFIG_VOICE_PUSH_DEMO_ENABLE=n，见 sdkconfig）；关闭时
 * voice_push_demo_start() 返回 ESP_ERR_NOT_SUPPORTED，不创建任何任务。
 *
 * 该流程没有对应的服务端 FILE_SEND 命令：仓库内的协议文档只描述服务端发起的文件外发，
 * 未记录服务端是否接受主动写入的 BEGIN FILE 流，也未定义它的成功回执。因此“未在期限内
 * 收到确认”只说明结果未知，不能据此判断对端不支持该流程。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动设备主动推送（显式传输）演示任务。
 *
 * 行为由配置项决定：
 * - CONFIG_VOICE_PUSH_DEMO_INTERVAL_SECONDS == 0：WSS 客户端启动后把文件列表
 *   显式传输一次；
 * - > 0：每 N 秒重复传输整个列表。
 *
 * @return ESP_OK 演示任务已创建。
 * @return ESP_ERR_NOT_SUPPORTED 演示未启用（CONFIG_VOICE_PUSH_DEMO_ENABLE=n）。
 * @return ESP_ERR_NO_MEM 演示任务创建失败。
 *
 * @note 幂等：重复调用不会创建第二个任务。CONFIG_VOICE_PUSH_DEMO_INTERVAL_SECONDS==0
 *       时任务在传完一轮后自删除，但 s_demo_started 仍为 true，因此这是“只能启动
 *       一次、不能重启”的幂等，再次分发只能通过复位设备。
 * @note 本函数不阻塞；演示任务在后台执行，不允许在中断上下文调用。
 */
esp_err_t voice_push_demo_start(void);

#ifdef __cplusplus
}
#endif
