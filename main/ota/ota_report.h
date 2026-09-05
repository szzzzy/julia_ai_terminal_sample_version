/**
 * @file    ota_report.h
 * @brief   向服务器报告升级进度，并保证成功、失败等关键结果在断线或掉电后仍可补发。
 *
 * 下载任务只说明设备已经到达哪个阶段，不直接操作 MQTT。通信模块负责发送；
 * 这样升级逻辑在网络断开时仍能先保存结果，等连接恢复后继续报告。
 *
 * 成功、失败、回滚和准备重启等关键结果先写入持久存储，Broker 确认收到后才删除；
 * 下载百分比只保留最新值，允许在断线时丢失，避免频繁写 Flash 缩短存储寿命。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ota_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 128-bit event_id 的小写十六进制文本长度，包含末尾 NUL。 */
#define NATIVE_OTA_EVENT_ID_SIZE 33

/** OTA 状态名称最大长度，包含末尾 NUL。 */
#define NATIVE_OTA_REPORT_STATE_SIZE 32

/** 单条状态 JSON 的固定容量，包含末尾 NUL。 */
#define NATIVE_OTA_STATUS_JSON_SIZE 1024

/** 固定为 8 条的关键事件持久化队列深度；普通进度不写入该队列。 */
#define NATIVE_OTA_REPORT_PENDING_MAX 8

/**
 * @brief 服务器能够观察到的固件升级阶段。
 *
 * 这些值表示设备已经完成或正在执行的业务步骤，不是设备内部保存下载断点的格式。
 */
typedef enum {
    NATIVE_OTA_REPORT_ACCEPTED = 0, /**< 清单已校验并已创建 OTA 任务。 */
    NATIVE_OTA_REPORT_DOWNLOADING, /**< 已开始下载或正在上报下载进度。 */
    NATIVE_OTA_REPORT_VERIFYING, /**< 下载完成，正在校验镜像和摘要。 */
    NATIVE_OTA_REPORT_REBOOTING, /**< 已设置下次启动分区，设备即将重启。 */
    NATIVE_OTA_REPORT_BOOTED_PENDING_VERIFY, /**< 新镜像已启动，仍等待本地验收。 */
    NATIVE_OTA_REPORT_SUCCEEDED, /**< 本地健康检查通过且镜像已确认有效。 */
    NATIVE_OTA_REPORT_FAILED, /**< 本次 OTA 因终端错误失败。 */
    NATIVE_OTA_REPORT_ROLLED_BACK, /**< 新镜像无效，设备已回滚或识别到回滚。 */
    NATIVE_OTA_REPORT_DEFERRED, /**< 提交前条件暂不满足，保留记录等待后续处理。 */
} native_ota_report_state_t;

/**
 * @brief 一次升级在重启前后保持不变的身份信息。
 *
 * 只保存服务器用于识别任务、设备和版本的字段，不保存下载地址、证书或固件内容。
 */
typedef struct {
    char job_id[NATIVE_OTA_JOB_ID_SIZE]; /**< 可选云端任务 ID；未提供时为空串。 */
    char request_id[NATIVE_OTA_REQUEST_ID_SIZE]; /**< 触发本次升级的检查请求 ID。 */
    char artifact_id[NATIVE_OTA_ARTIFACT_ID_SIZE]; /**< 固件发布物唯一 ID。 */
    char product[NATIVE_OTA_PRODUCT_ID_SIZE]; /**< 本地配置确认过的产品标识。 */
    char hardware_version[NATIVE_OTA_HARDWARE_VERSION_SIZE]; /**< 本地配置确认过的硬件版本。 */
    char current_version[sizeof(((esp_app_desc_t *)0)->version)]; /**< 重启前运行的版本。 */
    char target_version[sizeof(((esp_app_desc_t *)0)->version)]; /**< 待安装镜像版本。 */
    uint32_t image_size; /**< 清单声明的镜像大小，单位为字节。 */
    uint32_t attempt; /**< 本 artifact 的下载尝试序号，从 1 开始。 */
} native_ota_report_context_t;

/**
 * @brief 一条已经准备好交给 MQTT 发送的升级状态消息。
 *
 * 发送方如需异步处理，必须在返回前复制内容；函数返回后原消息可能立即失效。
 */
typedef struct {
    bool critical; /**< true 表示必须持久化并等待 QoS 1 PUBACK。 */
    native_ota_report_state_t state; /**< JSON 中对应的 OTA 生命周期状态。 */
    char event_id[NATIVE_OTA_EVENT_ID_SIZE]; /**< 本条事件的幂等关联 ID。 */
    size_t json_len; /**< JSON 有效长度，不包含末尾 NUL，单位为字节。 */
    char json[NATIVE_OTA_STATUS_JSON_SIZE]; /**< 已序列化的 NUL 结尾状态 JSON。 */
} native_ota_report_message_t;

/**
 * @brief 把一条升级状态交给通信模块的快速提交函数。
 *
 * 本函数不能等待真正发送完成。暂时提交失败不会丢失关键结果，报告模块仍会保留，
 * 等通信恢复后再次尝试。
 */
typedef esp_err_t (*native_ota_report_transport_t)(
    const native_ota_report_message_t *message, void *context);

/**
 * @brief 读取尚未确认的升级结果，并启动后台确认处理。
 *
 * @return ESP_OK 已初始化或此前已初始化。
 * @return ESP_ERR_NO_MEM FreeRTOS 同步对象或确认任务创建失败。
 * @return 其他 esp_err_t 读取 NVS 状态失败；该错误不会自动擦除已有状态。
 *
 * @note 必须在 nvs_flash_init() 成功后、注册 transport 前调用；不能在中断中调用。
 */
esp_err_t native_ota_report_init(void);

/**
 * @brief 登记实际发送升级状态的通信函数，或在 MQTT 不可用时暂时清除。
 *
 * @param[in] transport 非阻塞事件提交回调；传入 NULL 暂时禁用发送。
 * @param[in] context 原样传给 transport 的上下文指针，可为 NULL。
 * @return ESP_OK 注册成功。
 * @return ESP_ERR_INVALID_STATE 报告模块尚未初始化。
 * @return ESP_ERR_TIMEOUT 在规定时间内无法取得内部锁。
 *
 * @note 暂停发送不会删除任何尚未确认的关键结果。
 */
esp_err_t native_ota_report_set_transport(native_ota_report_transport_t transport,
                                           void *context);

/**
 * @brief 从已校验清单中提取服务器识别本次升级所需的固定信息。
 *
 * @param[out] context 输出一次 OTA 任务的稳定关联上下文，不允许为 NULL。
 * @param[in] manifest 已通过控制面校验的清单，不允许为 NULL。
 * @param[in] current_version 当前运行镜像版本字符串，不允许为空。
 * @return ESP_OK 上下文复制成功。
 * @return ESP_ERR_INVALID_ARG 参数缺失或清单关键字段为空。
 *
 * @note 只写入调用者内存，不访问 NVS，也不创建任务。
 */
esp_err_t native_ota_report_context_init(native_ota_report_context_t *context,
                                          const native_ota_manifest_t *manifest,
                                          const char *current_version);

/**
 * @brief 返回写入协议 JSON 的稳定状态名称。
 *
 * @param[in] state 生命周期状态枚举值。
 * @return 静态只读字符串；未知值返回 `unknown`。
 */
const char *native_ota_report_state_name(native_ota_report_state_t state);

/**
 * @brief 报告设备已经进入一个新的升级阶段。
 *
 * 关键阶段先保存再发送；暂时无法上报不会改变下载和校验本身的结果。
 */
esp_err_t native_ota_report_event(const native_ota_report_context_t *context,
                                  native_ota_report_state_t state,
                                  uint32_t bytes_downloaded,
                                  native_ota_failure_reason_t failure_reason);

/**
 * @brief 在进度变化足够大或间隔足够久时报告最新下载进度。
 *
 * 进度只保存在内存中；断线时允许跳过中间值，恢复后报告最新值。
 */
esp_err_t native_ota_report_progress(const native_ota_report_context_t *context,
                                     uint32_t bytes_downloaded);

/**
 * @brief MQTT 恢复后，重新发送尚未确认的关键结果和最新进度。
 *
 * @return ESP_OK 全部当前可发送内容已交给 transport，或没有待发送内容。
 * @return ESP_ERR_NOT_SUPPORTED 尚未注册 transport。
 * @return 其他 esp_err_t transport 或内部锁操作失败。
 *
 * @note 本函数不会执行 NVS 写入；通常由 MQTT 连接/订阅就绪路径调用。
 */
esp_err_t native_ota_report_flush_pending(void);
/** 定期核对只重投未确认的关键事件，不重放缓存的进度。 */
esp_err_t native_ota_report_retry_pending(void);

/**
 * @brief 记录 Broker 已确认收到哪一条升级结果。
 *
 * @param[in] event_id 已确认事件的 NUL 结尾 ID，长度必须小于
 *                     NATIVE_OTA_EVENT_ID_SIZE。
 *
 * @note 这里只登记确认，后台再删除持久记录；登记失败时保留记录并允许以后重发。
 */
void native_ota_report_ack_event(const char *event_id);

/**
 * @brief 新固件首次启动、尚未完成本地健康检查时报告“等待确认”。
 *
 * @return ESP_OK 已生成事件，或当前没有跨重启待验收上下文。
 * @return 其他 esp_err_t 报告构建、持久化或 transport 操作失败。
 *
 * @note 必须在 NVS 初始化和报告模块初始化完成后、网络启动前调用。
 */
esp_err_t native_ota_report_boot_pending_verify(void);

/**
 * @brief 新固件通过本地健康检查并正式确认后报告成功。
 *
 * @return ESP_OK 已生成事件，或当前没有跨重启待验收上下文。
 * @return 其他 esp_err_t 报告构建、持久化或 transport 操作失败。
 */
esp_err_t native_ota_report_boot_succeeded(void);

/**
 * @brief 新固件未通过检查并恢复旧版本时报告回滚。
 *
 * @param[in] failure_reason 回滚原因；会序列化为稳定的 error_code 字段。
 * @return ESP_OK 已生成事件，或当前没有跨重启待验收上下文。
 * @return 其他 esp_err_t 报告构建、持久化或 transport 操作失败。
 */
esp_err_t native_ota_report_boot_rolled_back(native_ota_failure_reason_t failure_reason);

/**
 * @brief 普通启动时核对 bootloader 是否已经替设备完成了一次回滚。
 *
 * last_invalid_version 应来自 esp_ota_get_last_invalid_partition()；只有它与持久化
 * 目标版本匹配且当前运行版本不同，才会生成 rolled_back。
 *
 * @param[in] running_version 当前实际运行镜像版本，不允许为 NULL。
 * @param[in] last_invalid_version bootloader 最近判定无效的镜像版本，不允许为 NULL。
 * @return ESP_OK 已完成对账、没有匹配的回滚上下文，或已生成回滚事件。
 * @return 其他 esp_err_t 内部锁或报告持久化/提交失败。
 */
esp_err_t native_ota_report_reconcile_rollback(const char *running_version,
                                                const char *last_invalid_version);

#ifdef __cplusplus
}
#endif
