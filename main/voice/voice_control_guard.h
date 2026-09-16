#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include "cJSON.h"

#define VOICE_CONTROL_ID_BYTES 64U
#define VOICE_CONTROL_CACHE_SIZE 8U
#define VOICE_CONTROL_MAX_BYTES 768U
/* 在交给 cJSON 之前自检：限制解析栈深度、长度上限，并在任何 C 字符串比较之前
 * 拒绝内嵌 NUL 与重复 key。cJSON 本身接受这些输入。 */
cJSON *voice_control_parse(const char *data, size_t len);
/* 以下状态只在 WSS owner 内使用。缓存是 8 项环形覆盖：淘汰不会降低序号水位，
 * 旧命令在详细结果被回收后也不可能再执行一次。 */
typedef struct {
    char id[VOICE_CONTROL_ID_BYTES];
    char fingerprint[VOICE_CONTROL_MAX_BYTES + 1U];
    char response[512];
    uint32_t sequence;
} voice_control_result_t;
typedef struct {
    char interaction_id[VOICE_CONTROL_ID_BYTES];
    uint32_t interaction_seq;
    uint32_t high_water;
    bool active;
    unsigned used;
    unsigned next;
    voice_control_result_t results[VOICE_CONTROL_CACHE_SIZE];
} voice_control_guard_t;

/* 返回 NULL 表示合法的开轮请求，否则返回可对外回传的诊断串（不含敏感信息）。
 * 只有与当前对话无关的操作才允许 interaction=NULL。
 * 本函数只校验身份字段；命令序号由 evaluate() 负责。 */
const char *voice_control_guard_check(const voice_control_guard_t *guard,
    const cJSON *root, const char *device, const char *session, const char *interaction);
void voice_control_guard_reset(voice_control_guard_t *guard);
/* 取非负 JSON 整数：只接受数值类型且能无损转换为 uint32_t，不接受数字字符串。 */
bool voice_control_uint(const cJSON *root, const char *name, uint32_t *out);
/* 开新轮：只接受恰好下一轮序号，新轮清空结果缓存。同一轮同 interaction_id 重复开轮
 * 返回 duplicate_round（本轮仍 active）或 round_closed；interaction_seq 饱和则拒绝。 */
const char *voice_control_begin(voice_control_guard_t *guard, uint32_t seq, const char *id);
/* 判定一条 Scoped control：返回 NULL 表示需要执行；*cached 非 NULL 表示重复请求，
 * 直接回放原结果，不重新执行任何业务动作。低于 high_water 的旧序号返回
 * stale_request，跳到 high_water 之后又返回 out_of_order，轮次不符返回
 * stale_interaction；这三类都不会被执行。
 * now_ms 与载荷中的 expires_at_ms 都是 esp_timer 单调毫秒（开机起算），不是 UTC 墙钟；
 * 有效期窗口上限 10 秒，超出返回 invalid_deadline。 */
const char *voice_control_evaluate(const voice_control_guard_t *guard, const cJSON *root,
    const char *device, const char *session, int64_t now_ms, const char **cached);
/* 保存执行结果并把 high_water 提升到本序号；返回 false 表示结果无法保存。 */
bool voice_control_record(voice_control_guard_t *guard, const cJSON *root, const char *response);
