#pragma once

/**
 * @file    julia_memory.h
 * @brief   记忆与例行检测模块——Julia 的长期记忆对外接口。
 *
 * 职责概述（实现见 julia_memory.c）：
 *  - 事件日志：一条 `julia_event_t`（固定 96 字节、末尾带 CRC32）代表一次值得
 *    记住的事件，以环形覆盖方式保存在 SD 卡文件 events_v1.bin，最多保留
 *    EVENT_CAPACITY(=50) 条。
 *  - 用户画像（profile.json / NVS）：姓名、称呼、生日、喜好、备注。
 *  - 对话轮次：conversation_v2.jsonl 追加式日志 + summary.txt 最近对话摘要。
 *  - LLM prompt 组装：julia_memory_build_prompt() 把画像与摘要拼进系统提示。
 *  - 记忆维护：关键词回忆、遗忘（julia_memory_handle_forget / forget_all）。
 *  - 最后交互时间：julia_memory_last_interaction() 供 julia_context.c 判断长离隔。
 *
 * 存储介质与容量约束：
 *  - 长期数据在 SD 卡（/sdcard/julia/memory），启动前必须已完成 mount。
 *  - 画像/摘要/会话日志为“不限时长、有界大小”策略（见实现中的各上限宏）。
 *  - 事件日志是“环形覆盖”策略：写满后覆盖最旧的一条，不扩张文件。
 *
 * NOTE：本模块不存储音频或文本原文，仅保存裁剪后的摘要；敏感词会在写入前打码。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 情绪取值。0~4 为可表达的情绪，255(=JULIA_MEMORY_EMOTION_NONE) 特指“无情绪”。
 * 使用 255 而不是 0xFF 的实体值范围，便于与 uint8_t 状态“未设置”区分。 */
typedef enum {
    JULIA_MEMORY_EMOTION_CALM = 0,
    JULIA_MEMORY_EMOTION_HAPPY = 1,
    JULIA_MEMORY_EMOTION_SAD = 2,
    JULIA_MEMORY_EMOTION_EXCITED = 3,
    JULIA_MEMORY_EMOTION_TIRED = 4,
    JULIA_MEMORY_EMOTION_NONE = 255,
} julia_emotion_t;

/*
 * 事件记录：事件日志环形缓冲区的单条记录，同时是磁盘上的持久化布局。
 *
 * 字段语义：
 *  - timestamp：事件发生的“秒”。优先取 wall-clock（time(NULL)），若 RTC/SNTP
 *    尚未对齐（now < 1704067200，即 2024-01-01 00:00:00 UTC）则退化为
 *    uptime 秒（esp_timer_get_time()/1e6），因此同一条日志可能混用两种刻度，
 *    仅用于排序，不能当作绝对时刻。
 *  - type：事件类别（0=对话轮次、1=情绪事件、3=界面主题切换；2 暂未使用）。
 *  - emotion：julia_emotion_t；无情绪时通常为 NONE(255)。
 *  - reserved[2]：预留，暂未使用。
 *  - summary[64]：截断后的文本摘要；type==0 时额外截到 61 字节（见实现）。
 *  - storage_padding[20]：把结构体补到 96 字节；RAM 缓存用的 cached_event_t
 *    不含此字段（见实现），只有这里的正经 record/API 才是 96 字节。
 *  - crc32：对 crc32 字段之前所有字节做的 CRC32，用于校验完整性。
 *
 * 为什么固定 96 字节：_Static_assert 强制布局（含尾部 CRC 与填充），文件按
 * sizeof(julia_event_t) 定长读写，便于直接定位任意下标、损坏检测与未来扩展，
 * 而无需在每条记录里存长度。
 */
typedef struct {
    uint32_t timestamp;
    uint8_t type;
    uint8_t emotion;
    uint8_t reserved[2];
    char summary[64];
    uint8_t storage_padding[20];
    uint32_t crc32;
} julia_event_t;

_Static_assert(sizeof(julia_event_t) == 96, "julia_event_t must be 96 bytes");

/* 初始化记忆系统。前置条件：SD 卡已挂载。负责创建目录/文件、加载画像与摘要、
 * 清理过期会话日志、打开 NVS，并启动事件写入后台任务。必须在任何其它记忆函数
 * 之前调用；失败返回错误码。
 *
 * NOTE：当前工程中尚未看到对 julia_memory_init() 的调用点（见报告），
 *       若确未调用则事件日志/画像均不会工作，需结合启动流程确认。 */
esp_err_t julia_memory_init(void);
/* 组装给 LLM 的系统提示：把 base_prompt 加上用户画像(JSON)与最近对话摘要。
 * 输出写入 output，至少需要 output_size 字节。返回 ESP_OK / ESP_ERR_INVALID_ARG。
 * 调用上下文：语音对话任务（julia_voice.c），在发送 AI 请求前调用一次。 */
esp_err_t julia_memory_build_prompt(const char *base_prompt, char *output, size_t output_size);

/* 记录一次完整对话轮次：抽取画像、更新摘要、追加会话日志、更新 NVS 计数与
 * 最后交互时间，并写一条 type=0 的事件、通知例行检测。emotion 取 NONE。 */
esp_err_t julia_memory_record_turn(const char *user_text, const char *assistant_text);

/* 同上，但为该轮显式指定情绪值（julia_emotion_t / uint8_t 仍为 API 层面）。
 * 这是 record_turn 的实质实现。 */
esp_err_t julia_memory_record_turn_with_emotion(const char *user_text,
                                                const char *assistant_text,
                                                uint8_t emotion);

/* 处理“遗忘类”请求。按 text 是否含关键词区分遗忘范围：
 *  “所有/全部/清空记忆”→ 全清；“名字/称呼”→“生日”→“喜好/喜欢”→ 删除对应字段；
 *  其它 → 仅删除最近一条备注。命中时把答复写入 reply 并返回 true（调用方据此
 *  跳过 AI 请求）；未命中返回 false。 */
bool julia_memory_handle_forget(const char *user_text, char *reply, size_t reply_size);

/* 返回最后一次交互的 wall-clock 秒（0 表示尚未记录），供 julia_context.c 计算
 * 长离隔（LONG_ABSENCE_SECONDS=24h）。仅在 RTC/SNTP 对齐后才有意义。 */
int64_t julia_memory_last_interaction(void);

/* 入队一条事件。type 语义见 julia_event_t 注释（0/1/3，type>3 会被拒绝）。
 * summary 会被 UTF-8 安全截断。队列满时最多阻塞 500ms，超时返回
 * ESP_ERR_TIMEOUT（短暂背压比静默丢记忆更可靠）。由后台写入任务落盘。 */
esp_err_t julia_memory_append(uint8_t type, uint8_t emotion, const char *summary);

/* 取最近最多 n 条、且 CRC 校验通过的事件，按新的在前写入 out（最多 max 条），
 * 返回实际数量。从环形缓冲区当前 write_idx 向前回读。 */
int julia_memory_get_recent(int n, julia_event_t *out, int max);

/* 关键词回忆：遍历事件，把 summary 中含 kw 子串的（strstr）按新的在前写入 out，
 * 最多 max 条，返回命中数。匹配是“子串出现”而非分词/语义匹配。 */
int julia_memory_recall_keyword(const char *kw, julia_event_t *out, int max);

/* 把最近最多 4 条事件的 summary 连接成一行（用 "; " 分隔），供拼进 prompt。
 * 输出总长不超过 buf_size（内部再限 200 字节）。返回字符串长度。 */
int julia_memory_format_for_prompt(char *buf, int buf_size);

/* 清除全部事件：复位队列、清空内存缓存与头、删除事件文件。 */
esp_err_t julia_memory_forget_all(void);
