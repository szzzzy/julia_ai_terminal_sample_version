/**
 * @file julia_fault.c
 * @brief 严重故障快照的 NVS 实现。
 *
 * 每次进入 S7 只覆盖写一条定长记录，避免把运行日志持续写入 Flash。
 * NVS 本身不可用时函数只返回错误，调用方仍可继续故障呈现和复位。
 */
#include "julia_fault.h"

#include <stddef.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#define FAULT_SCHEMA_VERSION 1U
#define FAULT_NAMESPACE      "julia_fault"
#define FAULT_RECORD_KEY     "last"

static const char *TAG = "JULIA_FAULT";

const char *julia_fault_reason_name(julia_fault_reason_t reason)
{
    switch (reason) {
    case JULIA_FAULT_CRITICAL_INIT: return "CRITICAL_INIT";
    case JULIA_FAULT_DISPLAY_INIT: return "DISPLAY_INIT";
    case JULIA_FAULT_AUDIO_INIT: return "AUDIO_INIT";
    case JULIA_FAULT_VOICE_INIT: return "VOICE_INIT";
    case JULIA_FAULT_FSM_RUNTIME_INIT: return "FSM_RUNTIME_INIT";
    case JULIA_FAULT_FSM_STATE_CORRUPT: return "FSM_STATE_CORRUPT";
    case JULIA_FAULT_NVS_UNRECOVERABLE: return "NVS_UNRECOVERABLE";
    case JULIA_FAULT_FLASH_IO: return "FLASH_IO";
    case JULIA_FAULT_OTA_ROLLBACK_UNAVAILABLE: return "OTA_ROLLBACK_UNAVAILABLE";
    case JULIA_FAULT_CORE_TASK_STALLED: return "CORE_TASK_STALLED";
    case JULIA_FAULT_NONE:
    default: return "NONE";
    }
}

esp_err_t julia_fault_read_last(julia_fault_record_t *record)
{
    if (record == NULL) return ESP_ERR_INVALID_ARG;
    memset(record, 0, sizeof(*record));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(FAULT_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;

    size_t size = sizeof(*record);
    err = nvs_get_blob(handle, FAULT_RECORD_KEY, record, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    if (size != sizeof(*record) || record->schema_version != FAULT_SCHEMA_VERSION) {
        memset(record, 0, sizeof(*record));
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

esp_err_t julia_fault_record(julia_fault_reason_t reason, esp_err_t error,
                             julia_main_state_t main_state,
                             julia_s2_sub_state_t s2_sub_state)
{
    if (reason <= JULIA_FAULT_NONE ||
        reason > JULIA_FAULT_CORE_TASK_STALLED ||
        !julia_fsm_state_is_valid(main_state, s2_sub_state)) {
        return ESP_ERR_INVALID_ARG;
    }

    julia_fault_record_t previous;
    bool previous_valid = julia_fault_read_last(&previous) == ESP_OK;
    uint32_t sequence = previous_valid ? previous.sequence + 1U : 1U;
    uint32_t repeat_count = previous_valid &&
                            previous.reason == (uint32_t)reason &&
                            previous.uptime_ms <
                                (uint32_t)CONFIG_JULIA_FAULT_QUICK_UPTIME_SECONDS * 1000U
                                ? previous.repeat_count + 1U : 1U;
    julia_fault_record_t record = {
        .schema_version = FAULT_SCHEMA_VERSION,
        .sequence = sequence,
        .repeat_count = repeat_count,
        .reason = (uint32_t)reason,
        .error_code = (int32_t)error,
        .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .free_heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        .reset_reason = (uint32_t)esp_reset_reason(),
        .main_state = (uint8_t)main_state,
        .s2_sub_state = (uint8_t)s2_sub_state,
    };
    const esp_app_desc_t *app = esp_app_get_description();
    if (app != NULL) {
        strncpy(record.firmware_version, app->version,
                sizeof(record.firmware_version) - 1U);
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(FAULT_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, FAULT_RECORD_KEY, &record, sizeof(record));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        ESP_LOGE(TAG, "故障快照已保存：seq=%lu repeat=%lu reason=%s err=%s state=%s/%s",
                 (unsigned long)record.sequence, (unsigned long)record.repeat_count,
                 julia_fault_reason_name(reason),
                 esp_err_to_name(error), julia_fsm_main_state_name(main_state),
                 julia_fsm_s2_sub_state_name(s2_sub_state));
    } else {
        ESP_LOGE(TAG, "故障快照保存失败：reason=%s nvs=%s",
                 julia_fault_reason_name(reason), esp_err_to_name(err));
    }
    return err;
}

bool julia_fault_reset_allowed(void)
{
    julia_fault_record_t record;
    if (julia_fault_read_last(&record) != ESP_OK) return true;
    return record.repeat_count <= CONFIG_JULIA_FAULT_AUTO_RESET_LIMIT;
}
