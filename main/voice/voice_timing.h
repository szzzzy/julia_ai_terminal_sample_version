/**
 * @file voice_timing.h
 * @brief 会话时序诊断：把语音链路上的关键时点写进固定环形缓冲，再由诊断任务打印。
 *
 * 定位：只用于定位"首帧延迟/收尾时机/上行积压"这类端到端时序问题，**不参与任何业务判断**，
 * 也不改变采样、播放或协议行为。
 *
 * 线程模型：任何任务（采音、WSS owner、播放、FSM）都可以调用 record；写入在自旋锁内完成、
 * 不分配内存也不打印，因此不会阻塞实时路径。唯一的消费者是内部诊断任务（`take` 供主机测试
 * 单消费者读取）；放不下时只累计 dropped，绝不阻塞生产者。
 *
 * 时间基准：记录里的 `us` 是调用方传入的时刻，实际为 esp_timer 的单调微秒（启动以来）；
 * `boot` 为本次启动的标识。本模块不读取墙钟。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* 时序事件类型。命名分组：session_* 为 WSS 会话起止；capture_* 为本地分段；
 * enqueue_* / start_tx / end_tx / first_pcm_* / *_failed 为上行入队与发送；
 * uplink_stats 为积压统计；verdict 为云端判决；spks/first_pcm_rx/spke 为下行播放；
 * gate_state 与 tail_* 为分段门控与噪声收尾；recovery_shape/tail_guard 为恢复路径。
 * 具体字段含义随事件而定（见各调用点的 a/b 语义）。 */
typedef enum {
    VT_SESSION_START, VT_SESSION_END, VT_CAPTURE_START, VT_CAPTURE_END,
    VT_CAPTURE_ABORT, VT_ENQUEUE_FAILED, VT_ENQUEUE_END, VT_FIRST_PCM_QUEUED,
    VT_START_TX, VT_END_TX, VT_ABORT_TX, VT_FIRST_PCM_TX, VT_LAST_PCM_TX,
    VT_TX_FAILED, VT_UPLINK_STATS, VT_VERDICT, VT_SPKS, VT_FIRST_PCM_RX,
    VT_SPKE, VT_FIRST_I2S, VT_PLAY_DONE, VT_GATE_STATE,
    VT_TAIL_CONFIG, VT_TAIL_ENTER, VT_TAIL_RECOVER, VT_TAIL_SUMMARY,
    VT_RECOVERY_SHAPE, VT_TAIL_GUARD, VT_EVENT_COUNT
} voice_timing_kind_t;
/* 一条时序记录：boot=本次启动标识；seq=全局单调序号（含被丢弃的记录，因此可用于发现缺口）；
 * epoch=WSS generation；id=话语编号或播放代次；play=播放相关计数；dropped=写入时已丢弃条数；
 * kind=事件类型；us=事件时刻（µs，单调时钟）；a/b=事件相关数值，单位随事件而定。 */
typedef struct {
    uint32_t boot, seq, epoch, id, play, dropped;
    voice_timing_kind_t kind;
    int64_t us, a, b;
} voice_timing_record_t;
#if CONFIG_JULIA_VOICE_TIMING
/* 生产者调用前先 init 一次；记录只写入固定环形缓冲，不分配、不打印。
 * flush 由独立的低优先级诊断任务负责（绝不在采音/WSS/I2S 任务里打印）；
 * take 供单消费者读取，也是主机回归测试的读取接口。 */
void voice_timing_init(uint32_t boot);
void voice_timing_record(voice_timing_kind_t kind, uint32_t epoch, uint32_t id,
                         uint32_t play, int64_t us, int64_t a, int64_t b);
bool voice_timing_take(voice_timing_record_t *record);
const char *voice_timing_name(voice_timing_kind_t kind);
void voice_timing_flush(void);
#else
/* 关闭该配置时不编译实现：宏吃掉全部参数，保证诊断关闭时调用点也能通过编译。 */
#define voice_timing_init(...) ((void)0)
#define voice_timing_record(...) ((void)0)
#define voice_timing_flush(...) ((void)0)
#endif
