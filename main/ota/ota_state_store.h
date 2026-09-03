/**
 * @file    ota_state_store.h
 * @brief   保存升级下载进度，并记住哪些损坏制品不得再次自动尝试。
 *
 * 设备每写入一段固件才保存一次检查点，掉电后最多重下一小段，同时避免每个网络包
 * 都擦写 Flash。确定损坏的制品会被隔离；单纯网络失败只降低重试频率。
 *
 * 本文件保存的是设备重启后如何继续下载或提交，不是服务器看到的升级进度；
 * 两套状态用途不同，不能互相转换或混用。
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

/** 当前状态结构的版本；改变布局时必须递增。 */
#define OTA_STATE_STORE_SCHEMA_VERSION 2U

/** NVS 状态检查点之间的最小写入间隔，单位为字节。 */
#define OTA_STATE_STORE_CHECKPOINT_BYTES (16U * 1024U)

/** 下载恢复最多允许的网络类失败次数；达到后由 OTA 任务隔离 artifact。 */
#define OTA_STATE_STORE_NETWORK_RETRY_THRESHOLD CONFIG_OTA_DOWNLOAD_RETRY_THRESHOLD

/** 输出默认 NVS 分区水位，并关联最近一次存储操作结果。 */
void ota_state_store_log_nvs_usage(const char *tag, const char *operation,
                                   esp_err_t operation_error);

/** OTA 恢复记录的阶段。 */
typedef enum {
    OTA_RESUME_PHASE_EMPTY = 0, /**< 未建立有效恢复记录，仅作为保留值。 */
    OTA_RESUME_PHASE_DOWNLOADING = 1, /**< 已写入部分镜像，可按检查点尝试续传。 */
    OTA_RESUME_PHASE_READY_TO_COMMIT = 2, /**< 镜像已完整校验，等待切换启动分区。 */
    OTA_RESUME_PHASE_QUARANTINED = 3, /**< 终端校验失败，禁止同一 artifact 自动重试。 */
} ota_resume_phase_t;

/** 表示网络连续失败后的临时等待；等待结束后仍允许重试，不等于制品被隔离。 */
#define OTA_RESUME_PHASE_COOLING_DOWN ((ota_resume_phase_t)4)

/*
 * 断点阶段（phase）合法迁移表。
 *
 * 持久化值：EMPTY=0（保留）、DOWNLOADING=1、READY_TO_COMMIT=2、
 *           QUARANTINED=3、COOLING_DOWN=4。
 *
 * 分支由 ota_engine.c 的 ota_engine_task 驱动，写入方是它，读取方是它自己在
 * 下一次启动/重试时，以及 ota_boot_flow.c 在启动验收后的对账清理。
 *
 *   无记录 / EMPTY ──(record_init: 下载开始)──▶ DOWNLOADING
 *   DOWNLOADING ──(下载+镜像校验完成, phase=READY_TO_COMMIT)──▶ READY_TO_COMMIT
 *   DOWNLOADING ──(终端校验失败, quarantine_record)──▶ QUARANTINED
 *   DOWNLOADING ──(网络失败 retry_count 达到阈值)──▶ COOLING_DOWN
 *   COOLING_DOWN ──(冷却延时结束, 重置 retry_count)──▶ DOWNLOADING
 *   DOWNLOADING/READY_TO_COMMIT ──(记录与当前清单或目标分区不一致, ota_state_store_clear)──▶ 删除(回 EMPTY)
 *   READY_TO_COMMIT ──(当前版本启动成功, reconcile)──▶ 记录被删除(回 EMPTY)
 *   READY_TO_COMMIT ──(提交前掉电, 下轮核对分区摘要后直接提交或重下)──▶ 保持
 *   QUARANTINED ──(终端错误, 禁止自动重试)──▶ 保持不变
 *
 * 关键要求：网络失败允许稍后继续，镜像校验失败则禁止同一制品自动重试；完整校验
 * 后掉电应直接恢复提交，不能重新下载。删除记录表示没有可安全继续的任务，需要从零开始。
 */

/**
 * @brief 设备重启后继续同一升级所需的最小记录。
 *
 * 只把已经写入且保存检查点的前缀视为可靠。恢复时必须再次核对清单、下载地址、
 * 摘要、目标分区和服务器返回范围，避免把另一个制品接在旧数据后面。
 */
typedef struct {
    uint32_t schema_version; /**< 记录布局版本，必须等于 OTA_STATE_STORE_SCHEMA_VERSION。 */
    uint32_t expected_size; /**< 清单声明的完整镜像长度，单位为字节。 */
    uint32_t verified_offset; /**< 已写入并持久化检查点的镜像前缀长度，单位为字节。 */
    uint32_t retry_count; /**< 网络类失败后的重试次数，不包含终端校验失败。 */
    uint8_t sha256[NATIVE_OTA_SHA256_SIZE]; /**< 清单中的原始 SHA-256 摘要。 */
    char artifact_id[NATIVE_OTA_ARTIFACT_ID_SIZE]; /**< 服务端发布物唯一 ID。 */
    char version[sizeof(((esp_app_desc_t *)0)->version)]; /**< 目标应用版本字符串。 */
    char url[NATIVE_OTA_URL_SIZE]; /**< 目标固件 HTTPS URL。 */
    char etag[128]; /**< 服务器 ETag，用于确认续传内容仍是同一版本。 */
    uint8_t target_partition_subtype; /**< 目标 OTA 分区 subtype，防止恢复到错误槽位。 */
    uint8_t phase; /**< ota_resume_phase_t 的持久化值（见上方合法迁移表）。
                    *   记录校验允许的范围是 DOWNLOADING～COOLING_DOWN，含冷却、隔离阶段。 */
    uint16_t reserved; /**< 保留字段，用于维持布局对齐和后续扩展空间，当前不参与恢复匹配。 */
    uint32_t failure_reason; /**< 隔离时保存的 native_ota_failure_reason_t 数值；未失败时为 0。 */
    uint32_t cooldown_count; /**< 冷却已进入过的次数，用于指数退避 cooldown 延时；UINT32_MAX 表示不再增长。 */
} ota_resume_record_t;

/**
 * @brief 读取 OTA 恢复记录。
 *
 * @param[out] record 输出记录，不允许为 NULL。
 * @return ESP_OK 读取到有效记录。
 * @return ESP_ERR_NOT_FOUND 没有记录。
 * @return 其他 esp_err_t NVS 读取或 schema 校验失败。
 *
 * @note 失败时会清零输出结构体；必须在 nvs_flash_init() 成功后、普通任务上下文调用。
 */
esp_err_t ota_state_store_load(ota_resume_record_t *record);

/**
 * @brief 原子地保存一条 OTA 恢复记录。
 *
 * @param[in] record 待保存记录，不允许为 NULL。
 * @return ESP_OK 保存并提交成功。
 * @return 其他 esp_err_t NVS 写入或提交失败。
 *
 * @note 调用会执行 nvs_commit() 并写 Flash；不能在中断上下文调用。
 */
esp_err_t ota_state_store_save(const ota_resume_record_t *record);

/**
 * @brief 删除 OTA 恢复记录。
 *
 * @return ESP_OK 删除成功或记录本来不存在。
 * @return 其他 esp_err_t NVS 操作失败。
 *
 * @note 删除会提交 NVS；它只清理 ota_resume/record，不影响 ota_report 状态。
 */
esp_err_t ota_state_store_clear(void);

/**
 * @brief 判断记录是否对应同一个 artifact。
 *
 * @param[in] record 已加载记录，不允许为 NULL。
 * @param[in] manifest 当前服务器清单，不允许为 NULL。
 * @return true artifact_id、版本、大小、URL 和 SHA-256 全部一致。
 * @return false 任一字段不同或参数无效。
 *
 * @note 不比较 job_id、安全版本或过期时间；这些字段不参与断点对象身份判定。
 */
bool ota_state_store_matches_manifest(const ota_resume_record_t *record,
                                      const native_ota_manifest_t *manifest);

/**
 * @brief 用当前运行版本清理已经提交并成功启动的旧记录。
 *
 * @param[in] running_version 当前运行镜像版本，不允许为 NULL。
 * @return ESP_OK 清理完成或无需清理。
 * @return 其他 esp_err_t NVS 读取/删除失败。
 *
 * @note 只删除 phase 为 READY_TO_COMMIT 且版本等于当前运行版本的记录，避免误删
 *       其他 artifact 的下载断点或隔离记录。
 */
esp_err_t ota_state_store_reconcile_running_version(const char *running_version);

#ifdef __cplusplus
}
#endif
