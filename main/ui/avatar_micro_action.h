#pragma once

#include <stdint.h>

/*
 * 三层微动作系统的稳定对外接口。
 * update_avatar() 必须由主循环每 50ms 调用一次；语音、触摸或按键检测到
 * 用户交互时调用 on_user_interaction()，立即清空 boredom 并恢复清醒姿态。
 */
void update_avatar(uint32_t now_ms);
void on_user_interaction(void);

