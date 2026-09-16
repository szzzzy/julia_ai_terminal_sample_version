/**
 * @file    mqtt_comm.h
 * @brief   连接 MQTT Broker，把完整下行消息交给对应业务，并提供上行发布能力。
 *
 * 各业务在启动前登记自己需要接收的主题。连接成功后本模块统一订阅；一条消息即使
 * 被底层拆成多个片段，也会先恢复成完整内容，再交给语音、升级或音频业务处理。
 *
 * 本模块负责连接、订阅、消息恢复和发送。它不会下载固件、写升级分区或解释
 * 语音命令；这些操作由收到完整消息的业务模块完成。
 *
 * 普通状态上报只保证交给 MQTT 客户端，不等待服务器确认；升级成功、失败等关键
 * 事件则由升级报告模块先保存，收到 Broker 确认后才删除，以便断线后补发。
 *
 * MQTT 只承载语音控制消息，麦克风和回答声音走 WSS。连接变化只通知行为运行时；
 * 本模块不直接选择 S7.1 的返回状态，也不操作 offline 标签。
 *
 * @note 所有 topic 必须在 mqtt_comm_start() 生效前注册（app_main 装配阶段）；启动后
 *       注册的新 topic 不会随已建立的会话订阅。此外，晚注册的 critical topic 既不订阅、
 *       也不阻塞就绪判定，会静默失去 critical 语义（见 mqtt_comm_register_topic）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 一条已订阅消息完整到达后调用的业务处理函数。
 *
 * 内容末尾附带字符串结束符，长度不包含该结束符。处理函数运行在 MQTT 的公共
 * 事件任务中，只能快速校验并转交工作，不能等待下载或直接写入 Flash。
 *
 * @param[in] payload     完整消息载荷首地址，不要求调用方释放。
 * @param[in] payload_len 载荷有效长度，单位为字节。
 */
typedef void (*mqtt_inbound_handler_t)(const char *payload, size_t payload_len);

/**
 * @brief 登记一个要订阅的 MQTT 主题及其业务处理函数。
 *
 * @param[in] topic          要订阅的完整 topic，不允许为 NULL；字符串会被复制。
 * @param[in] max_payload_len 该 topic 单条消息允许的最大长度（不含末尾 NUL），
 *                            不得超过 NATIVE_OTA_JSON_MAX_LEN。超出该长度的下行消息
 *                            会被整条丢弃，且不产生回执或重传请求。
 * @param[in] critical       为 true 表示缺少该主题时核心控制功能不可用；订阅失败
 *                            或长时间没有确认时会重新建立 MQTT 连接，且全部 critical
 *                            订阅确认后才置就绪位。仅对启动前注册的 topic 有效：启动
 *                            后注册的 critical topic 不会订阅，也不会阻塞就绪判定。
 * @param[in] handler        完整载荷处理回调，不允许为 NULL。
 *
 * @return ESP_OK 注册成功（同名 topic 重复注册时覆盖旧配置）。
 * @return ESP_ERR_INVALID_ARG 参数无效、topic 过长或 max_payload_len 超出重组缓冲区容量。
 * @return ESP_ERR_NO_MEM 注册表已满。
 *
 * @note 必须在 mqtt_comm_start() 之前调用；不允许在中断上下文中调用。
 */
esp_err_t mqtt_comm_register_topic(const char *topic, size_t max_payload_len,
                                   bool critical, mqtt_inbound_handler_t handler);

/**
 * @brief 以 QoS 1 且不保留的方式发布一条普通 MQTT 消息。
 *
 * 返回成功只表示 MQTT 客户端接受了消息，不表示 Broker 已确认，也不表示服务器
 * 已完成业务处理。需要断线补发的升级关键事件必须使用升级报告模块。
 *
 * @param[in] topic     完整发布 topic，不允许为 NULL。
 * @param[in] data      消息内容首地址，不允许为 NULL。
 * @param[in] data_len  消息长度，单位为字节，大于 0 且不超过 INT_MAX。
 *
 * @return ESP_OK 消息已交给 ESP-MQTT 发送队列。
 * @return ESP_ERR_INVALID_ARG 参数无效。
 * @return ESP_ERR_INVALID_STATE MQTT 客户端尚未创建。
 * @return ESP_FAIL ESP-MQTT 发布接口拒绝请求。
 *
 * @note 可在任意普通任务上下文中调用；不允许在中断上下文中调用。
 */
esp_err_t mqtt_comm_publish(const char *topic, const char *data, size_t data_len);
/**
 * @brief 把一条本机状态封装镜像到 `voice/<device_id>/vstatus`，尽力而为。
 *
 * QoS 0 且不 retain：离线时跳过，outbox 已积压 4096 字节时也跳过，因此镜像可能整条
 * 丢失，服务器必须容忍缺失；镜像成功不代表控制已执行，WSS 状态同步 ACK 才是权威。
 *
 * @param[in] device_id 本机稳定设备 ID，由调用方提供；直接用于拼接 topic，不做校验。
 * @param[in] data      完整状态封装 JSON，不允许为 NULL。
 * @param[in] data_len  长度，单位为字节，大于 0 且不超过 512。
 *
 * @return ESP_OK 已交给 ESP-MQTT 的 QoS 0 发送队列。
 * @return ESP_ERR_INVALID_ARG 参数无效或长度超过 512。
 * @return ESP_ERR_INVALID_STATE 客户端不存在或未就绪（含主动暂停期间）。
 * @return ESP_ERR_NO_MEM outbox 已满而主动跳过本条；不是内存分配失败。
 * @return ESP_ERR_INVALID_SIZE 拼接出的 topic 超出局部缓冲区。
 * @return ESP_FAIL ESP-MQTT 拒绝入队。
 */
esp_err_t mqtt_comm_publish_voice_status(const char *device_id, const char *data, size_t data_len);

/**
 * @brief 启动 MQTT 连接、订阅和升级检查后台任务。
 *
 * 每次连接成功都会重新订阅所有已登记主题；固件检查仅在 S3 且服务就绪时发起，
 * 按配置周期检查。主动休眠停止客户端，异常断线由 MQTT 客户端自动重连。
 *
 * @return ESP_OK 客户端成功启动。
 * @return ESP_FAIL ESP-MQTT 客户端初始化失败。
 * @return 其他 esp_err_t 事件注册或客户端启动失败的错误码。
 *
 * @note 必须在 NVS、默认事件循环初始化完成且已获得 IP 后调用。重复调用是幂等的：
 *       已启动的客户端、任务和事件组会被复用，不会重复创建。
 * @note 本函数不允许在中断上下文中调用。
 */
esp_err_t mqtt_comm_start(void);

/**
 * @brief 请求暂停或恢复 MQTT 客户端。
 *
 * true 只是请求：检查任务在自身循环里执行 esp_mqtt_client_stop()，因为该调用不能在
 * 任何回调上下文里执行。请求期间 mqtt_comm_is_ready() 为 false，状态上报与 OTA 检查
 * 自行停住；未确认的关键事件仍保留在 ota_report 的持久化队列中。
 *
 * @note 调用方为 network_lifecycle（静默/唤醒）与 mqtt_comm_ip_ready()（重新联网）；
 *       可在普通任务上下文调用，不得从 ISR 调用。
 */
void mqtt_comm_set_paused(bool paused);

/**
 * @brief 查询客户端是否确实已停止。
 *
 * 需要“暂停请求已置位”并且“客户端已停止或尚未创建”同时成立；用于 network_lifecycle
 * 判断能否安全停射频，恢复请求刚置位时仍返回 false。
 */
bool mqtt_comm_is_paused(void);

/**
 * @brief 设备已经取得 IPv4 地址时启动 MQTT 服务。
 *
 * 供 network_lifecycle 的 ip_ready 回调注册使用；失败会由网络生命周期任务
 * 按有界退避重试。
 *
 * @param[in] arg 回调参数，本实现未使用。
 * @return mqtt_comm_start() 的返回值。
 */
esp_err_t mqtt_comm_ip_ready(void *arg);
/**
 * 线程安全地读取就绪快照。true 不仅要求 TCP/MQTT 已连接，还要求全部 critical
 * topic 收到 SUBACK；主动暂停期间恒为 false。调用不阻塞，可供状态巡检 Task 使用。
 */
bool mqtt_comm_is_ready(void);

#ifdef __cplusplus
}
#endif
