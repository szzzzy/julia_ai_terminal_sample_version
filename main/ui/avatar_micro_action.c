/*
 * 三层微动作系统的 ESP32-S3 + LVGL 编译入口。
 *
 * 具体实现保存在 avatar_micro_motion.c：
 * Layer 1: 多频呼吸、低频重心微晃、瞳孔目标/保持/缓动状态机；
 * Layer 2: 每 50ms 泊松判定、互斥动作调度、眨眼/wink/嘴角/哈欠/远望/肩膀；
 * Layer 3: boredom_ms 及 30s、60s、120s 三档行为变化。
 *
 * 该拆分让业务代码只包含 avatar_micro_action.h，同时保留项目内部的图层
 * 绑定和状态通知接口。实现不分配内存、不访问文件，单帧计算量为 O(1)。
 */
#include "avatar_micro_action.h"

