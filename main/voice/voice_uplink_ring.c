/**
 * @file voice_uplink_ring.c
 * @brief 音频上行 SPSC 环形缓冲实现。
 */
#include "voice_uplink_ring.h"

#include <limits.h>
#include <string.h>

/* ESP32 双核构建使用 GCC 原子内建保证 producer/consumer 发布顺序；主机测试是
 * 单线程确定性调度，TinyCC 不提供 stdatomic.h，直接访问即可测试同一ring算法。 */
static uint32_t load_u32_acquire(const volatile uint32_t *value)
{
#ifdef ESP_PLATFORM
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#else
    return *value;
#endif
}

static uint32_t load_u32_relaxed(const volatile uint32_t *value)
{
#ifdef ESP_PLATFORM
    return __atomic_load_n(value, __ATOMIC_RELAXED);
#else
    return *value;
#endif
}

static void store_u32_release(volatile uint32_t *value, uint32_t next)
{
#ifdef ESP_PLATFORM
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
#else
    *value = next;
#endif
}

static bool load_bool_acquire(const volatile bool *value)
{
#ifdef ESP_PLATFORM
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#else
    return *value;
#endif
}

static void store_bool_release(volatile bool *value, bool next)
{
#ifdef ESP_PLATFORM
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
#else
    *value = next;
#endif
}

static void publish_fence(void)
{
#ifdef ESP_PLATFORM
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

static void discard_published(voice_uplink_ring_t *ring)
{
    uint32_t published = load_u32_acquire(&ring->write_sequence);
    store_u32_release(&ring->read_sequence, published);
}

bool voice_uplink_ring_init(voice_uplink_ring_t *ring,
                            uint8_t *storage, uint16_t *lengths,
                            uint32_t *slot_generations, size_t capacity,
                            size_t frame_capacity)
{
    if (ring == NULL || storage == NULL || lengths == NULL ||
        slot_generations == NULL || capacity == 0U ||
        capacity > (size_t)(UINT32_MAX / 2U) + 1U ||
        (capacity & (capacity - 1U)) != 0U ||
        frame_capacity == 0U || frame_capacity > UINT16_MAX ||
        capacity > SIZE_MAX / frame_capacity) {
        return false;
    }

    memset(ring, 0, sizeof(*ring));
    ring->storage = storage;
    ring->lengths = lengths;
    ring->slot_generations = slot_generations;
    ring->capacity = capacity;
    ring->frame_capacity = frame_capacity;
    ring->write_sequence = 0;
    ring->read_sequence = 0;
    ring->active_generation = 0;
    ring->accepting = false;
    return true;
}

bool voice_uplink_ring_start_generation(voice_uplink_ring_t *ring,
                                        uint32_t generation)
{
    if (ring == NULL || generation == 0U) return false;
    store_bool_release(&ring->accepting, false);
    discard_published(ring);
    store_u32_release(&ring->active_generation, generation);
    store_bool_release(&ring->accepting, true);
    return true;
}

void voice_uplink_ring_stop_generation(voice_uplink_ring_t *ring)
{
    if (ring == NULL) return;
    store_bool_release(&ring->accepting, false);
    discard_published(ring);
}

voice_uplink_push_result_t voice_uplink_ring_push(voice_uplink_ring_t *ring,
                                                   const uint8_t *data,
                                                   size_t len)
{
    if (ring == NULL || data == NULL || len == 0U ||
        len > ring->frame_capacity) {
        return VOICE_UPLINK_PUSH_INVALID;
    }
    if (!load_bool_acquire(&ring->accepting)) {
        return VOICE_UPLINK_PUSH_INACTIVE;
    }

    uint32_t generation = load_u32_acquire(&ring->active_generation);
    uint32_t write_sequence = load_u32_relaxed(&ring->write_sequence);
    uint32_t read_sequence = load_u32_acquire(&ring->read_sequence);
    if ((uint32_t)(write_sequence - read_sequence) >=
        (uint32_t)ring->capacity) {
        return VOICE_UPLINK_PUSH_FULL;
    }

    size_t slot = write_sequence & (ring->capacity - 1U);
    memcpy(ring->storage + slot * ring->frame_capacity, data, len);
    ring->lengths[slot] = (uint16_t)len;
    ring->slot_generations[slot] = generation;

    /* owner 可能在 memcpy 期间结束会话。发布前再校验；若 owner 恰好在本次校验
     * 之后结束，槽仍携带旧 generation，新连接 consumer 会识别并跳过。 */
    publish_fence();
    if (!load_bool_acquire(&ring->accepting) ||
        load_u32_acquire(&ring->active_generation) != generation) {
        return VOICE_UPLINK_PUSH_INACTIVE;
    }
    store_u32_release(&ring->write_sequence, write_sequence + 1U);
    return VOICE_UPLINK_PUSH_OK;
}

bool voice_uplink_ring_peek(voice_uplink_ring_t *ring,
                            uint32_t expected_generation,
                            const uint8_t **data, size_t *len)
{
    if (ring == NULL || expected_generation == 0U ||
        data == NULL || len == NULL) {
        return false;
    }

    for (;;) {
        uint32_t read_sequence = load_u32_relaxed(&ring->read_sequence);
        uint32_t write_sequence = load_u32_acquire(&ring->write_sequence);
        if (read_sequence == write_sequence) return false;

        size_t slot = read_sequence & (ring->capacity - 1U);
        uint16_t slot_len = ring->lengths[slot];
        if (ring->slot_generations[slot] != expected_generation ||
            slot_len == 0U || slot_len > ring->frame_capacity) {
            store_u32_release(&ring->read_sequence, read_sequence + 1U);
            continue;
        }
        *data = ring->storage + slot * ring->frame_capacity;
        *len = slot_len;
        return true;
    }
}

bool voice_uplink_ring_consume(voice_uplink_ring_t *ring)
{
    if (ring == NULL) return false;
    uint32_t read_sequence = load_u32_relaxed(&ring->read_sequence);
    uint32_t write_sequence = load_u32_acquire(&ring->write_sequence);
    if (read_sequence == write_sequence) return false;
    store_u32_release(&ring->read_sequence, read_sequence + 1U);
    return true;
}

size_t voice_uplink_ring_count(const voice_uplink_ring_t *ring)
{
    if (ring == NULL) return 0;
    uint32_t write_sequence = load_u32_acquire(&ring->write_sequence);
    uint32_t read_sequence = load_u32_acquire(&ring->read_sequence);
    uint32_t count = write_sequence - read_sequence;
    return count > (uint32_t)ring->capacity ? ring->capacity : (size_t)count;
}

uint32_t voice_uplink_ring_generation(const voice_uplink_ring_t *ring)
{
    return ring != NULL
               ? load_u32_acquire(&ring->active_generation)
               : 0U;
}

bool voice_uplink_ring_is_accepting(const voice_uplink_ring_t *ring)
{
    return ring != NULL &&
           load_bool_acquire(&ring->accepting);
}
