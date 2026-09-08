/**
 * @file julia_motion.h
 * @brief S6 运动唤醒输入；确认搬动后由 FSM 恢复到 S3。
 */
#pragma once

#include "esp_err.h"

/** 初始化共享 QMI8658 并启动运动检测；显示与背光仍由 FSM 独占。 */
esp_err_t julia_motion_init(void);
