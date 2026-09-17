#pragma once
#include <stdint.h>
typedef enum { VAD_MODE_0, VAD_MODE_1, VAD_MODE_2, VAD_MODE_3, VAD_MODE_4 } vad_mode_t;
typedef void *vad_handle_t;
vad_handle_t vad_create(vad_mode_t mode);
void vad_destroy(vad_handle_t handle);
