/**
 * @file    audio_engine.c
 * @brief   下载音频素材并在完整性确认后保存为设备可用资源。
 *
 * 素材通过 HTTPS 写入独立数据分区。每下载一段才保存一次进度，兼顾掉电恢复和
 * Flash 寿命；下载完成后从分区重新读取并计算 SHA-256，只有与清单一致才通知上层。
 *
 * 模块关系：
 * - 传输核心由 http_downloader 完成（连接、Range、ETag、读循环）；
 * - 摘要计算复用 ota_stability 的分区回读路径，与固件 OTA 同一校验语义；
 * - 不解析控制面 JSON、不依赖 MQTT 客户端句柄。
 *
 * 它与固件升级共用下载和断点机制，但不会切换启动分区或重启设备。两者分别保存
 * 恢复记录，不能互相续传；同时发生时固件升级优先，避免竞争网络和 Flash。
 *
 * 对服务器可见的过程是：已接受、下载中、校验中、可用或失败。网络失败保留安全
 * 断点；清单、长度或摘要错误会清除无效结果，不能把不完整素材交给播放功能。
 */
#include "audio_engine.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "cJSON.h"

#include "http_downloader.h"
#include "mqtt_comm.h"
#include "ota_control_plane.h"
#include "ota_stability.h"

static const char *TAG = "audio_engine";

/** 音频断点记录布局版本；改变布局时必须递增，否则旧记录判为过期从零下载。 */
#define AUDIO_ENGINE_STORE_SCHEMA_VERSION 1U

/**
 * NVS 断点检查点之间的最小写入间隔，单位为字节。
 * 权衡：间隔越小，掉电可续传的粒度越细，但 NVS 擦写次数越多、写入越频繁；
 * 间隔越大，NVS 越省、但掉电后最多丢失 <16 KiB 已下载数据需要重下。
 * 音频素材通常远小于固件（上限 1 MiB），此处与 OTA 的检查点窗口保持同一量级。
 */
#define AUDIO_ENGINE_CHECKPOINT_BYTES (16U * 1024U)

/** 断点记录中的 ETag 文本容量，包含末尾 NUL（与 http_downloader 的 ETag 容量一致）。 */
#define AUDIO_ENGINE_ETAG_SIZE 128

/** 音频 NVS 命名空间，与 ota_resume / ota_report 天然隔离，互不串读。 */
#define AUDIO_ENGINE_NVS_NAMESPACE "audio_resume"

/**
 * 并发模型：s_audio_in_progress 是"单飞"运行标志，用于在任意调用方（如 MQTT
 * 事件任务、主动检查任务）发起下载时互斥，避免同一时刻存在两个音频下载任务。
 * - 写者：audio_engine_start() 创建任务前置 true；audio_download_task() 结尾
 *   （cleanup 段）置 false。两者都持自旋锁短临界区。
 * - 读者：audio_engine_is_running()（供 audio_service 判定 OTA/音频互斥）。
 * - 锁语义：用 portMUX 自旋锁是因为可能在中断/事件上下文被读取；锁只保护这一个
 *   bool，绝不包住下载、Flash/NVS 写等耗时操作，否则会拉长中断/调度延迟。
 * - 注意：锁不是队列/信号量，不提供"等待下载完成"能力；调用方如需等待应轮询
 *   audio_engine_is_running() 或依赖后续 audio_status 事件。
 */
static bool s_audio_in_progress;
static portMUX_TYPE s_audio_state_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 可跨重启恢复的音频下载断点记录（NVS blob "record"，单值存储）。
 *
 * 语义要点：
 * - verified_offset 是"已写入并通过 NVS 检查点"的数据前缀长度，即掉电后安全
 *   可复用的前缀长度；因为是检查点粒度，它只会落后于 s->offset（内存中已写长度），
 *   绝不会超前。
 * - 恢复前提：只有"清单元数据完全一致"时才允许复用前缀；恢复时仍须对清单、URL、
 *   摘要和 HTTP Content-Range 重新校验（服务器可能返回别的内容），不能盲目信任。
 * - version 字段仅用于日志/展示，不参与 audio_record_matches() 的身份判定——
 *   因为同一 audio_id + 大小 + URL + 摘要即视为同一素材内容。
 * - etag 是"服务器确认续传对象仍是同一版本"的凭据，仅在 resume 时传入下载器。
 * - 布局必须是定长、无指针、无 padding（见下方 _Static_assert），否则跨固件版本
 *   NVS 兼容性会被破坏。
 */
typedef struct {
    uint32_t schema_version; /**< 记录布局版本，必须等于 AUDIO_ENGINE_STORE_SCHEMA_VERSION。 */
    uint32_t expected_size; /**< 清单声明的完整文件长度，单位为字节。 */
    uint32_t verified_offset; /**< 已写入并持久化检查点的前缀长度，单位为字节。 */
    uint8_t sha256[NATIVE_OTA_SHA256_SIZE]; /**< 清单中的原始 SHA-256 摘要。 */
    char audio_id[NATIVE_OTA_AUDIO_ID_SIZE]; /**< 服务端音频素材唯一 ID。 */
    char version[NATIVE_OTA_AUDIO_VERSION_SIZE]; /**< 音频素材版本字符串。 */
    char url[NATIVE_OTA_URL_SIZE]; /**< 目标音频 HTTPS URL。 */
    char etag[AUDIO_ENGINE_ETAG_SIZE]; /**< 服务器 ETag，用于确认续传内容仍是同一版本。 */
} audio_resume_record_t;

/** 固定记录布局：全部字段为定长标量/字符数组，无指针、无 padding。 */
_Static_assert(sizeof(audio_resume_record_t) ==
               4U + 4U + 4U + NATIVE_OTA_SHA256_SIZE + NATIVE_OTA_AUDIO_ID_SIZE +
               NATIVE_OTA_AUDIO_VERSION_SIZE + NATIVE_OTA_URL_SIZE + AUDIO_ENGINE_ETAG_SIZE,
               "audio_resume_record_t layout must stay fixed for NVS compatibility");

/** 音频下载任务入口；任务参数为一次服务器响应的堆上深拷贝。 */
static void audio_download_task(void *pvParameter);

/* ------------------------------------------------------------------------- */
/* NVS 辅助                                                                    */
/* ------------------------------------------------------------------------- */

/* 本组 NVS 辅助约定：
 * - 全部运行在音频下载任务（audio_download_task）上下文中，属普通任务，可阻塞，
 *   但不是中断安全；不持有 s_audio_state_lock。
 * - 每次"保存"都执行 nvs_set_* + nvs_commit：写与提交成对，保证重命名掉电会话后
 *   记录要么完整写入要么保持旧值（NVS 本身在此处不做跨页事务，因此以 commit 为
 *   持久化边界）。失败时把底层 esp_err_t 原样返回，由调用方决定是否中止下载。
 * - audio_record_load/audio_current_version_load 失败时返回非 OK，但绝不修改
 *   输出缓冲的前置状态；注意 nvs_get_str 可能已部分填充缓冲，调用方应自行 memset。 */

static esp_err_t audio_nvs_open(nvs_handle_t *handle, nvs_open_mode_t mode)
{
    return nvs_open(AUDIO_ENGINE_NVS_NAMESPACE, mode, handle);
}

/** 读取音频断点记录；无记录返回 ESP_ERR_NOT_FOUND，布局不符返回 ESP_ERR_INVALID_VERSION。 */
static esp_err_t audio_record_load(audio_resume_record_t *record)
{
    if (record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = audio_nvs_open(&handle, NVS_READONLY);
    if (err != ESP_OK) {
        return err;
    }
    size_t length = sizeof(*record);
    err = nvs_get_blob(handle, "record", record, &length);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(*record) ||
        record->schema_version != AUDIO_ENGINE_STORE_SCHEMA_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

/** 原子地保存音频断点记录。 */
static esp_err_t audio_record_save(const audio_resume_record_t *record)
{
    if (record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = audio_nvs_open(&handle, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, "record", record, sizeof(*record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/** 删除音频断点记录；记录本不存在时按成功处理。 */
static esp_err_t audio_record_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = audio_nvs_open(&handle, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, "record");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/** 保存当前已安装音频素材版本。 */
static esp_err_t audio_current_version_save(const char *version)
{
    if (version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = audio_nvs_open(&handle, NVS_READWRITE);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, "current_version", version);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/** 读取当前已安装音频素材版本；未安装返回 ESP_ERR_NOT_FOUND。 */
static esp_err_t audio_current_version_load(char *version, size_t version_size)
{
    if (version == NULL || version_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = audio_nvs_open(&handle, NVS_READONLY);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, "current_version", version, &version_size);
    nvs_close(handle);
    return err;
}

/* ------------------------------------------------------------------------- */
/* 记录初始化与匹配                                                            */
/* ------------------------------------------------------------------------- */

/** 以当前清单初始化音频断点记录（只初始化内存，不写 NVS）。 */
static void audio_record_init(audio_resume_record_t *record,
                              const native_audio_manifest_t *manifest)
{
    memset(record, 0, sizeof(*record));
    record->schema_version = AUDIO_ENGINE_STORE_SCHEMA_VERSION;
    record->expected_size = manifest->file_size;
    memcpy(record->sha256, manifest->sha256, sizeof(record->sha256));
    strncpy(record->audio_id, manifest->audio_id, sizeof(record->audio_id) - 1U);
    strncpy(record->version, manifest->version, sizeof(record->version) - 1U);
    strncpy(record->url, manifest->url, sizeof(record->url) - 1U);
}

/** 判断断点记录是否对应当前音频清单（ID、大小、URL 和摘要全部一致）。 */
static bool audio_record_matches(const audio_resume_record_t *record,
                                 const native_audio_manifest_t *manifest)
{
    return record->schema_version == AUDIO_ENGINE_STORE_SCHEMA_VERSION &&
           record->expected_size == manifest->file_size &&
           memcmp(record->sha256, manifest->sha256, sizeof(record->sha256)) == 0 &&
           strcmp(record->audio_id, manifest->audio_id) == 0 &&
           strcmp(record->url, manifest->url) == 0;
}

/* ------------------------------------------------------------------------- */
/* 状态上报                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief 构造并发布一条尽力而为的 audio_status 事件（QoS 1，不持久化）。
 *
 * @param[in] manifest        当前音频清单，不允许为 NULL。
 * @param[in] state           协议状态名（accepted/downloading/verifying/ready/failed）。
 * @param[in] progress        进度百分比 0～100。
 * @param[in] failure_reason  失败原因；无失败传 NATIVE_OTA_FAILURE_NONE。
 * @return ESP_OK 已交给 MQTT 发送队列；其他值表示 MQTT 未就绪或构造失败。
 *
 * @note 本上报是"尽力而为"：MQTT 未连接时会失败但不影响下载结果。
 *       progress 只在 ready 时有意义（100），accepted/downloading/verifying/failed
 *       一律为 0；servers 用 error_code 的数值（即 native_ota_failure_reason_t
 *       的枚举值）判断失败类别，因此该枚举不可重排、只能追加。
 */
static esp_err_t audio_report_status(const native_audio_manifest_t *manifest,
                                     const char *state, uint32_t progress,
                                     native_ota_failure_reason_t failure_reason)
{
    if (manifest == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char device_id[NATIVE_OTA_DEVICE_ID_SIZE] = { 0 };
    (void)native_ota_get_device_id(device_id, sizeof(device_id));

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool added = cJSON_AddStringToObject(root, "type", "audio_status") != NULL &&
                 cJSON_AddNumberToObject(root, "schema_version", 1) != NULL &&
                 cJSON_AddStringToObject(root, "device_id", device_id) != NULL &&
                 cJSON_AddStringToObject(root, "audio_id", manifest->audio_id) != NULL &&
                 cJSON_AddStringToObject(root, "state", state) != NULL &&
                 cJSON_AddNumberToObject(root, "progress", (double)progress) != NULL &&
                 cJSON_AddNumberToObject(root, "error_code", (double)failure_reason) != NULL;
    if (!added) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char json[512];
    bool printed = cJSON_PrintPreallocated(root, json, sizeof(json), false);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 音频状态 topic 为 <prefix>/<device_id>，与 OTA 状态 topic 同一拼装规则；
     * 通过通信层通用发布接口发送，不依赖任何 OTA 报告内部状态。 */
    char topic[192];
    int topic_len = snprintf(topic, sizeof(topic), "%s/%s",
                             CONFIG_COMM_MQTT_AUDIO_STATUS_TOPIC_PREFIX, device_id);
    if (topic_len <= 0 || (size_t)topic_len >= sizeof(topic)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return mqtt_comm_publish(topic, json, strlen(json));
}

/* ------------------------------------------------------------------------- */
/* 公共下载器 sink / restart                                                   */
/* ------------------------------------------------------------------------- */

/**
 * @brief 音频下载 sink 上下文：把下载器的网络块写入音频数据分区并保存检查点。
 *
 * 该上下文只被一个生产者（http_downloader_run，在同一音频下载任务内同步调用）
 * 使用，因此无需再加锁；它把"sink 数据 -> Flash 写入 -> 检查点"这一条链的
 * 状态（写位置、持久化水印）集中管理。
 */
typedef struct {
    const native_audio_manifest_t *manifest; /**< 当前清单，只读（用于边界/重置）。 */
    const esp_partition_t *partition; /**< 目标音频数据分区。 */
    audio_resume_record_t *record; /**< 当前断点记录，sink 更新它的 verified_offset。 */
    size_t offset; /**< 已写入分区的字节数（含断点前缀），同时是摘要校验的边界。 */
    size_t last_checkpoint; /**< 上次已持久化到 NVS 的检查点偏移，用于触发下一次写。 */
} audio_engine_sink_ctx_t;

/**
 * @brief 将下载器网络块写入音频数据分区（http_downloader sink 回调）。
 *
 * @param[in] ctx  audio_engine_sink_ctx_t*。
 * @param[in] data 本次网络块首地址，仅回调返回前有效，不能保留。
 * @param[in] len  数据长度，单位为字节。
 * @return ESP_OK 数据已写入并（必要时）保存了检查点。
 * @return 其他 esp_err_t 中止下载，错误码原样返回给 http_downloader_run()，由
 *         调用方（audio_download_task）按错误码映射失败类别。
 *
 * @note 约束/副作用：
 * - 在 http_downloader_run 的调用任务（即音频下载任务）上下文中同步执行，可写
 *   Flash 与 NVS，但不应阻塞过久（网络读循环在等下一次回调）；不能保留 data 指针。
 * - 即使服务端用 chunked 编码没有 Content-Length，也绝不让任何一块数据把实际
 *   写入长度推过清单声明的 file_size，防止恶意/损坏响应破坏分区；上游还据此把
 *   ESP_ERR_INVALID_SIZE 分类为校验/大小失败。
 * - 每跨过 AUDIO_ENGINE_CHECKPOINT_BYTES 才写一次 NVS 检查点（先写记录再 commit），
 *   失败时中止下载；此时内存 offset 已前进但 NVS 水印仍停留在上次检查点，掉电后
 *   最多丢一个检查点窗口的数据。
 */
static esp_err_t audio_engine_sink(void *ctx, const uint8_t *data, size_t len)
{
    audio_engine_sink_ctx_t *s = (audio_engine_sink_ctx_t *)ctx;
    if (s == NULL || data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 即使服务端采用 chunked 编码而未提供 Content-Length，也绝不能让
     * 单个异常响应写过清单声明的素材边界。 */
    if (s->manifest == NULL || s->offset > s->manifest->file_size ||
        len > (size_t)s->manifest->file_size - s->offset) {
        ESP_LOGE(TAG, "Audio response exceeds manifest size");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_partition_write(s->partition, s->offset, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write audio partition: %s", esp_err_to_name(err));
        return err;
    }
    s->offset += len;
    if (s->offset - s->last_checkpoint >= AUDIO_ENGINE_CHECKPOINT_BYTES) {
        s->record->verified_offset = (uint32_t)s->offset;
        err = audio_record_save(s->record);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist audio checkpoint: %s", esp_err_to_name(err));
            return err;
        }
        s->last_checkpoint = s->offset;
    }
    return ESP_OK;
}

/**
 * @brief http_downloader 断点重置回调：重建断点记录并清零写入偏移。
 *
 * 触发条件：服务器忽略 Range（返回 200/416，即不支持续传）或响应 ETag 变化，
 * 下载器决定放弃已有前缀、从零全量重传。本回调负责把"持久化断点"与"内存写位置"
 * 一起归零，并把当前清单重新落盘为一条全新的初始断点记录。
 *
 * @param[in] ctx audio_engine_sink_ctx_t*。
 * @return ESP_OK 允许从零开始全量下载。
 * @return 其他 esp_err_t 中止下载，错误码原样透传给 http_downloader_run()。
 *
 * @note 回调时下载器尚未读取新连接的 body，可安全重置写目标；NVS 写失败时返回
 *       非 OK 会中止本次下载（此时断点已不可信，宁可失败也不半途续传）。
 */
static esp_err_t audio_engine_restart(void *ctx)
{
    audio_engine_sink_ctx_t *s = (audio_engine_sink_ctx_t *)ctx;
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s->offset = 0;
    s->last_checkpoint = 0;
    audio_record_init(s->record, s->manifest);
    return audio_record_save(s->record);
}

/* ------------------------------------------------------------------------- */
/* 下载任务                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief 执行带断点续传、分区写入和摘要校验的音频下载任务。
 *
 * @param[in] pvParameter 指向堆上 native_audio_manifest_t；任务取得所有权后
 *                        立即释放，不能为 NULL。
 *
 * 任务先复用与清单匹配的断点记录，再经公共下载器下载；Range、HTTP 状态、
 * Content-Length、Content-Range 与完整接收校验由下载器完成，分区写入与
 * 检查点由 sink 完成。摘要校验失败或分区不可用时清除断点记录；网络类失败
 * 保留断点供下次续传。
 *
 * @note 本函数是 FreeRTOS 任务入口：运行在"audio_task"（prio=5、栈 12288 B），
 *       全程阻塞直到下载结束或失败。它独占拥有 pvParameter（堆上深拷贝），
 *       进入后立即释放；任务结束前必须由本函数自己把 s_audio_in_progress 清回
 *       false 并 vTaskDelete(NULL)，因此任何提前 return 都会破坏单飞标志。
 */
static void audio_download_task(void *pvParameter)
{
    /* 深拷贝到局部变量后立即释放入参：清单的所有权已从调用方转移给本任务，
     * 此后 manifest 与下载循环解耦，不再依赖调用方缓冲区的生命周期。 */
    native_audio_manifest_t manifest = *(native_audio_manifest_t *)pvParameter;
    free(pvParameter);

    esp_err_t err = ESP_OK;
    native_ota_failure_reason_t failure_reason = NATIVE_OTA_FAILURE_NONE;
    /* 网络类失败保留断点记录；校验类失败清除，下次从零下载。 */
    bool keep_record = false;
    size_t resume_offset = 0;
    bool resume = false;
    const esp_partition_t *partition = NULL;
    audio_resume_record_t record;
    bool record_active = false;
    audio_engine_sink_ctx_t sink_ctx = { 0 };

    ESP_LOGI(TAG, "Starting audio download task: audio_id=%s, version=%s, size=%" PRIu32,
             manifest.audio_id, manifest.version, manifest.file_size);

    (void)audio_report_status(&manifest, "accepted", 0, NATIVE_OTA_FAILURE_NONE);

    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                         CONFIG_AUDIO_STORAGE_PARTITION);
    if (partition == NULL) {
        ESP_LOGE(TAG, "Audio storage partition '%s' not found", CONFIG_AUDIO_STORAGE_PARTITION);
        failure_reason = NATIVE_OTA_FAILURE_STORAGE_UNAVAILABLE;
        goto cleanup;
    }
    /* 音频素材必须能完整放进目标数据分区：清单 file_size 是校验过的正数，
     * 且已被控制面限制在 CONFIG_AUDIO_MAX_FILE_SIZE（默认 1 MiB，与 audio_data
     * 分区等大小）。此处再与分区实际容量比对，防御"清单大小 > 分区"导致越界写。 */
    if (manifest.file_size > partition->size) {
        ESP_LOGE(TAG, "Audio file_size=%" PRIu32 " exceeds partition size=%" PRIu32,
                 manifest.file_size, partition->size);
        failure_reason = NATIVE_OTA_FAILURE_IMAGE_TOO_LARGE;
        goto cleanup;
    }

    /* 只有清单元数据完全一致时才允许复用 Flash 前缀和 HTTP Range 检查点。 */
    err = audio_record_load(&record);
    if (err == ESP_OK) {
        /* 还要满足"已有前缀"且"严格小于完整长度"：前缀 0 表示没下载过、
         * 等于 file_size 表示其实已下完（应直接走校验，而非再发起 Range）。 */
        if (audio_record_matches(&record, &manifest) &&
            record.verified_offset > 0U &&
            record.verified_offset < manifest.file_size) {
            record_active = true;
            /* normalize 会把 offset 向下对齐到 Flash 加密安全写入边界；若启用加密，
             * 对齐可能丢弃尚未持久化的尾部数据，因此续传以它的返回值为准。 */
            resume_offset = ota_stability_normalize_resume_offset(record.verified_offset);
            resume = resume_offset > 0U;
            ESP_LOGI(TAG, "Resuming audio artifact %s from offset=%zu",
                     manifest.audio_id, resume_offset);
        } else {
            ESP_LOGI(TAG, "Audio resume record belongs to another artifact; starting from zero");
            (void)audio_record_clear();
        }
    } else if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_VERSION) {
        ESP_LOGE(TAG, "Failed to load audio resume record: %s", esp_err_to_name(err));
        failure_reason = NATIVE_OTA_FAILURE_NVS_WRITE_FAILED;
        goto cleanup;
    }

    if (!record_active) {
        /* 首次下载（无断点记录）或旧记录不属于本清单：重建初始断点并立即持久化，
         * 使后续每次写入都有可依赖的 NVS 水印，掉电才能续传。 */
        audio_record_init(&record, &manifest);
        err = audio_record_save(&record);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot persist initial audio resume record: %s", esp_err_to_name(err));
            failure_reason = NATIVE_OTA_FAILURE_NVS_WRITE_FAILED;
            goto cleanup;
        }
        record_active = true;
    }

    sink_ctx.manifest = &manifest;
    sink_ctx.partition = partition;
    sink_ctx.record = &record;
    sink_ctx.offset = resume_offset;
    sink_ctx.last_checkpoint = resume_offset;

    /* 下载器配置：expected_size / resume_offset / expected_etag 从断点恢复而来；
     * cert_pem=NULL 表示用构建嵌入的 CA；sink 与 restart 回调把"写分区 + 检查点"
     * 注入传输核心。传输层失败的分类完全交给下载器（dl_result.failure_reason）。 */
    (void)audio_report_status(&manifest, "downloading", 0, NATIVE_OTA_FAILURE_NONE);

    http_downloader_config_t dl_config = {
        .url = manifest.url,
        .cert_pem = NULL, /* NULL 使用构建嵌入的 ca_cert.pem。 */
        .timeout_ms = CONFIG_EXAMPLE_OTA_RECV_TIMEOUT,
#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
        .skip_cert_common_name_check = true,
#endif
        .expected_size = manifest.file_size,
        .resume_offset = resume ? resume_offset : 0,
        .expected_etag = resume ? record.etag : NULL,
        .sink = audio_engine_sink,
        .sink_ctx = &sink_ctx,
        .restart_cb = audio_engine_restart,
        .restart_ctx = &sink_ctx,
    };
    http_downloader_result_t dl_result;
    err = http_downloader_run(&dl_config, &dl_result);

    /* 传输层失败直接使用下载器分类；sink 返回的业务错误按错误码映射。 */
    if (err != ESP_OK) {
        if (dl_result.failure_reason != NATIVE_OTA_FAILURE_NONE) {
            failure_reason = dl_result.failure_reason;
        } else if (err >= ESP_ERR_NVS_BASE && err < ESP_ERR_NVS_BASE + 0x10) {
            failure_reason = NATIVE_OTA_FAILURE_NVS_WRITE_FAILED;
        } else {
            failure_reason = NATIVE_OTA_FAILURE_IMAGE_VALIDATE_FAILED;
        }
        /* 只有"可重试的瞬态"传输失败才保留断点记录供下次续传（网络/ TLS / HTTP 状态）；
         * 其余（NVS 写失败、存储/校验类）属于环境或资源问题，重试往往无意义，清除
         * 断点以便下次从零开始，避免拿着不可信的断点反复失败。 */
        keep_record = (failure_reason == NATIVE_OTA_FAILURE_NETWORK_TIMEOUT ||
                       failure_reason == NATIVE_OTA_FAILURE_TLS_VERIFY_FAILED ||
                       failure_reason == NATIVE_OTA_FAILURE_HTTP_STATUS_INVALID);
        goto cleanup;
    }

    /* 首次获得服务器 ETag 时立即持久化，供下次断点续传确认对象身份。 */
    if (dl_result.etag[0] != '\0' && record.etag[0] == '\0') {
        strncpy(record.etag, dl_result.etag, sizeof(record.etag) - 1U);
        record.etag[sizeof(record.etag) - 1U] = '\0';
        err = audio_record_save(&record);
        if (err != ESP_OK) {
            failure_reason = NATIVE_OTA_FAILURE_NVS_WRITE_FAILED;
            goto cleanup;
        }
    }

    /* body 完整、长度匹配是摘要校验之前的必要条件：即便传输层返回 OK，
     * 也可能因 chunked/截断导致实际写入长度与清单不符。这里把"长度不符"
     * 与"体不完整"区分开——后者是网络超时（可续传），前者是内容异常（清断点）。 */
    if (!dl_result.complete || sink_ctx.offset != manifest.file_size) {
        ESP_LOGE(TAG, "Received audio is incomplete: got=%zu expected=%" PRIu32,
                 sink_ctx.offset, manifest.file_size);
        failure_reason = dl_result.complete ?
                         NATIVE_OTA_FAILURE_IMAGE_VALIDATE_FAILED :
                         NATIVE_OTA_FAILURE_NETWORK_TIMEOUT;
        keep_record = !dl_result.complete;
        goto cleanup;
    }

    (void)audio_report_status(&manifest, "verifying", 0, NATIVE_OTA_FAILURE_NONE);

    /* 摘要校验：以"分区里真实写入的 [0,file_size) 前缀"重算 SHA-256，与清单对比。
     * 这是完整性/防篡改的最后一道保障；校验失败即认定素材不可信（HASH_MISMATCH）。
     * 范围严格限定为 file_size，避免把分区剩余空间混入摘要。 */
    uint8_t downloaded_sha256[NATIVE_OTA_SHA256_SIZE];
    err = ota_stability_calculate_partition_sha256(partition, manifest.file_size,
                                                   downloaded_sha256);
    if (err != ESP_OK) {
        failure_reason = NATIVE_OTA_FAILURE_IMAGE_VALIDATE_FAILED;
        goto cleanup;
    }
    if (memcmp(downloaded_sha256, manifest.sha256, sizeof(downloaded_sha256)) != 0) {
        ESP_LOGE(TAG, "Downloaded audio SHA-256 does not match the audio response");
        failure_reason = NATIVE_OTA_FAILURE_HASH_MISMATCH;
        goto cleanup;
    }

    /* 摘要校验通过：先把"当前已安装版本"写入 NVS（成败只记日志，不阻止交付），
     * 再清除断点（此时素材已完整且可信，不再需要续传记录），随后通知上层播放。
     * 若版本持久化失败仍继续：素材本身可用，仅版本上报可能为旧值。 */
    err = audio_current_version_save(manifest.version);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist current audio version: %s", esp_err_to_name(err));
    }
    (void)audio_record_clear();
    record_active = false;

    (void)audio_report_status(&manifest, "ready", 100, NATIVE_OTA_FAILURE_NONE);
    /* 弱钩子：默认实现只记日志；产品板级代码可提供强符号覆盖以播放分区内容。
     * 这里不拿返回值中断下载流程——播放失败不影响"下载已完成"这一事实。 */
    esp_err_t play_err = native_audio_on_ready(&manifest, sink_ctx.offset);
    if (play_err != ESP_OK) {
        ESP_LOGW(TAG, "Audio playback handoff reported: %s", esp_err_to_name(play_err));
    }
    failure_reason = NATIVE_OTA_FAILURE_NONE;

cleanup:
    /* 统一退出路径：失败时上报（必要时按 keep_record 决定是否清除断点），
     * 无论成功/失败都必须清回单飞标志后自删任务，否则 audio_engine_start 会
     * 永久拒绝后续下载。 */
    if (failure_reason != NATIVE_OTA_FAILURE_NONE) {
        ESP_LOGE(TAG, "Audio task finished with reason=%s",
                 native_ota_failure_reason_name(failure_reason));
        (void)audio_report_status(&manifest, "failed", 0, failure_reason);
        if (record_active && !keep_record) {
            (void)audio_record_clear();
        }
    }
    portENTER_CRITICAL(&s_audio_state_lock);
    s_audio_in_progress = false;
    portEXIT_CRITICAL(&s_audio_state_lock);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* 公共接口                                                                    */
/* ------------------------------------------------------------------------- */

/* 说明：头文件 audio_engine.h 已给出完整契约；此处的实现要点是——
 * 任何时候至多存在一个音频下载任务（单飞），由 s_audio_in_progress + 自旋锁保证：
 * 置位与创建任务在临界区内是原子的，且任务创建失败必须回滚置位，否则会永久占用
 * 单飞名额。任务栈 12288 B、优先级 5（与 OTA 下载任务同量级）。 */
esp_err_t audio_engine_start(const native_audio_manifest_t *manifest)
{
    if (manifest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 深拷贝清单到堆对象作为任务参数：调用方可用任意生命周期缓冲传入，
     * 任务取得所有权后自行释放，双方无指针共享。 */
    native_audio_manifest_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *request = *manifest;

    /* 单飞检查 + 置位必须同一临界区内完成，避免两个调用方同时通过检查。 */
    portENTER_CRITICAL(&s_audio_state_lock);
    if (s_audio_in_progress) {
        portEXIT_CRITICAL(&s_audio_state_lock);
        free(request);
        return ESP_ERR_INVALID_STATE;
    }
    s_audio_in_progress = true;
    portEXIT_CRITICAL(&s_audio_state_lock);

    ESP_LOGI(TAG, "Creating audio task: audio_id=%s, version=%s, size=%" PRIu32,
             request->audio_id, request->version, request->file_size);
    if (xTaskCreate(audio_download_task, "audio_task", 12288, request, 5, NULL) != pdPASS) {
        /* 任务创建失败：必须回滚置位。否则下次调用会被 s_audio_in_progress 挡住。 */
        portENTER_CRITICAL(&s_audio_state_lock);
        s_audio_in_progress = false;
        portEXIT_CRITICAL(&s_audio_state_lock);
        free(request);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool audio_engine_is_running(void)
{
    portENTER_CRITICAL(&s_audio_state_lock);
    bool running = s_audio_in_progress;
    portEXIT_CRITICAL(&s_audio_state_lock);
    return running;
}

esp_err_t audio_engine_get_current_version(char *version, size_t version_size)
{
    if (version == NULL || version_size < NATIVE_OTA_AUDIO_VERSION_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = audio_current_version_load(version, version_size);
    if (err != ESP_OK) {
        strncpy(version, NATIVE_OTA_AUDIO_VERSION_UNKNOWN, version_size - 1U);
        version[version_size - 1U] = '\0';
    }
    return ESP_OK;
}

/* 弱符号默认钩子：产品板级代码可提供同名强符号覆盖以接管播放（例如把
 * audio_data 分区读出的 stored_size 字节交给扬声器）。定义在 .c 而非 .h，
 * 使"无强符号时全局只此一份弱定义"且默认不报未定义引用。 */
__attribute__((weak)) esp_err_t native_audio_on_ready(const native_audio_manifest_t *manifest,
                                                      size_t stored_size)
{
    ESP_LOGI(TAG, "Audio artifact %s ready: %zu bytes stored (default hook, no player attached)",
             manifest != NULL ? manifest->audio_id : "?", stored_size);
    return ESP_OK;
}
