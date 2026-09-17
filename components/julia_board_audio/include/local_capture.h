/** 16 kHz / 20 ms 的帧级电平统计与分段判决：维护动态底噪，决定一段话语的起音、结束和上限，
 * 并把预录、实时帧和 end 交给 emit 回调。
 *
 * 本模块不负责网络发送、任务与内存分配，也不做设备级增益：调用方必须传入恰好
 * LC_FRAME_SAMPLES 个样本，并把 `CONFIG_JULIA_MIC_GAIN_PERCENT` 数字增益应用在送入之前，
 * 否则底噪阈值与云端电平对不上。
 *
 * 底噪参数对应线上 2026-09-15 NoiseFloorTracker，时间单位为毫秒。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lc_spectrum.h"

#define LC_FRAME_SAMPLES 320U
/* 正常 8 秒窗只有 401 帧；留足四段迟到回填的临时重叠，避免改变云端 deque 统计。 */
#define LC_FLOOR_CAPACITY 4096U
/* 20 ms/帧，合计 500 ms；预录随 start 之前的帧一起进入段内索引。 */
#define LC_PREROLL_FRAMES 25U
/* 750 帧/15 秒是上传段的帧数上限；实际长度还会被 mode 相关的上限和静音计数提前截断。 */
#define LC_MAX_FRAMES 750U
typedef struct { int64_t ms; double db; } lc_floor_frame_t;
typedef struct {
    double bg; /* 当前动态底噪估计（dBFS），取值限制在 −80～−35 dB。 */
    lc_floor_frame_t fast[LC_FLOOR_CAPACITY], slow[LC_FLOOR_CAPACITY];
    unsigned fast_head, fast_count, slow_head, slow_count, rise;
    int64_t last_update;
    bool updated;
    double scratch[LC_FLOOR_CAPACITY];
} lc_floor_t;
typedef enum { LC_OFF, LC_WAKE, LC_DIALOG } lc_mode_t;
typedef enum { LC_START, LC_AUDIO, LC_END, LC_ABORT } lc_event_t;
typedef struct {
    lc_event_t event;
    lc_mode_t mode;
    uint32_t id, index;
    double floor_dbfs, dbfs;
    bool limit;
    const int16_t *pcm;
} lc_record_t;
typedef bool (*lc_emit_t)(void *ctx, const lc_record_t *record);
typedef struct {
    lc_floor_t floor;
    lc_spectrum_t spectrum;
    /* Set only by capture owner while LC_OFF. Off preserves energy-only behavior. */
    bool fft_enabled;
    lc_mode_t mode;
    uint32_t next_id, id, frames;
    bool active, failed;
    unsigned silence, window_head, window_count, window_active; /* silence 单位为毫秒。 */
    /* 必须匹配 lc_process 中 LC_WAKE 的窗长 25：越界会写坏后面的预录缓冲。 */
    bool window[25];
    int16_t preroll[LC_PREROLL_FRAMES][LC_FRAME_SAMPLES];
    double preroll_db[LC_PREROLL_FRAMES];
    unsigned pre_head, pre_count;
    double frozen;
    lc_emit_t emit;
    void *ctx;
} local_capture_t;

/** 返回 count 个样本的 RMS 电平，单位 dBFS；全零输入返回 −120 dBFS。 */
double lc_rms_dbfs(const int16_t *pcm, size_t count);
/** 把底噪直接设为 bg。噪声判决的重锚在调用前已把 bg 限制在 −80～−35 dB。 */
void lc_floor_reset(lc_floor_t *floor, double bg);
/** 向快/慢窗加入一帧电平（dBFS）；frozen 为真表示该帧已属于起音段，不再参与底噪跟踪。 */
void lc_floor_frame(lc_floor_t *floor, double db, bool frozen, int64_t ms);
/** 按原采集顺序回填一段已上传音频的电平并立即更新，用于迟到的判决。 */
void lc_floor_feed_segment(lc_floor_t *floor, const double *db, unsigned count, int64_t now);
/** 返回第 10 百分位（dBFS）。会原地排序 values，调用后顺序不再保持。 */
double lc_percentile10(double *values, unsigned count);
void lc_init(local_capture_t *capture, lc_emit_t emit, void *ctx);
/** 模式/连接切换丢弃预录并声明未完整段作废；底噪跨状态保留。 */
void lc_set_mode(local_capture_t *capture, lc_mode_t mode);
/** 输入必须恰为 320 个已应用板级增益的 PCM16 样本。ms 为单调采样时间轴的毫秒值。
 * 返回 false 表示 emit 拒绝或底噪运行中失败，调用方必须结束会话而不是跳过这一段。 */
bool lc_process(local_capture_t *capture, const int16_t *pcm, int64_t ms);
