#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Byte FIFO for PCM16. The caller supplies storage and serializes access. */
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
