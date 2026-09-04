/**
 * @file    julia_avatar.h
 * @brief   根据设备是否在听、等待回答或说话，更新 Julia 的眼睛、嘴型和休息画面。
 *
 * 语音服务只说明当前交流阶段并提供已播放声音；本模块把这些信息转换为表情。
 * 睡眠策略可切换到闭眼画面。界面同步由模块内部完成，调用方不需要操作 LVGL 锁。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** 用户能够观察到的四种交流画面阶段。 */
typedef enum {
    JULIA_AVATAR_DIALOG_IDLE = 0,
    JULIA_AVATAR_DIALOG_LISTENING,
    JULIA_AVATAR_DIALOG_THINKING,
    JULIA_AVATAR_DIALOG_SPEAKING,
} julia_avatar_dialog_phase_t;

/** 创建 Julia 立绘并启动眨眼、呼吸和嘴型更新。 */
esp_err_t julia_avatar_init(void);

/**
 * 播放一次开机睁眼与快速眨眼。重复调用不会重复创建对象；结束后保持清醒立绘，
 * 再进入普通随机眨眼。
 */
esp_err_t julia_avatar_play_boot_sequence(void);

/** 标记回答声音开始或结束，使嘴型只在设备实际说话时活动。 */
void julia_avatar_talking_start(void);
void julia_avatar_talking_stop(void);

/** 提供已经送往扬声器的单声道声音，用实际音量选择嘴巴张开程度。 */
void julia_avatar_feed_pcm(const int16_t *samples, size_t sample_count);

/**
 * 设置当前交流阶段。重复设置同一阶段不会重新刷新；界面修改会在内部安全串行。
 */
void julia_avatar_set_dialog_phase(julia_avatar_dialog_phase_t phase);

/** 在普通立绘和完整闭眼休息画面之间切换。 */
void julia_avatar_set_dozing(bool active);

/**
 * 显示由 doze_frame_preview.png 生成的连接中断立绘，并隐藏独立眼睛和嘴巴图层。
 * 三秒后状态管理进入待机时会自动换回普通待机画面。
 */
void julia_avatar_show_disconnected(void);

/** 查询当前交流画面阶段，不触发重绘。 */
julia_avatar_dialog_phase_t julia_avatar_get_dialog_phase(void);

/** 设置固定在屏幕左上侧的黑色小号状态叠字；UI 未初始化时先缓存。 */
void julia_avatar_set_status_text(const char *text);

bool julia_avatar_is_ready(void);
