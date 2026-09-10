#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* All mutations run on the WSS owner; readiness can be read from other tasks. */
void voice_state_sync_start(void);
void voice_state_sync_end(void);
void voice_state_sync_poll(void);
bool voice_state_sync_handle_text(const uint8_t *text, size_t len);
bool voice_state_sync_is_ready(void);
