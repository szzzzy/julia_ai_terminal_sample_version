/**
 * @file voice_uplink_ring.h
 * @brief 单生产者/单消费者的固定槽音频上行环形缓冲。
 *
 * PCM 存储由调用方提供，固件使用 PSRAM；短小元数据由调用方放在内部 RAM。
 * producer 只发布完整帧，consumer 只有在 WSS 整帧发送成功后才 consume。
 * 每个槽绑定连接 generation，consumer 会丢弃不属于当前连接的迟到槽，禁止
 * 断线前音频进入重连后的新会话。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VOICE_UPLINK_PUSH_OK = 0,
    VOICE_UPLINK_PUSH_INACTIVE,
    VOICE_UPLINK_PUSH_FULL,
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

/** 初始化调用方提供的存储；capacity 必须是 2 的幂，保证序号回绕映射连续。 */
bool voice_uplink_ring_init(voice_uplink_ring_t *ring,
                            uint8_t *storage, uint16_t *lengths,
                            uint32_t *slot_generations, size_t capacity,
                            size_t frame_capacity);

/** 由 WSS owner 开始一个空的新上行代次；每条新连接必须使用新值。 */
bool voice_uplink_ring_start_generation(voice_uplink_ring_t *ring,
                                        uint32_t generation);
/** 由 WSS owner 停止当前代次并丢弃已发布积压。 */
void voice_uplink_ring_stop_generation(voice_uplink_ring_t *ring);

/** producer 非阻塞复制并发布一帧。 */
voice_uplink_push_result_t voice_uplink_ring_push(voice_uplink_ring_t *ring,
                                                   const uint8_t *data,
                                                   size_t len);
/** consumer 获取当前 generation 的队首；迟到旧代次槽会在内部跳过。 */
bool voice_uplink_ring_peek(voice_uplink_ring_t *ring,
                            uint32_t expected_generation,
                            const uint8_t **data, size_t *len);
/** consumer 在对应 peek 数据完整发送后释放队首。 */
bool voice_uplink_ring_consume(voice_uplink_ring_t *ring);

size_t voice_uplink_ring_count(const voice_uplink_ring_t *ring);
uint32_t voice_uplink_ring_generation(const voice_uplink_ring_t *ring);
bool voice_uplink_ring_is_accepting(const voice_uplink_ring_t *ring);
