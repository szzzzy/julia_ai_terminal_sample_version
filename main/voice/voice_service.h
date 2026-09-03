/**
 * @file    voice_service.h
 * @brief   解释服务器的语音命令，并组织麦克风上传、回答播放和文件发送。
 *
 * MQTT 负责下发“开始说话、结束说话、发送文件和结束交流”等控制信息；
 * WSS 负责上传麦克风、接收回答声音并传输文件。两条连接收到的命令最终都在
 * 负责 WSS 收发的同一个任务中按顺序执行，避免控制与音频互相越过。
 *
 * 文件发送必须先声明文件名和大小，随后发送文件内容，最后报告实际字节数；
 * 缺少结束标记时服务器必须丢弃文件。本模块不直接驱动麦克风硬件，也不编码音频。
 *
 * 麦克风是否上传与设备是否正在听用户说话不是同一件事：默认服务器唤醒模式下，
 * 待机时也持续上传声音供服务器识别唤醒词；MIC_START 只表示用户已经开始本轮话语。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 语音命令（含 FILE_SEND 的 URI 部分）的最大长度，含末尾 NUL。 */
#define VOICE_SERVICE_CMD_MAX_LEN 128

/** FILE_SEND / 命令 URI 的最大长度（含末尾 NUL）。 */
#define VOICE_SERVICE_URI_MAX_LEN 128

/**
 * @brief 接通麦克风上传和服务器回答播放，使语音命令开始具备实际作用。
 *
 * @note 必须先初始化板级麦克风和扬声器，再调用本函数；随后才能建立 WSS 连接。
 */
esp_err_t voice_service_init_board_audio(void);

/**
 * @brief 准备语音命令入口，并登记需要订阅的 MQTT 主题。
 *
 * 此时只登记主题，不连接服务器。设备取得 IPv4 地址并且显示、声音与状态管理
 * 都就绪后，才会启动语音连接。
 *
 * @return ESP_OK 初始化成功。
 * @return 其他 esp_err_t 通信层 topic 注册失败。
 *
 * @note 必须在 mqtt_comm_start() 生效前（即网络启动前）调用；幂等。
 */
esp_err_t voice_service_init(void);

/**
 * @brief 设备已经联网时启动语音服务器连接。
 *
 * 供 network_lifecycle 的 ip_ready 回调注册使用；失败会由网络生命周期任务
 * 按有界退避重试。
 *
 * @param[in] arg 回调参数，本实现未使用。
 * @return wss_transport_start() 的返回值。
 */
esp_err_t voice_service_ip_ready(void *arg);

/**
 * @brief 请求把设备上的一个 WAV 文件发送给语音服务器。
 *
 * @param[in] uri 文件 URI："SD:/x/y" 映射到 /sdcard/x/y，"SPIFFS:/x/y" 映射到
 *                /spiffs/x/y；非法格式以 ERROR bad_uri 拒绝。
 *
 * @return ESP_OK 命令已入队，由当前 WSS 会话任务执行；断链时队列不重放。
 * @return ESP_ERR_INVALID_ARG uri 为空。
 * @return ESP_ERR_INVALID_SIZE uri 超出上限。
 * @return ESP_ERR_NO_MEM 命令队列已满。
 * @return ESP_ERR_INVALID_STATE WSS 客户端尚未启动。
 *
 * @note 本函数只登记请求并立即返回；实际读取和发送由语音连接任务完成。
 */
esp_err_t voice_service_send_file(const char *uri);

/**
 * @brief 把一块麦克风声音加入当前语音连接的待发送缓冲区。
 *
 * 只有语音连接可用且允许上传时才接收。数据完整发出后才从缓冲区移除；
 * 连接断开时丢弃尚未发出的旧声音，避免重连后把过期话语交给服务器。
 *
 * @param[in] buf 音频数据首地址，不允许为 NULL。
 * @param[in] len 数据长度，1～656 字节。
 *
 * @return ESP_OK 数据块已写入ring。
 * @return ESP_ERR_INVALID_ARG buf 为空或 len 为 0。
 * @return ESP_ERR_INVALID_SIZE len 超过 PCM1 最大帧长。
 * @return ESP_ERR_NO_MEM 麦克风待发送缓冲区已满，本轮连接将被安全结束。
 * @return ESP_ERR_INVALID_STATE WSS 客户端尚未启动。
 */
esp_err_t voice_service_send_chunk(const uint8_t *buf, size_t len);

/**
 * @brief 确认用户已经开始本轮说话。
 *
 * 默认服务器唤醒模式下，麦克风此前可能已经在上传；本命令的业务含义是
 * “唤醒完成，用户现在开始表达”。播放中的唤醒回应或旧回答会先被停止。
 *
 * @return ESP_OK 命令已入队；ESP_ERR_NO_MEM 命令队列已满；ESP_ERR_INVALID_STATE 尚未启动。
 */
esp_err_t voice_service_mic_start(void);

/**
 * @brief 确认用户本轮话语已经结束，设备开始等待回答。
 *
 * 本命令不等于关闭麦克风上传。默认服务器唤醒模式下，只要 WSS 连接仍在，
 * 待机、等待回答和播放回答期间都可以继续上传声音。
 *
 * @return ESP_OK 命令已入队；ESP_ERR_NO_MEM 命令队列已满；ESP_ERR_INVALID_STATE 尚未启动。
 */
esp_err_t voice_service_mic_stop(void);

#ifdef __cplusplus
}
#endif
