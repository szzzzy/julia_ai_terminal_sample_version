#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PCM16 字节 FIFO。存储由调用方提供；本模块不含任何锁或 RTOS 调用，访问串行化
 * 完全由调用方负责。
 *
 * 不变量：
 *   - capacity 必须为偶数且 ≥ 2（pcm_buffer_init() 会把传入值向下取偶后存为
 *     buffer->capacity），storage 不得小于 capacity；容量为 0 时缓冲不可用，但也不会
 *     取模除零——write 会被 bytes > capacity - size 直接拒绝、read 会因 size == 0
 *     提前返回，取模语句到不了。
 *   - write 满时整块拒绝，绝不部分写入；返回 false 同时表示“满 / 参数非法 / 已结束”，
 *     调用方不能只看 false 区分原因。
 *   - end 只关闭输入，不清空已排队数据；finished 表示最后一帧已被取走；
 *     reset 清除 ended，使缓冲重新可写。
 *
 * 互斥要求：一对 read/write 之间必须互斥；end 和 reset 改变状态机的边界，
 * 同样必须与 write/read 互斥。典型做法见持有该缓冲的 voice_playback.c：
 * 读、写、end、reset 全部在同一个 mutex 内完成。 */
typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t size;
    bool ended;
} pcm_buffer_t;

void pcm_buffer_init(pcm_buffer_t *buffer, uint8_t *storage, size_t capacity);
void pcm_buffer_reset(pcm_buffer_t *buffer);
bool pcm_buffer_write(pcm_buffer_t *buffer, const void *data, size_t bytes);
size_t pcm_buffer_read(pcm_buffer_t *buffer, void *data, size_t capacity);
void pcm_buffer_end(pcm_buffer_t *buffer);
bool pcm_buffer_finished(const pcm_buffer_t *buffer);
