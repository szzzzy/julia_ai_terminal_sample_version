/**
 * @file    julia_avatar.h
 * @brief   Julia L1 立绘层的公共接口（相位帧 + RMS 嘴型 + 微动）。
 *
 * 这是当前运行时实际生效的立绘链路接口（app_main 经 julia_avatar_init 启动）。
 * 上游主要是 voice_service（WSS 任务，驱动张嘴/说话起止/对话相位）与
 * julia_idle_display（dozing 睡眠/唤醒）。所有 LVGL 操作都在模块内部加锁，
 * 调用方无需（也不应）在调用前持有 lvgl_port_lock。详见 julia_avatar.c。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** The sole dialogue-phase definition shared by the voice and avatar layers. */
typedef enum {
    JULIA_AVATAR_DIALOG_IDLE = 0,
    JULIA_AVATAR_DIALOG_LISTENING,
    JULIA_AVATAR_DIALOG_THINKING,
    JULIA_AVATAR_DIALOG_SPEAKING,
} julia_avatar_dialog_phase_t;

/** Build the static Julia portrait and start the L1 micro-motion task. */
esp_err_t julia_avatar_init(void);

/** Mark the beginning/end of downlink speech. Safe before UI initialization. */
void julia_avatar_talking_start(void);
void julia_avatar_talking_stop(void);

/** Feed signed, mono 16-bit speaker PCM to the RMS mouth estimator. */
void julia_avatar_feed_pcm(const int16_t *samples, size_t sample_count);

/**
 * Set the voice dialogue phase.  This may be called from WSS and command
 * queue tasks; redundant changes are ignored and all LVGL work is locked.
 */
void julia_avatar_set_dialog_phase(julia_avatar_dialog_phase_t phase);

/** Switch the complete portrait to/from the generated sleep artwork. */
void julia_avatar_set_dozing(bool active);

/** Return the current dialogue phase without touching LVGL. */
julia_avatar_dialog_phase_t julia_avatar_get_dialog_phase(void);

bool julia_avatar_is_ready(void);
