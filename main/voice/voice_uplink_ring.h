/**
 * @file voice_uplink_ring.h
 * @brief 在麦克风采集速度短时快于网络发送速度时，暂存完整声音块。
 *
 * 麦克风只放入完整声音块；负责语音连接的任务确认整块发送成功后才移除。
 * 每块声音都记录所属连接，断线前积压的内容不会在新连接中补发，避免服务器把
 * 旧话语误认为新的用户输入。容量固定，装满时调用方必须结束本轮连接而不是静默跳帧。
 *
 * 唯一 producer 是板级采音任务，唯一 consumer 是 WSS owner（会话任务），因此是无锁
 * SPSC 结构：跨任务同步点只有槽内容与写序号、读序号，以及 owner 发布的代次和 accepting
 * 标志，读写指针本身不做互斥。已经发布但属于旧代次的槽由 consumer 按代次过滤丢弃，
 * 这一步不能省略。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    /** 已发布，consumer 之后可见。 */
    VOICE_UPLINK_PUSH_OK = 0,
    /** 当前没有可接收的代次（未开始或已被 owner 关闭）；调用方只丢弃本帧。 */
    VOICE_UPLINK_PUSH_INACTIVE,
    /** 缓冲已满；调用方必须关闭入口并以 audio_overflow 结束会话，不能静默跳帧。 */
    VOICE_UPLINK_PUSH_FULL,
    /** 参数非法（空指针、零长或超过单帧上限）；调用方只丢弃本帧。 */
    VOICE_UPLINK_PUSH_INVALID,
} voice_uplink_push_result_t;

typedef struct {
    uint8_t *storage;
    uint16_t *lengths;
    uint32_t *slot_generations;
    size_t capacity;
    size_t frame_capacity;
    volatile uint32_t write_sequence;
    volatile uint32_t read_sequence;
    volatile uint32_t active_generation;
    volatile bool accepting;
} voice_uplink_ring_t;

/** 使用调用方提供的内存建立缓冲。
 *
 * capacity 是槽数，必须为 2 的幂，长期运行序号回绕后仍能靠位与定位；frame_capacity
 * 是单帧字节上限，受长度表 uint16_t 限制不得超过 65535。storage 需提供
 * capacity×frame_capacity 字节，并与 lengths/slot_generations 一样由调用方持有，
 * 生命周期必须覆盖 ring 本身。 */
bool voice_uplink_ring_init(voice_uplink_ring_t *ring,
                            uint8_t *storage, uint16_t *lengths,
                            uint32_t *slot_generations, size_t capacity,
                            size_t frame_capacity);

/** 为一条新的语音连接启用空缓冲；每次重连必须使用新的数据归属编号，generation 为 0
 * 视为非法并返回 false。换代顺序固定为「关入口 → 丢弃已发布内容 → 写新代次 → 开入口」，
 * 顺序不可调整：先开入口会让 producer 把新帧写进尚未清理的槽。丢弃不覆盖此刻正在发布
 * 的那一格，该槽由 consumer 按代次过滤跳过。 */
bool voice_uplink_ring_start_generation(voice_uplink_ring_t *ring,
                                        uint32_t generation);
/** 停止接收新的麦克风声音；已积压内容暂不移动，由负责连接的任务统一清理。 */
void voice_uplink_ring_close_generation(voice_uplink_ring_t *ring);
/** 结束当前语音连接并丢弃所有尚未发送的旧声音：先关入口，再一次性丢弃；只由 owner 调用。 */
void voice_uplink_ring_stop_generation(voice_uplink_ring_t *ring);

/** 麦克风任务非阻塞地加入一块完整声音，沿用当前代次。 */
voice_uplink_push_result_t voice_uplink_ring_push(voice_uplink_ring_t *ring,
                                                   const uint8_t *data,
                                                   size_t len);
/** 预录/分段携带采集时的连接编号：expected_generation 为 0 表示沿用当前代次，非 0 时
 * 与当前代次不符即拒绝（与 peek 的 0 判非法相反）。禁止把旧段标成刚重连的新连接数据。 */
voice_uplink_push_result_t voice_uplink_ring_push_generation(voice_uplink_ring_t *ring,
    const uint8_t *data, size_t len, uint32_t expected_generation);
/** 取得当前连接最早的一块待发送声音。
 *
 * 前置条件：expected_generation 必须是调用方期望的代次且不得为 0。遇到属于其它代次的
 * 残留槽会推进读指针并永久丢弃该内容，不是保留跳过。返回的 data 指向 ring 内部存储，
 * 只在下一次 consume 之前有效。 */
bool voice_uplink_ring_peek(voice_uplink_ring_t *ring,
                            uint32_t expected_generation,
                            const uint8_t **data, size_t *len);
/** 一块声音完整发送成功后，将它从缓冲区移除；必须与紧邻的一次成功 peek 一一配对，
 * 发送失败时不得调用，否则会丢掉尚未发送的声音。 */
bool voice_uplink_ring_consume(voice_uplink_ring_t *ring);

/** 两次独立原子读得到的近似积压槽数，并发下单次调用可能偏大或偏小，不能当精确深度用。 */
size_t voice_uplink_ring_count(const voice_uplink_ring_t *ring);
uint32_t voice_uplink_ring_generation(const voice_uplink_ring_t *ring);
bool voice_uplink_ring_is_accepting(const voice_uplink_ring_t *ring);
