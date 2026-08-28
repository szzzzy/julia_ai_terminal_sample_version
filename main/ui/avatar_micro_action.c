/**
 * @file    avatar_micro_action.c
 * @brief   三层微动作系统的“编译入口”（唯一实现体在 avatar_micro_motion.c）。
 *
 * 职责：本编译单元只提供“微动作”对外接口的最小化入口，自身不含任何逻辑；
 * 所有实现都在 avatar_micro_motion.c（见该文件头）。业务代码只要包含
 * avatar_micro_action.h 即可调用 update_avatar/on_user_interaction。
 *
 * 说明（L2/L3 残留标注，不改不删）：
 *   - 本文档按 fused 完整版描述了 Layer 1/2/3（多频呼吸+重心微晃+瞳孔状态机 /
 *     ~50ms 泊松互斥动作调度 / boredom 三档行为）。
 *   - 依 docs/UI_L0L1_PORT.md，L0/L1 只保留“立绘 + RMS 嘴型 + 眨眼/呼吸/微动”；
 *     Layer 2（眨眼/wink/嘴角/哈欠/远望/肩膀的泊松互斥调度）与 Layer 3（boredom 分级）
 *     在本移植中**未实现**。下方描述仅是残留说明，供追溯，不要据此断言存在对应行为。
 */
#include "avatar_micro_action.h"

