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
/* LC_IDLE_TIMEOUT 只用于本地：它不对应任何一段话语，因此从不进入上传报文。 */
typedef enum { LC_START, LC_AUDIO, LC_END, LC_ABORT, LC_IDLE_TIMEOUT } lc_event_t;
typedef struct {
    lc_event_t event;
    lc_mode_t mode;
    uint32_t id, index;
    double floor_dbfs, dbfs;
    bool limit;
    const int16_t *pcm;
} lc_record_t;
typedef bool (*lc_emit_t)(void *ctx, const lc_record_t *record);
typedef bool (*lc_voice_frame_t)(void *ctx, const int16_t *pcm, bool *speech);
typedef bool (*lc_voice_reset_t)(void *ctx);
/* 实验参数，不是语音分类器：字段含义为高频比（比例值，0～1，由
 * JULIA_CAPTURE_NOISE_RATIO_PERMILLE 换算）、谱质心上限（Hz）、能量/疑似噪声的统计窗口长度（帧）、
 * 窗口内能量帧下限（帧）、疑似噪声占能量帧的比例（百分比）、确认所需帧数（帧）。
 * 具体取值依据未确认，需按上板录音标定；只在 LC_OFF 状态下配置。 */
typedef struct {
    float high_ratio, centroid_hz;
    unsigned window_frames, min_energy_frames, noise_percent, confirm_frames;
} lc_noise_config_t;
typedef struct {
    uint8_t frames[25]; /* 0=安静或未知, 1=有能量, 2=疑似噪声 */
    unsigned head, count, energy, noise, confirm;
    bool dominant;
} lc_noise_window_t;
void lc_noise_reset(lc_noise_window_t *w);
/* 返回值表示“本帧是否不得提供活跃抵扣”（veto）。候选帧要求能量、VAD 和既有频谱门控同时
 * 通过，不只是“本帧不疑似噪声”；active 为真时还要满足连续确认帧数。 */
bool lc_noise_observe(lc_noise_window_t *w, const lc_noise_config_t *cfg,
                      bool energy, bool suspected, bool candidate, bool active);

/* 收尾迟滞只作用于段内结束判定：最近 5 帧（100 ms）内至少 4 帧为恢复候选才解除收尾；
 * 每次锁存的噪声收尾只有一次不可续期的 5 帧（100 ms）宽限。 */
#define LC_NOISE_RECOVERY_FRAMES 5U
#define LC_NOISE_RECOVERY_NEED 4U
#define LC_NOISE_GRACE_FRAMES 5U
typedef struct {
    bool latched, grace_used;
    unsigned candidate_bits, grace_remaining;
} lc_noise_tail_t;
typedef enum { LC_TAIL_NORMAL, LC_TAIL_COUNT, LC_TAIL_HOLD, LC_TAIL_RECOVER } lc_tail_action_t;
void lc_noise_tail_reset(lc_noise_tail_t *tail);
lc_tail_action_t lc_noise_tail_step(lc_noise_tail_t *tail, bool trigger, bool candidate);
typedef struct {
    lc_floor_t floor;
    lc_spectrum_t spectrum;
    /* 由采集 owner 在 LC_OFF 状态下设置；关闭时只保留能量判据（以及各自仍开启的门控）。 */
    bool fft_enabled;
    bool noise_gate_enabled;
    lc_noise_config_t noise_config;
    lc_noise_window_t noise_window;
    bool noise_tail_enabled;
    float noise_recovery_ratio, noise_recovery_centroid;
    unsigned noise_tail_end_guard_ms;
    lc_noise_tail_t noise_tail;
    unsigned noise_tail_entries, noise_tail_recoveries, noise_tail_hold_frames;
    lc_voice_frame_t voice_frame;
    lc_voice_reset_t voice_reset;
    void *voice_ctx;
    bool voice_reset_pending;
    unsigned voice_end_wake_ms, voice_end_dialog_ms;
    lc_mode_t mode;
    uint32_t next_id, id, frames;
    bool active, failed;
    unsigned silence, window_head, window_count, window_active; /* silence 单位为毫秒。 */
    /* 必须匹配 lc_process 中 LC_WAKE 的窗长 25：越界会写坏后面的预录缓冲。 */
    bool window[25]; /* 只保存“本帧是否活跃”的累积窗；wake 用满 25 格，dialog 只用前 15 格。 */
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
/** 回填一段已上传音频的电平：按原采集顺序、以 now 为终点每帧回调 20 ms 的时间戳入窗，最后更新两次，
 *  用于迟到的判决；两次更新的原因见 local_capture.c。 */
void lc_floor_feed_segment(lc_floor_t *floor, const double *db, unsigned count, int64_t now);
/** 返回第 10 百分位（dBFS）。会原地排序 values，调用后顺序不再保持。 */
double lc_percentile10(double *values, unsigned count);
void lc_init(local_capture_t *capture, lc_emit_t emit, void *ctx);
/* 绑定只在 LC_OFF 状态下由采集 owner 执行（或采音启动前）。VAD 是在能量/FFT 之外追加的
 * 判据，不参与音频数据的修改；返回 false 表示分类器本身出错，而不是“本帧不是语音”。
 * 两个结束时长单位为毫秒，且必须是 20 ms 的整数倍。 */
bool lc_set_voice_detector(local_capture_t *capture, lc_voice_frame_t frame,
                           lc_voice_reset_t reset, void *ctx,
                           unsigned wake_ms, unsigned dialog_ms);
/** 模式/连接切换丢弃预录并声明未完整段作废；底噪跨状态保留。 */
void lc_set_mode(local_capture_t *capture, lc_mode_t mode);
/** 输入必须恰为 320 个已应用板级增益的 PCM16 样本。ms 为单调采样时间轴的毫秒值。
 * 返回 false 表示 emit 拒绝或底噪运行中失败，调用方必须结束会话而不是跳过这一段。 */
bool lc_process(local_capture_t *capture, const int16_t *pcm, int64_t ms);
