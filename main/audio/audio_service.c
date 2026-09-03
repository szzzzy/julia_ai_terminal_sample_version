/**
 * @file    audio_service.c
 * @brief   音频服务层实现：MQTT 音频控制面 <-> 音频下载引擎的业务协调入口。
 *
 * 模块关系：
 * - 从 mqtt_comm.c 接收一条已完成 MQTT 分片重组的 audio_check_response；
 * - 使用 audio_control_plane 解析校验，得到 native_audio_manifest_t；
 * - 与固件 OTA 下载互斥：OTA 下载进行中时拒绝启动音频下载（OTA 优先），
 *   音频下载自身也拒绝并发；
 * - 将下载请求交给 audio_engine。
 *
 * 本模块本身不持有运行状态：下载是否进行中转调 audio_engine_is_running()。
 *
 * NOTE：需结合调用方确认——本代码库中 mqtt_comm 目前只把 ota_check_response 注册到
 * ota_engine_handle_server_json()，audio_service_handle_response() 尚未被任何
 * MQTT 主题回调或注册表引用（见 mqtt_comm.c mqtt_handle_ota_response）。也就是说
 * 音频"检查->下载"链路在此分支里仍是"待接线"：入口已就绪，但缺一条
 * audio_check_response 主题的注册/分发，需确认由哪一层接入。
 */
#include "audio_service.h"

#include <stdbool.h>

#include "esp_log.h"

#include "audio_control_plane.h"
#include "audio_engine.h"
#include "ota_engine.h"
#include "ota_types.h"

static const char *TAG = "audio_service";

/**
 * @brief 初始化音频服务层（当前无自有状态，为装配点保留）。
 *
 * 已在 main.c 中、NVS 初始化后调用；幂等；不在中断上下文调用。若后续增加
 * 服务层状态，应在此处初始化并保证与 OTA 服务层的初始化顺序一致。
 */
esp_err_t audio_service_init(void)
{
    /* 当前服务层没有自有状态；保留初始化点与 OTA 服务层保持一致。 */
    return ESP_OK;
}

/**
 * @brief 处理服务器返回的完整 audio_check_response（入口）。
 *
 * @param[in] json     JSON 数据首地址，不允许为 NULL，不要求以 NUL 结尾。
 * @param[in] json_len JSON 有效长度，范围为 1～NATIVE_OTA_JSON_MAX_LEN。
 *
 * @return ESP_OK 响应有效；可能无需下载（update=false），也可能已创建下载任务。
 * @return ESP_ERR_INVALID_ARG 参数或 JSON/清单字段无效。
 * @return ESP_ERR_INVALID_STATE request_id 过期，或已有 OTA/音频下载任务运行。
 * @return ESP_ERR_NO_MEM 参数分配或任务创建失败。
 *
 * @note 在 MQTT/通信事件任务上下文中调用：本函数只做解析与"调度"，不在调用者
 *       线程里执行下载、不写 Flash、不阻塞等待；下载在独立的 audio_task 中发生。
 */
esp_err_t audio_service_handle_response(const char *json, size_t json_len)
{
    if (json == NULL || json_len == 0U || json_len > NATIVE_OTA_JSON_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    native_audio_manifest_t manifest;
    bool download_requested = false;
    /* 解析失败时 manifest 会被清零、download_requested 置 false（见 .c 注释），
     * 这里直接返回错误码即可，无需再处理输出。 */
    esp_err_t err = audio_control_plane_parse_audio_response(json, json_len,
                                                             &manifest,
                                                             &download_requested);
    if (err != ESP_OK) {
        return err;
    }
    if (!download_requested) {
        /* update=false：正常"无需更新"，静默返回。 */
        return ESP_OK;
    }

    /* 固件 OTA 与音频下载共用网络/Flash 资源，v1 策略：OTA 优先，互斥执行。
     * 注意这是"检查后创建前"的瞬时互斥：即使此处通过，audio_engine_start 也会
     * 在单飞临界区里再次防止并发音频任务。 */
    if (ota_engine_is_running() || audio_engine_is_running()) {
        ESP_LOGW(TAG, "Audio download rejected: OTA or another audio download is running");
        return ESP_ERR_INVALID_STATE;
    }

    return audio_engine_start(&manifest);
}
