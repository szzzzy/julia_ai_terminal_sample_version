/**
 * @file    audio_service.c
 * @brief   音频服务层实现：MQTT 音频控制面 <-> 音频下载引擎的业务协调入口。
 *
 * 设计链路（当前未接通，见下方现状）：
 * - 从 mqtt_comm.c 接收一条已完成 MQTT 分片重组的 audio_check_response；
 * - 使用 audio_control_plane 解析校验，得到 native_audio_manifest_t；
 * - 单向门禁：OTA 下载进行中时拒绝启动音频下载（OTA 优先），音频下载自身也拒绝并发；
 * - 将下载请求交给 audio_engine。
 *
 * 本模块本身不持有运行状态：下载是否进行中转调 audio_engine_is_running()。
 *
 * 现状（当前边界，不是待办实现）：MQTT 层只注册 OTA 的响应/通知主题并分发到
 * ota_engine_handle_server_json()，没有任何 audio_check_response 主题或分发点，
 * 因此 audio_service_handle_response() 与 native_audio_build_check_request() 都没有
 * 外部调用方（本层只有 audio_service_init() 被 main.c 调用）；audio_engine_start()
 * 的唯一调用点就是本文件的 audio_service_handle_response()（audio_service.c:97）。
 * 即"音频检查 → 下载"的入口已就绪，尚未接通外部触发入口的是检查请求的发送方与响应
 * 主题的注册方。
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
 *        两种结果共用该返回码，调用方无法据此区分（也没有应用层回执可代偿）。
 * @return ESP_ERR_INVALID_ARG 参数或 JSON/清单字段无效。
 * @return ESP_ERR_INVALID_STATE request_id 过期，或已有 OTA/音频下载任务运行。
 * @return ESP_ERR_NO_MEM 参数分配或任务创建失败。
 *
 * @note 在 MQTT/通信事件任务上下文中调用：本函数只做解析与"调度"，不在调用者
 *       线程里执行下载、不写 Flash、不阻塞等待；下载在独立的 audio_task 中发生。
 * @note 协议只有 MQTT PUBACK，没有应用层执行回执，因此服务器无法区分"被本函数拒绝、
 *       从未执行"与"已经开始/完成"——报错只对本地调用方有意义。
 */
esp_err_t audio_service_handle_response(const char *json, size_t json_len)
{
    /* 超长报文直接判为无效，不做截断后再解析：截断等于把服务器实际发送的 JSON 换成
     * 另一份文档，可能解析出与响应不符的清单。分片重组层已按 NATIVE_OTA_JSON_MAX_LEN
     * 限制单条 payload，超长说明上游契约已被破坏，应当整体丢弃。 */
    if (json == NULL || json_len == 0U || json_len > NATIVE_OTA_JSON_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    native_audio_manifest_t manifest;
    bool download_requested = false;
    /* 解析失败时 manifest 会被清零、download_requested 置 false（见 audio_control_plane.c
     * 的 cleanup 段），这里直接返回错误码即可，无需再处理输出。 */
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

    /* 固件 OTA 与音频下载共用网络/Flash 资源，v1 策略：OTA 优先，本层只做单向门禁。
     * 这是"检查后创建前"的瞬时判定，边界如下：
     *   - 只覆盖"音频不抢在 OTA 前面启动"这一侧：OTA 正在运行就拒绝启动音频；
     *   - 音频自身的并发由 audio_engine_start() 的单飞临界区兜底，引擎不查询 OTA 状态，
     *     因此不存在双向原子互斥；
     *   - 反向不成立——OTA 准入不查询 audio_engine_is_running()，检查通过后到任务真正
     *     创建之间若 OTA 被放行，两者可能短暂并行（当前业务链没有外部触发入口，
     *     该竞态在运行期不会被触发）。 */
    if (ota_engine_is_running() || audio_engine_is_running()) {
        ESP_LOGW(TAG, "Audio download rejected: OTA or another audio download is running");
        return ESP_ERR_INVALID_STATE;
    }

    return audio_engine_start(&manifest);
}
