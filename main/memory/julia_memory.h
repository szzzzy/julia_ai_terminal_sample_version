#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    JULIA_MEMORY_EMOTION_CALM = 0,
    JULIA_MEMORY_EMOTION_HAPPY = 1,
    JULIA_MEMORY_EMOTION_SAD = 2,
    JULIA_MEMORY_EMOTION_EXCITED = 3,
    JULIA_MEMORY_EMOTION_TIRED = 4,
    JULIA_MEMORY_EMOTION_NONE = 255,
} julia_emotion_t;

typedef struct {
    uint32_t timestamp;
    uint8_t type;
    uint8_t emotion;
    uint8_t reserved[2];
    char summary[64];
    uint8_t storage_padding[20];
    uint32_t crc32;
} julia_event_t;

_Static_assert(sizeof(julia_event_t) == 96, "julia_event_t must be 96 bytes");

esp_err_t julia_memory_init(void);
esp_err_t julia_memory_build_prompt(const char *base_prompt, char *output, size_t output_size);
esp_err_t julia_memory_record_turn(const char *user_text, const char *assistant_text);
esp_err_t julia_memory_record_turn_with_emotion(const char *user_text,
                                                const char *assistant_text,
                                                uint8_t emotion);
bool julia_memory_handle_forget(const char *user_text, char *reply, size_t reply_size);
int64_t julia_memory_last_interaction(void);

esp_err_t julia_memory_append(uint8_t type, uint8_t emotion, const char *summary);
int julia_memory_get_recent(int n, julia_event_t *out, int max);
int julia_memory_recall_keyword(const char *kw, julia_event_t *out, int max);
int julia_memory_format_for_prompt(char *buf, int buf_size);
esp_err_t julia_memory_forget_all(void);
