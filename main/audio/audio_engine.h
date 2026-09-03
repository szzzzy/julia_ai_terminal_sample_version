/**
 * @file    audio_engine.h
 * @brief   在后台下载可复用的音频素材，校验完整后保存到独立数据分区。
 *
 * 同一时间只下载一个素材。网络中断时保存进度，恢复后继续；完整大小和 SHA-256
 * 与服务器清单一致后才标记可用并通知播放功能。素材写入独立数据分区，不会修改
 * 当前固件或 OTA 应用分区。
 *
 * 服务器报文由音频服务校验，网络发送由通信模块完成。本模块只负责下载、写入和
 * 完整性检查。音频下载与固件升级互斥；两者同时需要网络和 Flash 时，固件升级优先。
 *
 * 下载任务（audio_task，优先级 5、栈 12288 B）的状态机与资源占用见 audio_engine.c；
 * 关于"音频下载 vs I2S 采集/播放"：本模块只负责把音频素材落地到分区，不读写 I2S，
 * 真正的采集/回放由 components/julia_board_audio 与 main/voice 承担。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "ota_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 使用已经校验的服务器清单启动一次音频素材下载。
 *
 * 返回成功只表示后台任务已经创建，不表示素材已经下载完成。已有音频任务或固件
 * 升级正在运行时明确拒绝，调用方可稍后重新请求。
 *
 * @param[in] manifest 已通过音频控制面校验的清单，不允许为 NULL。
 *
 * @return ESP_OK 任务创建成功。
 * @return ESP_ERR_INVALID_ARG manifest 为 NULL。
 * @return ESP_ERR_INVALID_STATE 已有音频或 OTA 下载任务正在运行。
 * @return ESP_ERR_NO_MEM 清单副本分配或任务创建失败。
 *
 * @note 只创建任务，不在调用者上下文中执行下载；不允许在中断上下文调用。
 *       任务参数 = 优先级 5、栈 12288 B（"audio_task"），阻塞直到下载结束。
 *       任何错误路径都会确保清回单飞标志，因此失败后可直接重试。
 */
esp_err_t audio_engine_start(const native_audio_manifest_t *manifest);

/**
 * @brief 查询设备是否正在下载或校验音频素材。
 *
 * @return true 已有音频任务运行；false 空闲。
 *
 * @note 运行状态只保存在本模块内部，由短临界区保护。
 */
bool audio_engine_is_running(void);

/**
 * @brief 读取最近一次完整校验通过的音频素材版本。
 *
 * @param[out] version      接收 NUL 结尾版本字符串的缓冲区。
 * @param[in]  version_size 缓冲区容量，必须至少为 NATIVE_OTA_AUDIO_VERSION_SIZE。
 *
 * @return ESP_OK 版本读取成功；未安装过素材时输出 NATIVE_OTA_AUDIO_VERSION_UNKNOWN。
 * @return ESP_ERR_INVALID_ARG 参数无效。
 *
 * @note 实现读 NVS audio_resume/current_version；NVS 不可用时按未安装处理。
 */
esp_err_t audio_engine_get_current_version(char *version, size_t version_size);

/**
 * @brief 素材完整校验通过后通知实际使用该素材的产品功能。
 *
 * 默认实现只记录日志；产品板级代码（如 JULIA 播放栈）可提供同名强符号覆盖，
 * 从音频数据分区读取 stored_size 字节并播放。
 *
 * @param[in] manifest    本次已下载清单，只读，回调返回后失效。
 * @param[in] stored_size 音频分区中已写入的有效字节数，等于清单 file_size。
 *
 * @return ESP_OK 播放接管成功；其他值仅记录日志，不改变下载结果。
 */
esp_err_t native_audio_on_ready(const native_audio_manifest_t *manifest, size_t stored_size);

#ifdef __cplusplus
}
#endif
