/**
 * @file    wss_transport.h
 * @brief   建立并维护设备到语音服务器的加密 WebSocket 连接。
 *
 * 本模块保证连接、握手、帧格式、保活和断线重连正确；它不判断一段二进制数据
 * 是回答声音还是文件。收到完整文本或二进制消息后交给语音服务解释，语音服务
 * 需要发送内容时也先交回本模块封装成 WebSocket 帧。
 *
 * 整个程序只有一个任务可以直接读写该连接。其它任务只能提交待发送内容，
 * 从而避免两个任务同时操作同一个加密连接造成帧交叉或连接损坏。
 *
 * 证书复用构建内嵌的 server_certs/ca_cert.pem 信任锚；本模块不访问 OTA 分区、
 * 不打开文件；独立停滞监测仅在升级恢复时通过 julia_fault 写入 NVS 故障记录。
 *
 * 例如“必须先收到开始播放命令才能接收回答声音”“文件必须以 BEGIN/END 包围”
 * 都是语音业务规则，由语音服务检查；本模块只保证消息完整、顺序正确且不超长。
 */
#pragma once


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t wss_transport_generation(void);
bool wss_transport_is_owner(void);
/* 运行时观测接口：只在 WSS owner 上调用；其他上下文调用为空操作。 */
void wss_transport_fsm_wait(bool waiting);

/** 一条完整文本或二进制消息允许携带的最大业务数据量。 */
#define WSS_TRANSPORT_MAX_PAYLOAD 1200

/**
 * @brief 服务器发来一条完整文本消息时调用。
 *
 * @param[in] data 文本载荷，不保证以 NUL 结尾。
 * @param[in] len  文本长度，单位为字节。
 *
 * @note 处理期间会暂停继续接收；返回后消息内存失效，因此需要长期保存时必须复制。
 */
typedef void (*wss_transport_text_cb_t)(const uint8_t *data, size_t len);

/**
 * @brief 服务器发来一条完整二进制消息时调用，例如一块回答声音。
 *
 * 分成多个 WebSocket 帧到达的消息会先重组，业务层始终看到完整内容。
 */
typedef void (*wss_transport_binary_cb_t)(const uint8_t *data, size_t len);

/**
 * @brief 轮到处理其它任务提交的待发送业务内容时调用。
 *
 * @param[in] item      待处理内容，由语音服务定义，仅在回调返回前有效。
 * @param[in] item_size 内容长度，必须与启动时约定一致。
 *
 * @note 在会话任务上下文中同步执行，因此回调必须保持有界、不得阻塞调用方；
 *       回调内可直接调用 wss_transport_send_now()。
 * @note item 指向队列条目接收缓冲，内存只在本次回调期间有效，需要长期保存时必须自行复制。
 */
typedef void (*wss_transport_queue_item_cb_t)(void *item, size_t item_size);

/**
 * @brief 加密连接和 WebSocket 认证都成功、可以开始收发业务消息时调用。
 *
 * @note 语音服务可在这里开始上传麦克风；需要立即回复时可直接发送。
 */
typedef void (*wss_transport_session_start_cb_t)(void);

/** 会话结束原因：当前只进入断链归因日志和 on_session_end 的 reason 参数，不改变重连节奏
 * （重连下限由 CLOSE 码决定，见 wss_auth_policy.h 的 wss_close_retry_floor）。 */
typedef enum {
    WSS_TRANSPORT_END_NONE = 0, /**< 尚无归因，仅作为"未记录"的初值。 */
    WSS_TRANSPORT_END_PEER_CLOSE, /**< 对端发来 CLOSE（含 4401 认证拒绝等应用码）。 */
    WSS_TRANSPORT_END_RX_ERROR, /**< 接收失败、EOF 或非法帧，链路已不可用。 */
    WSS_TRANSPORT_END_TX_ERROR, /**< 写方向永久错误，无法再送出字节。 */
    WSS_TRANSPORT_END_TX_STALL, /**< 帧级写期限耗尽，对端长时间不收数据。 */
    WSS_TRANSPORT_END_KEEPALIVE_TIMEOUT, /**< 发出 PING 后探测窗口内没有任何下行帧。 */
    WSS_TRANSPORT_END_PROTOCOL_ERROR, /**< 本地按 RFC 6455 判定协议错误并以 1002/1007 关闭。 */
    WSS_TRANSPORT_END_APPLICATION_ERROR, /**< 上层判定业务无法安全继续，请求结束会话。 */
    WSS_TRANSPORT_END_AUDIO_OVERFLOW, /**< 上行缓冲写满，上层要求结束会话并重新开始。 */
    WSS_TRANSPORT_END_REASON_COUNT, /**< 枚举计数哨兵，不是有效原因。 */
} wss_transport_end_reason_t;

/**
 * @brief 连接关闭或确认失效、准备等待重连时调用。
 *
 * @param[in] reason 本次连接结束的主要原因。
 * @note 语音服务应在这里停止上传和播放，并丢弃只属于旧连接的数据。
 * @note 回调发生在 TLS 句柄销毁之后、重连等待之前，仍在会话任务上下文；此时链路已不可用，
 *       任何发送都会失败，属预期行为。
 */
typedef void (*wss_transport_session_end_cb_t)(wss_transport_end_reason_t reason);

/**
 * @brief 语音服务交给连接层的消息处理函数和队列容量。
 */
typedef struct {
    wss_transport_text_cb_t on_text; /**< 服务端文本消息回调，可为 NULL。 */
    wss_transport_binary_cb_t on_binary; /**< 服务端二进制消息回调，可为 NULL。 */
    wss_transport_queue_item_cb_t on_queue_item; /**< 普通命令队列条目回调，不允许为 NULL。 */
    wss_transport_session_start_cb_t on_session_start; /**< 会话认证成功回调，可为 NULL。 */
    wss_transport_session_end_cb_t on_session_end; /**< 会话结束回调，可为 NULL。 */
    void (*on_poll)(void); /**< 每轮收到一帧（含空闲超时）后调用一次，用于推进少量语音或文件
                            *   数据；必须保持有界、不得阻塞。可为 NULL。 */
    /** 可选的下行背压门控：返回 false 时把 recv 推迟 10 ms，但仍继续处理发送与 on_poll；
     * 回调必须自己负责在下行长期停滞时结束会话。 */
    bool (*can_receive)(void);
    size_t queue_item_size; /**< 每项待发送业务内容占用的字节数；普通队列与固定 4 槽的控制队列共用。 */
    unsigned queue_depth; /**< 普通待发送内容的队列深度；控制队列固定 4 槽，不受该字段影响。 */
} wss_transport_config_t;

/**
 * @brief 启动语音服务器连接和自动重连任务。
 *
 * 连接失败或使用过程中断开后，会等待配置的时间再连接；调用方不需要另建重连任务。
 *
 * @param[in] config 启动配置，不允许为 NULL；on_queue_item 必须有效。
 *
 * @return ESP_OK 客户端已启动。
 * @return ESP_ERR_INVALID_ARG 配置无效。
 * @return ESP_ERR_NO_MEM 队列条目缓冲、命令队列或会话任务创建失败。
 * @return ESP_ERR_INVALID_STATE 已有一次启动正在进行。
 *
 * @note 幂等：重复调用直接复用已创建的会话任务，不会重复分配。
 * @note 必须取得 IPv4 后调用；本函数不允许在中断上下文中调用。
 */
esp_err_t wss_transport_start(const wss_transport_config_t *config);
/** 暂停或恢复整个收发循环：暂停请求会唤醒会话任务，使其立即停止连接与收发。
 * 恢复后从重连流程重新开始，不在暂停期间保留旧会话。 */
void wss_transport_set_paused(bool paused);
/** true 表示"已请求暂停且会话任务确实已停下"；暂停请求刚发出、任务尚未就绪时仍为 false。 */
bool wss_transport_is_paused(void);

/**
 * @brief 提交一项待语音连接处理的业务内容，例如文件发送请求。
 *
 * @param[in] item      条目内容首地址，不允许为 NULL；内容会被复制。
 * @param[in] item_size 条目大小，必须等于启动时配置的 queue_item_size。
 *
 * @return ESP_OK 已入队。
 * @return ESP_ERR_INVALID_ARG item 为 NULL。
 * @return ESP_ERR_INVALID_SIZE item_size 与队列条目大小不匹配。
 * @return ESP_ERR_NO_MEM 命令队列已满。
 * @return ESP_ERR_INVALID_STATE 客户端尚未启动或 WSS 会话未就绪。
 *
 * @note 可被任意普通任务（如 MQTT 事件任务）调用；只入队不阻塞。
 */
esp_err_t wss_transport_enqueue(const void *item, size_t item_size);

/** 提交一项高优先级语音控制内容；麦克风数据不会占用这组容量。 */
esp_err_t wss_transport_enqueue_control(const void *item, size_t item_size);
/** 业务处理已经无法安全继续时，要求关闭当前连接并重新建立。 */
void wss_transport_fail_session(void);
/** 抬高下一次重连等待的下限，单位秒，上限 300 s，只抬高不降低；认证成功后由会话任务清零。
 * 只允许在 WSS owner 上下文调用，不阻塞该回调。当前调用点传入 30/60（见 wss_close_retry_floor）。 */
void wss_transport_defer_retry(unsigned seconds);

/**
 * 从其它任务请求结束当前语音连接。调用方只说明原因，不能直接关闭加密连接；
 * 负责收发的任务会在当前完整写入结束后统一清理。主要用于麦克风缓冲已满。
 *
 * @note 只有第一次请求生效，后续原因被忽略；请求成功即关闭就绪标志，新的入队会立刻被拒绝，
 *       但 TLS 释放始终由 owner 执行。
 */
esp_err_t wss_transport_request_session_end(wss_transport_end_reason_t reason);
/**
 * 线程安全地读取会话就绪快照。true 表示 TLS、WebSocket 握手和认证均完成；
 * 调用不接触 TLS 句柄、不阻塞，可供连接状态巡检 Task 使用。
 *
 * @note 实现先判暂停与 quiet 再看会话标志：暂停期间恒为 false。quiet 只在
 *       CONFIG_JULIA_LOCAL_CAPTURE_ENABLE 关闭的兼容分支下才使本函数恒为 false；该选项
 *       开启（当前生效）时 wss_quiet_blocks() 恒为 false，S5/S6 仍可保持连接与就绪。
 *       因此 false 只说明"当前不可收发"，不等于握手失败或链路已断；FSM 的 WSS 在线判据
 *       建立在本函数之上，不能用它区分"暂停"与"断链"。
 */
bool wss_transport_is_ready(void);
const char *wss_transport_end_reason_name(wss_transport_end_reason_t reason);

/**
 * @brief 从负责连接的任务中发送一条 WebSocket 消息。
 *
 * 发送前按 RFC 6455 校验：opcode 只允许已定义值（0x0 续帧、0x1 文本、0x2 二进制、
 * 0x8 CLOSE、0x9 PING、0xA PONG）；控制帧载荷不得超过 125 字节；载荷非空时
 * payload 不得为 NULL；载荷不得超过 WSS_TRANSPORT_MAX_PAYLOAD。
 *
 * @param[in] opcode  帧操作码（0x1 文本、0x2 二进制、0x9 PING、0xA PONG 等）。
 * @param[in] payload 载荷首地址，len 为 0 时可为 NULL。
 * @param[in] len     载荷长度，不允许超过 WSS_TRANSPORT_MAX_PAYLOAD；
 *                    控制帧不允许超过 125。
 *
 * @return ESP_OK 发送完成。
 * @return ESP_ERR_INVALID_ARG 保留操作码、控制帧超长、载荷超长或载荷非空但
 *         payload 为 NULL；不标记会话故障。
 * @return ESP_FAIL 会话无效、永久写错误或帧级写期限耗尽；暂时的
 *         WANT_READ/WANT_WRITE/EAGAIN 会先有限重试。最终失败会标记会话故障，
 *         会话循环将关闭链路并重连。
 *
 * @note 只能在本模块调用语音服务的处理函数期间使用；其它任务必须先提交到队列。
 */
esp_err_t wss_transport_send_now(uint8_t opcode, const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif
