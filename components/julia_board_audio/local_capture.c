/* 本文件负责 20 ms 帧级判决与动态底噪跟踪：只维护音频判决状态，不做网络 I/O、任务和
 * 内存分配，所有阈值都以 dBFS 相对底噪、毫秒或帧为单位，时间来自调用方传入的采样时间轴。 */
/* 门控构成：能量相对底噪的差值、可选的 WebRTC VAD、可选的 320 点频谱包络（lc_spectrum.c）、
 * 实验性的底噪双窗口与收尾迟滞（本文件）。四者共用同一帧，先判能量再决定是否继续算特征，
 * 起音与段内活跃对缺失特征的处理并不相同，改动顺序会改变判决结果。 */
/* 门控开关来自 Kconfig：CONFIG_JULIA_CAPTURE_VAD_ENABLE 绑定 VAD、
 * CONFIG_JULIA_CAPTURE_FFT_GATE 打开频谱门控、CONFIG_JULIA_CAPTURE_NOISE_WINDOW 打开本文件的
 * 实验性底噪窗口、CONFIG_JULIA_CAPTURE_NOISE_TAIL 追加 dialog 收尾迟滞。逐项关闭即回到上一层
 * 行为；全部关闭时本文件只保留能量与静音计数。 */
#include "local_capture.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

void lc_noise_reset(lc_noise_window_t *w) { memset(w, 0, sizeof(*w)); }

/* 收尾状态在段结束、模式切换和 VAD 复位时清空；latched 之后不会因窗口变安静自动解除，
 * 只有 4/5 帧恢复条件或显式 reset 才会退出。 */
void lc_noise_tail_reset(lc_noise_tail_t *tail) { memset(tail, 0, sizeof(*tail)); }

lc_tail_action_t lc_noise_tail_step(lc_noise_tail_t *tail, bool trigger, bool candidate)
{
    /* trigger 为真表示本帧被判为噪声占优且尚未进入收尾；进入时先清空旧状态，
     * 使“宽限期只用一次”的约束以整段收尾为单位成立。 */
    if (!tail->latched) {
        if (!trigger) return LC_TAIL_NORMAL;
        lc_noise_tail_reset(tail);
        tail->latched = true;
    }
    tail->candidate_bits = ((tail->candidate_bits << 1) | (candidate ? 1U : 0U)) &
                           ((1U << LC_NOISE_RECOVERY_FRAMES) - 1U);
    unsigned candidates = 0;
    for (unsigned bits = tail->candidate_bits; bits; bits >>= 1) candidates += bits & 1U;
    /* 候选帧计数只看最近 5 帧里的 1 比特数量，达到 4 帧立即恢复并清空收尾状态；
     * 恢复后若再进入收尾，候选计数会由调用方重新累积。 */
    if (candidates >= LC_NOISE_RECOVERY_NEED) {
        lc_noise_tail_reset(tail);
        return LC_TAIL_RECOVER;
    }
    if (candidate && !tail->grace_used) {
        tail->grace_used = true;
        tail->grace_remaining = LC_NOISE_GRACE_FRAMES;
    }
    /* 宽限期从首次出现恢复候选帧开始，此后每帧都递减，孤立的候选帧不能续期；
     * 宽限期耗尽只结束暂缓，之后仍允许按 4/5 帧条件恢复。 */
    if (tail->grace_remaining) {
        --tail->grace_remaining;
        return LC_TAIL_HOLD;
    }
    return LC_TAIL_COUNT;
}

bool lc_noise_observe(lc_noise_window_t *w, const lc_noise_config_t *cfg,
                      bool energy, bool suspected, bool candidate, bool active)
{
    /* 实验配置非法时按“放行”处理：重置窗口后返回 false（不产生 veto），PCM、预录和分段
     * 逻辑都不受影响；window_frames 上限 25 同时也是 lc_noise_window_t::frames 的容量。 */
    if (!isfinite(cfg->high_ratio) || cfg->high_ratio < 0 || cfg->high_ratio > 1 ||
        !isfinite(cfg->centroid_hz) || cfg->centroid_hz < 0 || cfg->centroid_hz > 8000 ||
        !cfg->window_frames || cfg->window_frames > 25 ||
        !cfg->min_energy_frames || cfg->min_energy_frames > cfg->window_frames ||
        !cfg->noise_percent || cfg->noise_percent > 100 || !cfg->confirm_frames) {
        lc_noise_reset(w);
        return false;
    }
    if (w->count == cfg->window_frames) {
        unsigned old = w->frames[w->head];
        w->energy -= old != 0; w->noise -= old == 2;
    } else ++w->count;
    unsigned value = energy ? (suspected ? 2U : 1U) : 0U;
    w->frames[w->head] = value;
    w->head = (w->head + 1) % cfg->window_frames;
    w->energy += value != 0; w->noise += value == 2;
    bool dominant = w->energy >= cfg->min_energy_frames &&
                    w->noise * 100 >= w->energy * cfg->noise_percent;
    /* 出现新的候选帧时立即恢复原有的抵扣机会。候选帧本身不构成人声证据，
     * 因此任何情况下都不得据此取消整段。 */
    if (!dominant || candidate) w->confirm = 0;
    else if (w->confirm < cfg->confirm_frames) ++w->confirm;
    /* dominant 是“本帧是否判为噪声占优”：候选帧只撤销本帧的判噪资格，不清空窗口历史，
     * 因此候选帧之后仍可能在旧证据上重新进入占优状态。 */
    w->dominant = dominant && !candidate;
    /* 返回 true 只表示“本帧不得提供活跃抵扣”，即 veto；返回 false 不改变任何既有状态，
     * 起音前的调用方因此在被 veto 时提前返回，段内的调用方则继续按原规则累计静音。 */
    return energy && suspected && w->dominant &&
           (!active || w->confirm >= cfg->confirm_frames);
}

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
/* 三档步进同样是 dBFS：上升每次最多 +1.5 dB 且需连续两次确认（对应 2×500 ms），
 * 下降每次最多 −0.25 dB，慢窗分位低于 bg−6 dB 时直接重锚。各门限的具体取值依据未确认。 */
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
    /* frozen 表示该帧已属于起音段：整个段内都不再向快/慢窗加入帧，底噪保持起音时冻结的值；
     * ≤−100 dBFS 视为“无环境信息”，同样不入窗。 */
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
    /* 噪声窗口默认值按字段顺序为：高频比 0.6（per mille 600）、质心 1800 Hz、
     * 窗口 15 帧、能量帧下限 5、疑似噪声占比 80%、确认 5 帧；以下 CONFIG_JULIA_CAPTURE_NOISE_*
     * 逐项覆盖。这些取值属于实验候选，需上板标定，依据未确认。 */
    c->noise_config = (lc_noise_config_t){0.6f, 1800.0f, 15, 5, 80, 5};
#ifdef CONFIG_JULIA_CAPTURE_NOISE_TAIL
    c->noise_tail_enabled = CONFIG_JULIA_CAPTURE_NOISE_TAIL;
#elif !defined(ESP_PLATFORM)
    c->noise_tail_enabled = true; /* 主机测试单独选择该门控，固件上未配置时保持关闭。 */
#endif
#ifdef CONFIG_JULIA_CAPTURE_NOISE_WINDOW
    c->noise_gate_enabled = CONFIG_JULIA_CAPTURE_NOISE_WINDOW;
#endif
#ifdef CONFIG_JULIA_CAPTURE_NOISE_RATIO_PERMILLE
    c->noise_config.high_ratio = CONFIG_JULIA_CAPTURE_NOISE_RATIO_PERMILLE / 1000.0f;
#endif
#ifdef CONFIG_JULIA_CAPTURE_NOISE_CENTROID_HZ
    c->noise_config.centroid_hz = CONFIG_JULIA_CAPTURE_NOISE_CENTROID_HZ;
#endif
#ifdef CONFIG_JULIA_CAPTURE_NOISE_WINDOW_FRAMES
    c->noise_config.window_frames = CONFIG_JULIA_CAPTURE_NOISE_WINDOW_FRAMES;
#endif
#ifdef CONFIG_JULIA_CAPTURE_NOISE_MIN_ENERGY_FRAMES
    c->noise_config.min_energy_frames = CONFIG_JULIA_CAPTURE_NOISE_MIN_ENERGY_FRAMES;
#endif
#ifdef CONFIG_JULIA_CAPTURE_NOISE_PERCENT
    c->noise_config.noise_percent = CONFIG_JULIA_CAPTURE_NOISE_PERCENT;
#endif
#ifdef CONFIG_JULIA_CAPTURE_NOISE_CONFIRM_FRAMES
    c->noise_config.confirm_frames = CONFIG_JULIA_CAPTURE_NOISE_CONFIRM_FRAMES;
#endif
    c->noise_recovery_ratio = c->noise_config.high_ratio;
    c->noise_recovery_centroid = c->noise_config.centroid_hz;
    /* 恢复门限默认继承起音门限，再由 JULIA_CAPTURE_NOISE_RECOVERY_* 单独收紧；两者走的是
     * 不同的谱条件组合（见 lc_process），因此不能用同一个常量表达。 */
#ifdef CONFIG_JULIA_CAPTURE_NOISE_RECOVERY_RATIO_PERMILLE
    c->noise_recovery_ratio = CONFIG_JULIA_CAPTURE_NOISE_RECOVERY_RATIO_PERMILLE / 1000.0f;
#endif
#ifdef CONFIG_JULIA_CAPTURE_NOISE_RECOVERY_CENTROID_HZ
    c->noise_recovery_centroid = CONFIG_JULIA_CAPTURE_NOISE_RECOVERY_CENTROID_HZ;
#endif
#ifdef CONFIG_JULIA_CAPTURE_NOISE_END_GUARD_MS
    /* 尾部保护向上取整到 20 ms 帧边界：静音计数只在整帧上推进，非整数毫秒无法生效。 */
    c->noise_tail_end_guard_ms = ((CONFIG_JULIA_CAPTURE_NOISE_END_GUARD_MS + 19) / 20) * 20;
#endif
#ifdef CONFIG_JULIA_CAPTURE_FFT_GATE
    c->fft_enabled = CONFIG_JULIA_CAPTURE_FFT_GATE;
#endif
    /* 开启 FFT 门控时初始化失败属于致命错误：后续帧无法计算特征，而放行会把整个门控静默关掉。 */
    if (!lc_spectrum_init(&c->spectrum) && c->fft_enabled) c->failed = true;
}

bool lc_set_voice_detector(local_capture_t *c, lc_voice_frame_t frame,
                           lc_voice_reset_t reset, void *ctx,
                           unsigned wake_ms, unsigned dialog_ms)
{
    /* VAD 只在 OFF 状态下绑定，避免中途换实现导致当前段的判据前后不一致；
     * 两个结束时长必须是 20 ms 的整数倍，才能直接和帧级静音计数比较。 */
    if (c->mode != LC_OFF || !frame || !reset || !wake_ms || !dialog_ms ||
        wake_ms % 20 || dialog_ms % 20) return false;
    c->voice_frame = frame; c->voice_reset = reset; c->voice_ctx = ctx;
    c->voice_end_wake_ms = wake_ms; c->voice_end_dialog_ms = dialog_ms;
    c->voice_reset_pending = true;
    return true;
}

void lc_set_mode(local_capture_t *c, lc_mode_t mode)
{
    if (c->mode == mode) return;
    /* 未完成段先按 capture_abort 声明作废再清场，云端才不会把截断音频当完整段。 */
    if (c->active && !c->failed) (void)emit(c, LC_ABORT, NULL, 0, false);
    c->mode = mode;
    c->voice_reset_pending = true;
    c->active = false;
    c->failed = false;
    c->pre_head = c->pre_count = c->window_head = c->window_count = c->window_active = 0;
    c->frames = c->silence = 0;
    /* 模式切换也清空噪声窗口与收尾状态：上一模式的噪声证据不属于新模式，VAD 侧同样
     * 通过 voice_reset_pending 复位；底噪（floor）跨状态保留，不在这里重置。 */
    lc_noise_reset(&c->noise_window);
    lc_noise_tail_reset(&c->noise_tail);
    c->noise_tail_entries = c->noise_tail_recoveries = c->noise_tail_hold_frames = 0;
}

bool lc_process(local_capture_t *c, const int16_t *pcm, int64_t ms)
{
    if (c->failed) return false;
    if (c->mode == LC_OFF) return true;
    bool speech = true;
    if (c->voice_frame) {
        if (c->voice_reset_pending) {
            lc_noise_reset(&c->noise_window);
            lc_noise_tail_reset(&c->noise_tail);
            if (!c->voice_reset(c->voice_ctx)) { c->failed = true; return false; }
            c->voice_reset_pending = false;
        }
        /* 每一帧都送进 VAD，包括低能量与非语音帧：WebRTC 内部维护自己的噪声估计和有界挂起，
         * 只喂活跃帧会让它的判决偏离设计口径。 */
        if (!c->voice_frame(c->voice_ctx, pcm, &speech)) { c->failed = true; return false; }
    }
    if (c->fft_enabled && !c->spectrum.ready) { c->failed = true; return false; }
    double db = lc_rms_dbfs(pcm, LC_FRAME_SAMPLES);
    /* 段内帧不参与底噪跟踪：起音时冻结的底噪在整个段内保持有效。 */
    lc_floor_frame(&c->floor, db, c->active, ms);
    double floor = c->active ? c->frozen : c->floor.bg;
    bool energy = db > floor + 3.0;
    lc_spectral_features_t features = {0};
    bool spectral_ok = true, veto = false, candidate = false, recovery_candidate = false;
    /* 频谱特征每帧最多计算一次，计算条件按用途分岔：段内帧和唤醒候选沿用各自的能量口径，
     * 实验性噪声窗口开启时只要本帧能量过线就计算，使窗口能观察 VAD 判负的能量帧；
     * VAD 的挂起（hangover）因此不会成为噪声占比的分母来源。 */
    bool legacy_energy = c->active || c->mode == LC_WAKE || db > floor + 9.0;
    if (c->fft_enabled && energy &&
        ((speech && legacy_energy) || c->noise_gate_enabled))
        spectral_ok = lc_spectrum_features(&c->spectrum, pcm, &features) &&
                      lc_spectrum_accept(&features);
    if (c->fft_enabled && c->noise_gate_enabled) {
        /* suspected 是噪声联合特征：高频比（per mille 起音门限换算后的比例）与谱质心（Hz）
         * 必须同时超过各自门限。两项都是“严格大于”，测量值恰好等于门限时不判为疑似噪声。 */
        bool suspected = features.valid &&
            features.band_1000_8000_ratio > c->noise_config.high_ratio &&
            features.centroid_hz > c->noise_config.centroid_hz;
        candidate = energy && speech && spectral_ok && !suspected;
        /* 起音对“没有联合噪声特征”刻意放宽，只要不具备噪声特征就算候选；
         * 但恢复一段被噪声占优的音频要求更严，必须同时满足高频比和质心两项上限。 */
        recovery_candidate = candidate && features.valid &&
            features.band_1000_8000_ratio <= c->noise_recovery_ratio &&
            features.centroid_hz <= c->noise_recovery_centroid;
        veto = lc_noise_observe(&c->noise_window, &c->noise_config, energy,
            suspected, candidate, c->active);
    }
    if (c->active) {
        /* 段内帧先无条件进入上传序列，再结算活跃/静音；顺序颠倒会让本帧的静音判定影响
         * 它自己是否被发送。 */
        if (!emit(c, LC_AUDIO, pcm, db, false)) return false;
        /* 普通对话降低活跃抵扣量，减少间歇噪声拖延段尾；代价是对断续讲话的停顿容忍度下降。
         * 每帧 20 ms：普通对话抵扣 40 ms（1:2），唤醒候选保留 80 ms（1:4）。 */
        const unsigned active_credit_ms = c->mode == LC_DIALOG ? 40U : 80U;
        /* 段内活跃是四重条件的与：能量（相对冻结底噪）、VAD、频谱门控、噪声窗口未否决。
         * 噪声窗口关闭时 veto 恒为 false，本行即退化为原有能量+VAD+谱门控口径。 */
        bool active = energy && speech && spectral_ok && !veto;
        lc_tail_action_t tail_action = LC_TAIL_NORMAL;
        /* 收尾迟滞只作用于 dialog 的段内阶段，唤醒候选段保持原逐帧规则：短唤醒段若被拉长，
         * 会明显增加后续误触发。三个条件缺一（非 dialog、未开 FFT/噪声窗口、未开 NOISE_TAIL）
         * 时 tail_action 保持 NORMAL，即回到未启用该实验前的行为。 */
        if (c->mode == LC_DIALOG && c->fft_enabled && c->noise_gate_enabled && c->noise_tail_enabled) {
            bool was_latched = c->noise_tail.latched;
            tail_action = lc_noise_tail_step(&c->noise_tail, veto, recovery_candidate);
            if (!was_latched && c->noise_tail.latched) ++c->noise_tail_entries;
            if (tail_action == LC_TAIL_RECOVER) ++c->noise_tail_recoveries;
        }
        if (tail_action == LC_TAIL_RECOVER) {
            /* 恢复即清零静音计数，使收尾从头开始；同时清空噪声窗口，避免下一帧仅凭残留的
             * 高频证据立刻重新进入收尾状态。 */
            c->silence = 0;
            lc_noise_reset(&c->noise_window);
        }
        else if (tail_action == LC_TAIL_HOLD) ++c->noise_tail_hold_frames;
        else if (tail_action == LC_TAIL_COUNT) c->silence += 20;
        else if (active)
            c->silence = c->silence > active_credit_ms ? c->silence - active_credit_ms : 0;
        else c->silence += 20;
        /* 上限按已发送帧数计；唤醒段 400 帧/8 秒，普通段 750 帧/15 秒。 */
        bool limit = c->frames >= (c->mode == LC_WAKE ? 400U : LC_MAX_FRAMES);
        unsigned end_ms = c->voice_frame
            ? (c->mode == LC_WAKE ? c->voice_end_wake_ms : c->voice_end_dialog_ms)
            : (c->mode == LC_WAKE ? 500U : 700U);
        /* 只有已锁存噪声收尾的段才追加尾部 PCM 保护；该保护不禁止恢复，
         * 也不会越过上面的最大帧数上限。 */
        if (c->noise_tail.latched) end_ms += c->noise_tail_end_guard_ms;
        if (limit || c->silence >= end_ms) {
            if (!emit(c, LC_END, NULL, db, limit)) return false;
            c->active = false;
            lc_noise_tail_reset(&c->noise_tail);
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
    /* 起音前的活跃判据与段内不同：底噪用实时跟踪值而非冻结值，能量门限按模式分档
     * （wake +3 dB、dialog +9 dB），VAD、频谱门控与噪声否决同样按与逻辑参与。 */
    bool active = db > c->floor.bg + (c->mode == LC_WAKE ? 3.0 : 9.0) &&
                  speech && spectral_ok && !veto;
    if (c->window_count == window) c->window_active -= c->window[c->window_head];
    else ++c->window_count;
    c->window[c->window_head] = active;
    c->window_head = (c->window_head + 1) % window;
    c->window_active += active;
    /* 当前帧被噪声否决时不允许由更早的活跃证据起音；预录内容和后续候选帧的机会都保留，
     * 窗口计数已按本帧更新，因此否决结束后不需要补帧。 */
    if (veto) return true;
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
    lc_noise_tail_reset(&c->noise_tail);
    c->noise_tail_entries = c->noise_tail_recoveries = c->noise_tail_hold_frames = 0;
    if (!emit(c, LC_START, NULL, db, false)) return false;
    /* 预录按原采集顺序紧跟 start 补发，段内索引从 0 连续覆盖预录和实时帧。 */
    unsigned start = (c->pre_head + LC_PREROLL_FRAMES - c->pre_count) % LC_PREROLL_FRAMES;
    for (unsigned i = 0; i < c->pre_count; ++i) {
        unsigned p = (start + i) % LC_PREROLL_FRAMES;
        if (!emit(c, LC_AUDIO, c->preroll[p], c->preroll_db[p], false)) return false;
    }
    return true;
}
