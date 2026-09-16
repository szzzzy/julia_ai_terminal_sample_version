#include "local_capture.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 单位统一为 dBFS；调用方按毫秒时间轴传入，算法内不读挂钟。 */
double lc_rms_dbfs(const int16_t *pcm, size_t count)
{
    if (!count) return -120.0;    /* 与云端 float32 归一化输入一致；double 累加避免平台求和顺序放大误差。 */
    double sum = 0;
    for (size_t i = 0; i < count; ++i) {
        float x = pcm[i] / 32768.0f;
        sum += (double)(x * x);
    }
    double rms = sqrt(sum / count);
    return rms <= 1e-12 ? -120.0 : 20.0 * log10(rms);
}

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* 快窗、慢窗和噪声判决的重锚都用同一个分位实现，任何改动都会同时影响三条路径。 */
double lc_percentile10(double *v, unsigned n)
{
    if (!n) return -120.0;
    qsort(v, n, sizeof(*v), compare);
    /* numpy 默认 linear，不能改成向下取整的第 N 个元素。 */
    double pos = (n - 1) * 0.1;
    unsigned lo = (unsigned)pos, hi = lo + (lo + 1 < n);
    return v[lo] + (v[hi] - v[lo]) * (pos - lo);
}

/* 调用方提供 bg：上电初值 −60 dBFS，噪声判决重锚改用该段电平的 P10 并限幅。 */
void lc_floor_reset(lc_floor_t *f, double bg)
{
    memset(f, 0, sizeof(*f));
    f->bg = bg;
}
static void append(lc_floor_frame_t *v, unsigned *head, unsigned *count,
                   int64_t ms, double db)
{
    if (*count == LC_FLOOR_CAPACITY) {
        *head = (*head + 1) % LC_FLOOR_CAPACITY;
        --*count;
    }
    v[(*head + *count) % LC_FLOOR_CAPACITY] = (lc_floor_frame_t){ms, db};
    ++*count;
}

static double window_percentile(lc_floor_t *f, lc_floor_frame_t *v,
                                 unsigned *head, unsigned *count,
                                 int64_t ms, int64_t span)
{
    while (*count && ms - v[*head].ms > span) {
        *head = (*head + 1) % LC_FLOOR_CAPACITY;
        --*count;
    }
    for (unsigned i = 0; i < *count; ++i)
        f->scratch[i] = v[(*head + i) % LC_FLOOR_CAPACITY].db;
    return lc_percentile10(f->scratch, *count);
}

static double clamp(double db) { return fmin(-35.0, fmax(-80.0, db)); }

/* 按云端 NoiseFloorTracker 的对照参数计算快/慢窗分位：中断确认 1250 ms、快窗 1500 ms、
 * 慢窗 8000 ms、更新间隔 500 ms，单位为毫秒。 */
static void update(lc_floor_t *f, int64_t ms)
{
    if (f->updated && ms - f->last_update > 1250) f->rise = 0;
    f->last_update = ms;
    f->updated = true;
    double fast = window_percentile(f, f->fast, &f->fast_head, &f->fast_count, ms, 1500);
    double slow = window_percentile(f, f->slow, &f->slow_head, &f->slow_count, ms, 8000);
    if (f->fast_count >= 20 && fast > f->bg + 1.0) {
        if (f->rise < 2) ++f->rise;
        if (f->rise >= 2) f->bg = clamp(f->bg + fmin(fast - f->bg, 1.5));
    } else if (f->slow_count >= 50 && slow < f->bg) {
        f->rise = 0;
        if (slow < f->bg - 6.0) f->bg = fmax(slow, -80.0);
        else f->bg = clamp(f->bg - fmin(f->bg - slow, 0.25));
    } else f->rise = 0;
}

void lc_floor_frame(lc_floor_t *f, double db, bool frozen, int64_t ms)
{
    if (frozen || !isfinite(db) || db <= -100.0) return;
    append(f->fast, &f->fast_head, &f->fast_count, ms, db);
    if (db < f->bg + 12.0) append(f->slow, &f->slow_head, &f->slow_count, ms, db);
    if (!f->updated || ms - f->last_update >= 500) update(f, ms);
}

void lc_floor_feed_segment(lc_floor_t *f, const double *db, unsigned n, int64_t now)
{
    for (unsigned i = 0; i < n; ++i)
        lc_floor_frame(f, db[i], false, now - (int64_t)(n - i) * 20);
    /* 连续两次 update 是对齐云端 deque 回填的有意行为：第一次对齐窗口边界，
     * 第二次让上升确认计数生效。删除任何一次都会改变裁决后的底噪，不是笔误。 */
    update(f, now);
    update(f, now);
}
/* emit 返回 false 表示上层拒绝或发送失败：置 failed 后所有后续处理立即失败，
 * 由调用方结束本次会话，不静默丢音继续识别。 */
static bool emit(local_capture_t *c, lc_event_t event, const int16_t *pcm,
                 double db, bool limit)
{
    lc_record_t r = {event, c->mode, c->id, c->frames, c->frozen, db, limit, pcm};
    if (!c->emit(c->ctx, &r)) { c->failed = true; return false; }
    if (event == LC_AUDIO) ++c->frames;
    return true;
}

void lc_init(local_capture_t *c, lc_emit_t fn, void *ctx)
{
    memset(c, 0, sizeof(*c));
    /* 上电初值 −60 dBFS；之后由真实帧跟踪，判决定下的值同样受 −80～−35 dB 限制。 */
    lc_floor_reset(&c->floor, -60.0);
    c->emit = fn;
    c->ctx = ctx;
}

void lc_set_mode(local_capture_t *c, lc_mode_t mode)
{
    if (c->mode == mode) return;
    /* 未完成段先按 capture_abort 声明作废再清场，云端才不会把截断音频当完整段。 */
    if (c->active && !c->failed) (void)emit(c, LC_ABORT, NULL, 0, false);
    c->mode = mode;
    c->active = false;
    c->failed = false;
    c->pre_head = c->pre_count = c->window_head = c->window_count = c->window_active = 0;
    c->frames = c->silence = 0;
}

bool lc_process(local_capture_t *c, const int16_t *pcm, int64_t ms)
{
    if (c->failed) return false;
    if (c->mode == LC_OFF) return true;
    double db = lc_rms_dbfs(pcm, LC_FRAME_SAMPLES);
    /* 段内帧不参与底噪跟踪：起音时冻结的底噪在整个段内保持有效。 */
    lc_floor_frame(&c->floor, db, c->active, ms);
    if (c->active) {
        if (!emit(c, LC_AUDIO, pcm, db, false)) return false;
        /* 普通对话降低活跃抵扣量，减少间歇噪声拖延段尾；代价是对断续讲话的停顿容忍度下降。
         * 每帧 20 ms：普通对话抵扣 40 ms（1:2），唤醒候选保留 80 ms（1:4）。 */
        const unsigned active_credit_ms = c->mode == LC_DIALOG ? 40U : 80U;
        if (db > c->frozen + 3.0)
            c->silence = c->silence > active_credit_ms ? c->silence - active_credit_ms : 0;
        else c->silence += 20;
        /* 上限按已发送帧数计；唤醒段 400 帧/8 秒，普通段 750 帧/15 秒。 */
        bool limit = c->frames >= (c->mode == LC_WAKE ? 400U : LC_MAX_FRAMES);
        if (limit || c->silence >= (c->mode == LC_WAKE ? 500U : 700U)) {
            if (!emit(c, LC_END, NULL, db, limit)) return false;
            c->active = false;
            c->pre_count = c->window_count = c->window_active = c->window_head = 0;
        }
        return true;
    }
    /* 未起音时的帧只留在预录环里，不上传也不产生空段；wake/dialog 共用同一份预录。 */
    memcpy(c->preroll[c->pre_head], pcm, LC_FRAME_SAMPLES * sizeof(*pcm));
    c->preroll_db[c->pre_head] = db;
    c->pre_head = (c->pre_head + 1) % LC_PREROLL_FRAMES;
    if (c->pre_count < LC_PREROLL_FRAMES) ++c->pre_count;
    /* 累积窗等价于 500 ms（wake，25 帧）与 300 ms（dialog，15 帧），两项都必须 ≤ 结构体内
     * window[25] 的容量。 */
    unsigned window = c->mode == LC_WAKE ? 25U : 15U;
    bool active = db > c->floor.bg + (c->mode == LC_WAKE ? 3.0 : 9.0);
    if (c->window_count == window) c->window_active -= c->window[c->window_head];
    else ++c->window_count;
    c->window[c->window_head] = active;
    c->window_head = (c->window_head + 1) % window;
    c->window_active += active;
    /* 唤醒候选只要 20 ms 活跃音频，普通对话要求 300 ms 窗内累计 120 ms。 */
    if (c->window_active < (c->mode == LC_WAKE ? 1U : 6U)) return true;
    c->active = true;
    /* 话语编号从 1 开始，在未回绕区间内单调递增：next_id 每次自增，结果落到 0 时再自增一次，
     * 因此 0 从不作为编号使用（0 是“无效编号”的哨兵）。编号序列在 UINT32_MAX 处回绕到 1，
     * “编号越大越新”只在未回绕区间内成立。 */
    c->id = ++c->next_id;
    if (!c->id) c->id = ++c->next_id;
    /* 冻结当前底噪：整个段内的起音/延续/静音判定都用它，不再受后续跟踪影响。 */
    c->frozen = c->floor.bg;
    c->silence = c->frames = 0;
    if (!emit(c, LC_START, NULL, db, false)) return false;
    /* 预录按原采集顺序紧跟 start 补发，段内索引从 0 连续覆盖预录和实时帧。 */
    unsigned start = (c->pre_head + LC_PREROLL_FRAMES - c->pre_count) % LC_PREROLL_FRAMES;
    for (unsigned i = 0; i < c->pre_count; ++i) {
        unsigned p = (start + i) % LC_PREROLL_FRAMES;
        if (!emit(c, LC_AUDIO, c->preroll[p], c->preroll_db[p], false)) return false;
    }
    return true;
}
