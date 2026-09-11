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

/**
 * 打开/关闭嘴型会话门控；断联本地提示和云端回答共用它。停止后迟到 PCM 不得
 * 重新张嘴。可从普通 Task 调用，内部负责状态锁和必要的 LVGL 串行访问。
 */
void julia_avatar_talking_start(void);
void julia_avatar_talking_stop(void);

/**
 * 提供已经写入扬声器的 mono PCM16；只同步计算能量，不保存输入指针。
 * 应从播放 owner 调用，网络收包阶段不得提前驱动嘴型。
 */
void julia_avatar_feed_pcm(const int16_t *samples, size_t sample_count);

/**
 * 设置当前交流阶段。重复设置同一阶段不会重新刷新；界面修改会在内部安全串行。
 */
void julia_avatar_set_dialog_phase(julia_avatar_dialog_phase_t phase);

/** 在普通立绘和完整闭眼休息画面之间切换。 */
void julia_avatar_set_dozing(bool active);

/** 查询当前交流画面阶段，不触发重绘。 */
julia_avatar_dialog_phase_t julia_avatar_get_dialog_phase(void);

/** 设置固定在屏幕左上侧的黑色小号状态叠字；UI 未初始化时先缓存。 */
void julia_avatar_set_status_text(const char *text);
/**
 * 设置与主状态正交的离线叠加层。UI 尚未初始化时缓存请求；函数内部串行 LVGL
 * 访问，调用方不得直接操作标签对象。主状态切换不会隐式清除该标志。
 */
void julia_avatar_set_offline(bool offline);

/**
 * 设置状态文字上方的常驻电量提示：正常显示黑色 BAT，低电量显示红色 LOW；
 * 未检测到有效电池电压时隐藏。百分比按5%取整，是带载电压的近似换算。
 */
void julia_avatar_set_battery_status(bool present, bool low, uint8_t percent);

/** 返回对象树与刷新 Task 均已创建的瞬时快照；不作为跨任务内存同步屏障。 */
bool julia_avatar_is_ready(void);
