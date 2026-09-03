/**
 * @file voice_uplink_ring.h
 * @brief 在麦克风采集速度短时快于网络发送速度时，暂存完整声音块。
 *
 * 麦克风只放入完整声音块；负责语音连接的任务确认整块发送成功后才移除。
 * 每块声音都记录所属连接，断线前积压的内容不会在新连接中补发，避免服务器把
 * 旧话语误认为新的用户输入。容量固定，装满时调用方必须结束本轮连接而不是静默跳帧。
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

/** 使用调用方提供的内存建立缓冲；格数必须为 2 的幂，长期运行序号回绕后仍能正确定位。 */
bool voice_uplink_ring_init(voice_uplink_ring_t *ring,
                            uint8_t *storage, uint16_t *lengths,
                            uint32_t *slot_generations, size_t capacity,
                            size_t frame_capacity);

/** 为一条新的语音连接启用空缓冲；每次重连必须使用新的数据归属编号。 */
bool voice_uplink_ring_start_generation(voice_uplink_ring_t *ring,
                                        uint32_t generation);
/** 停止接收新的麦克风声音；已积压内容暂不移动，由负责连接的任务统一清理。 */
void voice_uplink_ring_close_generation(voice_uplink_ring_t *ring);
/** 结束当前语音连接并丢弃所有尚未发送的旧声音。 */
void voice_uplink_ring_stop_generation(voice_uplink_ring_t *ring);

/** 麦克风任务非阻塞地加入一块完整声音。 */
voice_uplink_push_result_t voice_uplink_ring_push(voice_uplink_ring_t *ring,
                                                   const uint8_t *data,
                                                   size_t len);
/** 取得当前连接最早的一块待发送声音；内部会跳过属于旧连接的残留内容。 */
bool voice_uplink_ring_peek(voice_uplink_ring_t *ring,
                            uint32_t expected_generation,
                            const uint8_t **data, size_t *len);
/** 一块声音完整发送成功后，将它从缓冲区移除。 */
bool voice_uplink_ring_consume(voice_uplink_ring_t *ring);

size_t voice_uplink_ring_count(const voice_uplink_ring_t *ring);
uint32_t voice_uplink_ring_generation(const voice_uplink_ring_t *ring);
bool voice_uplink_ring_is_accepting(const voice_uplink_ring_t *ring);
