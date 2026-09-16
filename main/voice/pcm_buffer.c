#include "pcm_buffer.h"

#include <string.h>

/* 前置不变量：容量向下取偶（保证 16 位样本不会被拆开），调用方必须保证 storage 不小于
 * capacity；容量取偶后为 0 时缓冲只是不可用，不会取模除零——write 会因
 * bytes > capacity - size 直接拒绝，read 也会因 size == 0 提前返回。 */
void pcm_buffer_init(pcm_buffer_t *buffer, uint8_t *storage, size_t capacity)
{
    *buffer = (pcm_buffer_t){.data = storage, .capacity = capacity & ~(size_t)1};
}

void pcm_buffer_reset(pcm_buffer_t *buffer)
{
    buffer->head = 0;
    buffer->size = 0;
    buffer->ended = false;
}

bool pcm_buffer_write(pcm_buffer_t *buffer, const void *data, size_t bytes)
{
    if (data == NULL || bytes == 0 || (bytes & 1U) || buffer->ended ||
        buffer->data == NULL || bytes > buffer->capacity - buffer->size) {
        return false;
    }
    size_t tail = (buffer->head + buffer->size) % buffer->capacity;
    size_t first = buffer->capacity - tail;
    if (first > bytes) first = bytes;
    memcpy(buffer->data + tail, data, first);
    memcpy(buffer->data, (const uint8_t *)data + first, bytes - first);
    buffer->size += bytes;
    return true;
}

size_t pcm_buffer_read(pcm_buffer_t *buffer, void *data, size_t capacity)
{
    if (data == NULL || buffer->size == 0) return 0;
    size_t bytes = capacity & ~(size_t)1;
    if (bytes > buffer->size) bytes = buffer->size;
    size_t first = buffer->capacity - buffer->head;
    if (first > bytes) first = bytes;
    memcpy(data, buffer->data + buffer->head, first);
    memcpy((uint8_t *)data + first, buffer->data, bytes - first);
    buffer->head = (buffer->head + bytes) % buffer->capacity;
    buffer->size -= bytes;
    return bytes;
}

void pcm_buffer_end(pcm_buffer_t *buffer) { buffer->ended = true; }
bool pcm_buffer_finished(const pcm_buffer_t *buffer) { return buffer->ended && buffer->size == 0; }
