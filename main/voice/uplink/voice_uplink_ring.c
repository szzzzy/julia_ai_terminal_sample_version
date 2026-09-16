/**
 * @file voice_uplink_ring.c
 * @brief 音频上行 SPSC 环形缓冲实现。
 *
 * 唯一 producer 是板级采音任务，唯一 consumer 是 WSS owner（会话任务）。跨任务的同步
 * 点只有槽内容与写序号（producer 先写槽、再以 release 发布写序号；consumer 先 acquire
 * 读序号、再取槽内容）、读序号（consumer 发布，producer 用它判满），以及 owner 发布的
 * 代次与 accepting 标志。任何绕过该顺序的写入都会让 consumer 读到尚未完成的内容。
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

/* 以 read=write 一次性丢弃全部已发布但未发送的内容。只能在 accepting 关闭后由 owner
 * 调用：此后 producer 的新一轮发布会被入口校验挡回。注意该动作不覆盖“正在发布中”的
 * 那一次：producer 可能已通过发布前校验，在入口关闭之后才完成写序号发布，因此丢弃完成
 * 后仍可能出现一格属于旧代次的已发布槽。consumer 的代次过滤不能省，靠它跳过该槽。 */
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
    /* 换代顺序固定为「关入口 → 丢弃旧内容 → 写新代次 → 开入口」，不可调整：先开入口会
     * 让 producer 把新帧写进尚未清理的槽，先写代次会让旧内容被当成当前代次。丢弃只清理
     * 已发布的槽，正在发布中的那一格由 consumer 按代次跳过。 */
    store_bool_release(&ring->accepting, false);
    discard_published(ring);
    store_u32_release(&ring->active_generation, generation);
    store_bool_release(&ring->accepting, true);
    return true;
}

void voice_uplink_ring_stop_generation(voice_uplink_ring_t *ring)
{
    if (ring == NULL) return;
    voice_uplink_ring_close_generation(ring);
    discard_published(ring);
}

void voice_uplink_ring_close_generation(voice_uplink_ring_t *ring)
{
    if (ring == NULL) return;
    store_bool_release(&ring->accepting, false);
}

voice_uplink_push_result_t voice_uplink_ring_push(voice_uplink_ring_t *ring,
                                                   const uint8_t *data,
                                                   size_t len)
{
    return voice_uplink_ring_push_generation(ring, data, len, 0);
}

voice_uplink_push_result_t voice_uplink_ring_push_generation(voice_uplink_ring_t *ring,
    const uint8_t *data, size_t len, uint32_t expected_generation)
{
    if (ring == NULL || data == NULL || len == 0U ||
        len > ring->frame_capacity) {
        return VOICE_UPLINK_PUSH_INVALID;
    }
    if (!load_bool_acquire(&ring->accepting)) {
        return VOICE_UPLINK_PUSH_INACTIVE;
    }

    uint32_t generation = load_u32_acquire(&ring->active_generation);
    if (expected_generation && generation != expected_generation)
        return VOICE_UPLINK_PUSH_INACTIVE;
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

    /* owner 可能在 memcpy 期间结束会话。发布前再校验；若 owner 恰好在本次校验之后关闭
     * 入口，本帧仍会被发布出去，但它携带的是旧 generation，新连接 consumer 会识别并永久
     * 跳过——这次“最后一次发布”正是代次过滤不能省的原因。 */
    publish_fence();
    if (!load_bool_acquire(&ring->accepting) ||
        load_u32_acquire(&ring->active_generation) != generation) {
        return VOICE_UPLINK_PUSH_INACTIVE;
    }
    /* 先写完槽内容再以 release 发布写序号：consumer 只在 acquire 到新序号后才读槽，
     * 这一步是内容可见性的前提。 */
    store_u32_release(&ring->write_sequence, write_sequence + 1U);
    return VOICE_UPLINK_PUSH_OK;
}

bool voice_uplink_ring_peek(voice_uplink_ring_t *ring,
                            uint32_t expected_generation,
                            const uint8_t **data, size_t *len)
{
    /* expected_generation 为 0 直接判非法，与 push 的「0 = 沿用当前代次」语义相反。 */
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
            /* 非当前代次或已损坏的槽直接推进读指针：属于永久丢弃，不是保留跳过。 */
            store_u32_release(&ring->read_sequence, read_sequence + 1U);
            continue;
        }
        /* 返回 ring 内部存储地址，调用方必须在下一次 consume 之前用完。 */
        *data = ring->storage + slot * ring->frame_capacity;
        *len = slot_len;
        return true;
    }
}

bool voice_uplink_ring_consume(voice_uplink_ring_t *ring)
{
    /* 与紧邻的一次成功 peek 一一配对：只推进一格，代表那一块已整块发送成功；
     * 发送失败时调用会永久丢掉尚未送出的声音。 */
    if (ring == NULL) return false;
    uint32_t read_sequence = load_u32_relaxed(&ring->read_sequence);
    uint32_t write_sequence = load_u32_acquire(&ring->write_sequence);
    if (read_sequence == write_sequence) return false;
    store_u32_release(&ring->read_sequence, read_sequence + 1U);
    return true;
}

size_t voice_uplink_ring_count(const voice_uplink_ring_t *ring)
{
    /* 写序号与读序号是两次独立原子读，并发下结果只是近似积压量，不能当精确深度用。 */
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
