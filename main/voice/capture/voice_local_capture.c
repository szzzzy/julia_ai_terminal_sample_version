/**
 * @file voice_local_capture.c
 * @brief 本地分段收音（capture-v1）的固件侧入口：把板级 PCM1 帧交给 local_capture 算法，
 * 并把 capture_start / PCM2 / capture_end 记录交给上层上行 FIFO。
 *
 * 模块职责：在设备侧完成起音/结束判决与分段记录组帧，供 WSS 上行发送。
 * 模块边界：不负责网络收发与重连。麦克风任务独占底噪、预录和判决历史，并写期望模式
 * （wanted_mode）；WSS owner 只协商能力（capture_ready 置位）和投递判决，不直接改底噪。
 * 关键依赖：local_capture / lc_vad 算法、voice_state_sync 的能力协商、wss_transport 的上行接口。
 * 核心不变量：send 回调失败会让采集器置 failed，本模块随后请求结束会话，由 WSS owner 统一
 * 重连，不允许丢音后继续识别；旧代次数据不得参与新连接。
 */
#include "voice_local_capture.h"
#include "voice_timing.h"
#if CONFIG_JULIA_CAPTURE_VAD_ENABLE
#include "lc_vad.h"
#endif
#include "voice_state_sync.h"
#include "voice_control_guard.h"
#include "wss_transport.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "cJSON.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* 判决队列槽：epoch 为收到判决时的连接代次，id 为话语编号，verdict 取 1=noise、2=empty、3=speech。 */
typedef struct { uint32_t epoch, id; unsigned verdict; } verdict_t;
/* 一段话语的判决回填依据：count 为已上传帧数，db[] 保留逐帧电平（dBFS）。
 * complete 是生产侧的段结束标记：收到 LC_END 记录时、在调用发送回调之前就置位，只表示本段
 * 正常结束。生成记录、写入上行 FIFO、真正送出是三个不同阶段，它不代表段尾已经发送成功；
 * 截断段（LC_ABORT）不会被标记完整，也就不会被迟到判决回填。 */
typedef struct {
    uint32_t epoch, id;
    unsigned count, verdict;
    bool complete;
    double db[LC_MAX_FRAMES];
} history_t;
/* history 为固定槽位：同一时刻最多四段待确认判决，按 id % 4 定位，重新起音会覆盖对应槽位，
 * 被覆盖段的迟到判决因槽位 id 不符而作废。sample_ms 是单调采样时间轴（毫秒），last_wall_ms
 * 只用于识别采音是否停顿。 */
typedef struct {
    local_capture_t capture;
#if CONFIG_JULIA_CAPTURE_VAD_ENABLE
    lc_vad_t vad;
#endif
    history_t history[4];
    double scratch[LC_MAX_FRAMES];
    int16_t pcm[LC_FRAME_SAMPLES];
    uint8_t frame[656];
    int64_t sample_ms, last_wall_ms;
    uint32_t listen_revision;
    unsigned idle_ms;
    bool listen_started, idle_expired;
} capture_storage_t;
static capture_storage_t *s;
static QueueHandle_t verdicts;
static voice_capture_send_t send_record;
static voice_capture_event_t state_event;
static atomic_uint epoch, wanted_mode;
static atomic_bool ready;
static uint32_t producer_epoch, last_applied;
static bool hello_sent;
static int64_t hello_deadline;

/* PCM2 头字段要求 little-endian，逐字节写出以免依赖主机字节序。 */
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = v >> (8 * i);
}

/* local_capture 的 emit 回调，运行在采音任务上下文，并复用 s->frame 组帧。
 * 返回 false 表示本记录不成立或发送失败：采集器随即置 failed，由 frame() 结束会话。 */
static bool output(void *ctx, const lc_record_t *r)
{
    (void)ctx;
    /* 未协商就绪或已换代时拒绝本记录，旧 generation 的数据不得进入新连接。 */
    if (!atomic_load(&ready) || producer_epoch != atomic_load(&epoch)) return false;
    history_t *h = &s->history[r->id % 4];
    size_t bytes;
    if (r->event == LC_AUDIO) {
        /* 656 字节 PCM2 是手写字节布局，字段偏移与语义由 docs/LOCAL_CAPTURE.md 规定；
         * 段内本地仍用 PCM1，只有线上分段帧改用此格式：
         *   0~3    ASCII "PCM2"
         *   4~7    uint32 utterance_id（little-endian）
         *   8~9    uint16 段内帧索引，自 0 连续，含预录
         *   10~11  int16 当前帧电平，单位百分之一 dB
         *   12~13  uint16 PCM 字节数，固定 640
         *   14     保留字节，写 0
         *   15     16~655 字节和的低 8 位，仅供接收端做一致性检查
         *   16~655 320 个 PCM16 样本
         * 校验和只保证传输一致性，不参与丢帧或判决依据。 */
        memcpy(s->frame, "PCM2", 4);
        put32(s->frame + 4, r->id);
        s->frame[8] = r->index;
        s->frame[9] = r->index >> 8;
        /* 该字段记录当前帧电平；start/end 报文里的 floor_dbfs 是冻结底噪，两者容易写反。 */
        int16_t db = (int16_t)lrint(r->dbfs * 100);
        s->frame[10] = (uint8_t)db;
        s->frame[11] = (uint16_t)db >> 8;
        s->frame[12] = 0x80; s->frame[13] = 0x02; s->frame[14] = 0;
        memcpy(s->frame + 16, r->pcm, 640);
        unsigned sum = 0;
        for (unsigned i = 16; i < 656; ++i) sum += s->frame[i];
        s->frame[15] = sum;
        bytes = 656;
        /* 判决回填只保留本段已上传的帧电平；超出固定容量说明本地记录已不可靠，
         * 此时结束会话，而不是继续用不完整历史回填。 */
        if (h->count >= LC_MAX_FRAMES) return false;
        h->db[h->count++] = r->dbfs;
    } else {
        const char *kind = r->event == LC_START ? "capture_start" :
                           r->event == LC_END ? "capture_end" : "capture_abort";
        /* start/end/abort 走 JSON。floor_dbfs 是起音时冻结的底噪，不是当前帧电平；
         * index 在 start 中为 0，在 end 中为段内已发送帧数；reason=limit 表示因帧数或
         * 时长上限结束，而 reason=silence 表示静音计数达标。 */
        int n = snprintf((char *)s->frame, sizeof(s->frame),
            "{\"type\":\"%s\",\"utterance_id\":%" PRIu32 ",\"mode\":\"%s\","
            "\"floor_dbfs\":%.9f,\"sample_rate\":16000,\"frame_ms\":20,"
            "\"frames\":%" PRIu32 ",\"reason\":\"%s\"}", kind, r->id,
            r->mode == LC_WAKE ? "wake" : "dialog", r->floor_dbfs, r->index,
            r->limit ? "limit" : "silence");
        if (n <= 0 || (size_t)n >= sizeof(s->frame)) return false;
        bytes = (size_t)n;
        /* 判决只对“完整且属于同一代次”的段生效：start 重置槽位，收到 end 记录时（发送回调
         * 之前）标记完整，abort 让槽位立刻失效，因此作废段不会被迟到判决回填。 */
        if (r->event == LC_START) {
            memset(h, 0, sizeof(*h));
            h->id = r->id; h->epoch = producer_epoch;
        }
        if (r->event == LC_END) h->complete = true;
        if (r->event == LC_ABORT) h->id = 0;
    }
    /* 上行 FIFO 写成功才回填状态；失败即返回 false，采集器随即置 failed。 */
    /* 必须在记录对 WSS 可见之前取快照：START 是相对时间轴的原点，
     * 并不表示语音在声学上就从这一时刻开始。 */
#if CONFIG_JULIA_VOICE_TIMING
    int64_t trace_us = esp_timer_get_time();
    if (r->event == LC_START) {
        voice_timing_record(VT_CAPTURE_START,producer_epoch,r->id,0,trace_us,0,r->mode);
        voice_timing_record(VT_GATE_STATE,producer_epoch,r->id,0,trace_us,
            s->capture.noise_gate_enabled, (int64_t)lrint(s->capture.noise_config.high_ratio*1000));
        voice_timing_record(VT_TAIL_CONFIG,producer_epoch,r->id,0,trace_us,
            r->mode == LC_DIALOG && s->capture.noise_tail_enabled &&
            s->capture.noise_gate_enabled && s->capture.fft_enabled,
            LC_NOISE_GRACE_FRAMES*20);
        voice_timing_record(VT_RECOVERY_SHAPE,producer_epoch,r->id,0,trace_us,
            (int64_t)lrint(s->capture.noise_recovery_ratio*1000),
            (int64_t)lrint(s->capture.noise_recovery_centroid));
        voice_timing_record(VT_TAIL_GUARD,producer_epoch,r->id,0,trace_us,
            s->capture.noise_tail_end_guard_ms,0);
    }
    else if (r->event == LC_END) {
        voice_timing_record(VT_CAPTURE_END,producer_epoch,r->id,0,trace_us,r->index,r->limit);
        voice_timing_record(VT_TAIL_SUMMARY,producer_epoch,r->id,0,trace_us,
            s->capture.noise_tail_entries,s->capture.noise_tail_recoveries);
    }
    else if (r->event == LC_ABORT)
        voice_timing_record(VT_CAPTURE_ABORT,producer_epoch,r->id,0,trace_us,0,0);
    else if (r->event == LC_AUDIO && r->index == 0)
        voice_timing_record(VT_FIRST_PCM_QUEUED,producer_epoch,r->id,0,trace_us,0,0);
#endif
    if (send_record(s->frame, bytes, producer_epoch) != ESP_OK) {
        voice_timing_record(VT_ENQUEUE_FAILED,producer_epoch,r->id,0,esp_timer_get_time(),r->event,0);
        return false;
    }
    if (r->event == LC_END)
        voice_timing_record(VT_ENQUEUE_END,producer_epoch,r->id,0,esp_timer_get_time(),r->index,0);
    /* 起音/结束只作为状态语义通知上层，实际如何回应由上层决定。 */
    if (producer_epoch == atomic_load(&epoch) && atomic_load(&ready) &&
        (r->event == LC_START || r->event == LC_END)) state_event(r->event, r->mode, s->listen_revision);
    if (r->event != LC_AUDIO)
        ESP_LOGI("local_capture", "event=%d epoch=%" PRIu32 " id=%" PRIu32
                 " mode=%d bg=%.2f frames=%" PRIu32 " limit=%u",
                 r->event, producer_epoch, r->id, r->mode, r->floor_dbfs, r->index, r->limit);
    return true;
}

/* 幂等：重复调用直接返回 ESP_OK，不覆盖已注册的回调，也不重建判决队列。 */
esp_err_t voice_local_capture_init(voice_capture_send_t send, voice_capture_event_t event)
{
    if (s) return ESP_OK;
    /* 工作内存一次分配在 PSRAM（MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT）：底噪双窗各
     * LC_FLOOR_CAPACITY=4096 项、每项 16 字节（合计 128 KiB），加同容量的 double scratch
     * （32 KiB）；4 段判决历史各含 LC_MAX_FRAMES=750 项 double 逐帧电平（每段 6000 字节）；
     * 另有 750 项 double scratch、320 样本 PCM 缓冲和 656 字节组帧缓冲。 */
    s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    verdicts = xQueueCreate(4, sizeof(verdict_t));
    if (!s || !verdicts) {
        if (s) heap_caps_free(s);
        if (verdicts) vQueueDelete(verdicts);
        s = NULL; verdicts = NULL;
        return ESP_ERR_NO_MEM;
    }
    send_record = send; state_event = event;
    lc_init(&s->capture, output, NULL);
#if CONFIG_JULIA_CAPTURE_VAD_ENABLE
    int64_t init_started = esp_timer_get_time();
    bool vad_ready = lc_vad_init(&s->vad, CONFIG_JULIA_CAPTURE_VAD_MODE);
    if (vad_ready)
        vad_ready = lc_set_voice_detector(&s->capture, lc_vad_frame, lc_vad_reset, &s->vad,
            ((CONFIG_JULIA_CAPTURE_VAD_WAKE_TAIL_MS + 19) / 20) * 20,
            ((CONFIG_JULIA_CAPTURE_VAD_DIALOG_TAIL_MS + 19) / 20) * 20);
    if (!vad_ready) {
        /* 可选 VAD 无法启动时，保留原有的 energy/FFT gate 配置。 */
        lc_vad_destroy(&s->vad);
        s->capture.noise_gate_enabled = false;
        ESP_LOGE("local_capture", "WebRTC VAD init failed; using existing energy/FFT gate");
    } else {
        ESP_LOGI("local_capture", "gate=energy+fft(%d)+webrtc mode=%d frame_ms=20 tail_wake=%u tail_dialog=%u init_us=%lld",
            s->capture.fft_enabled, CONFIG_JULIA_CAPTURE_VAD_MODE, s->capture.voice_end_wake_ms,
            s->capture.voice_end_dialog_ms, (long long)(esp_timer_get_time()-init_started));
    }
#endif
    ESP_LOGI("local_capture", "noise_gate enabled=%u ratio=%.3f centroid_hz=%.0f window=%u min_energy=%u percent=%u confirm=%u",
        s->capture.noise_gate_enabled, s->capture.noise_config.high_ratio,
        s->capture.noise_config.centroid_hz, s->capture.noise_config.window_frames,
        s->capture.noise_config.min_energy_frames, s->capture.noise_config.noise_percent,
        s->capture.noise_config.confirm_frames);
    ESP_LOGI("local_capture", "noise_tail dialog_only enabled=%u recovery_ratio=%.3f recovery_centroid=%.0f candidates=%u/%u grace_ms=%u end_guard_ms=%u",
        s->capture.noise_tail_enabled, s->capture.noise_recovery_ratio, s->capture.noise_recovery_centroid,
        LC_NOISE_RECOVERY_NEED, LC_NOISE_RECOVERY_FRAMES, LC_NOISE_GRACE_FRAMES*20,
        s->capture.noise_tail_end_guard_ms);
    return ESP_OK;
}

/* 只由 WSS owner 在会话开始/结束时调用（voice_service_on_session_start / on_session_end）；
 * 文件结束、busy 冷却等上行换代只重开 ring/pump 的代次，不经过本接口。generation 是本连接的
 * 数据归属编号：换代后旧代次的预录、未发送段和判决一律作废；generation=0 表示会话已结束。
 * 本函数不负责阻止后续 hello，poll() 内部也不判 generation=0，见 voice_local_capture_poll()。 */
void voice_local_capture_connection(uint32_t generation)
{
    atomic_store(&ready, false);
    atomic_store(&epoch, generation);
    atomic_store(&wanted_mode, LC_OFF);
    hello_sent = false;
    hello_deadline = generation ? esp_timer_get_time() + 5000000LL : 0;
}

bool voice_local_capture_ready(void) { return atomic_load(&ready); }
void voice_local_capture_mode(lc_mode_t mode) { atomic_store(&wanted_mode, mode); }

/* 由 WSS owner 每轮调用；返回值是上层分段上传的放行闸门，返回 false 时调用方不得发送 PCM2。
 * 未就绪期间在这里发 capture_hello，等待窗口 5 秒。本函数不检查 generation 是否为 0：会话
 * 结束后不再发 hello 依靠调用方停止轮询，见 voice_local_capture_connection()。 */
bool voice_local_capture_poll(void)
{
    if (atomic_load(&ready)) return true;
    if (!hello_sent) {
        char hello[192];
        int n = snprintf(hello, sizeof(hello),
            "{\"type\":\"capture_hello\",\"version\":1,\"session_id\":\"%s\","
            "\"sample_rate\":16000,\"frame_ms\":20,\"stream_generation\":%" PRIu32 "}",
            voice_state_sync_session_id(), (uint32_t)atomic_load(&epoch));
        if (n > 0 && (size_t)n < sizeof(hello) &&
            wss_transport_send_now(1, (const uint8_t *)hello, n) == ESP_OK) {
            hello_sent = true;
            hello_deadline = esp_timer_get_time() + 5000000LL;
        }
    } else if (esp_timer_get_time() >= hello_deadline) {
        /* 5 秒窗口内没有收到匹配的 capture_ready：结束本次会话并向旧服务器退避 60 秒，不再
         * 发出 PCM2。超时无法判定对端是否支持 capture-v1（也可能只是慢或丢包），此处不做
         * 能力推断。 */
        ESP_LOGE("local_capture", "capture-v1 acknowledgement timeout");
        wss_transport_defer_retry(60);
        wss_transport_fail_session();
    }
    return false;
}

/* 返回 true 表示本模块已消费该文本（包括因 session_id 不符而故意丢弃的消息），调用方不得
 * 再按其它命令解析；返回 false 才交给后续处理器。 */
bool voice_local_capture_text(const uint8_t *text, size_t len)
{
    if (!len || text[0] != '{') return false;
    cJSON *o = voice_control_parse((const char *)text, len);
    cJSON *type = cJSON_GetObjectItemCaseSensitive(o, "type");
    bool handled = cJSON_IsString(type) &&
        (!strcmp(type->valuestring, "capture_ready") || !strcmp(type->valuestring, "capture_verdict"));
    if (!handled) { cJSON_Delete(o); return false; }
    /* session_id 不符的消息一律静默忽略但仍返回 true：它属于别的会话，不是本模块的业务。 */
    cJSON *session = cJSON_GetObjectItemCaseSensitive(o, "session_id");
    if (!cJSON_IsString(session) || strcmp(session->valuestring, voice_state_sync_session_id())) {
        cJSON_Delete(o); return true;
    }
    if (!strcmp(type->valuestring, "capture_ready")) {
        cJSON *version = cJSON_GetObjectItemCaseSensitive(o, "version");
        cJSON *generation = cJSON_GetObjectItemCaseSensitive(o, "stream_generation");
        /* 版本和 stream_generation 必须同时指向本次握手，否则是旧连接或旧代次的迟到回复。 */
        if (hello_sent && cJSON_IsNumber(version) && version->valuedouble == 1 &&
            cJSON_IsNumber(generation) && generation->valuedouble == atomic_load(&epoch))
            atomic_store(&ready, true);
    } else if (atomic_load(&ready)) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "utterance_id");
        cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "verdict");
        /* utterance_id 必须是 ≥1 的整数：0 是“无效编号”的保留值，非整数或未知 verdict 直接丢弃。 */
        if (cJSON_IsNumber(id) && id->valuedouble >= 1 && id->valuedouble <= UINT32_MAX &&
            floor(id->valuedouble) == id->valuedouble && cJSON_IsString(v)) {
            unsigned kind = !strcmp(v->valuestring, "noise") ? 1 :
                            !strcmp(v->valuestring, "empty") ? 2 :
                            !strcmp(v->valuestring, "speech") ? 3 : 0;
            verdict_t value = {atomic_load(&epoch), (uint32_t)id->valuedouble, kind};
            if (kind) voice_timing_record(VT_VERDICT,value.epoch,value.id,0,esp_timer_get_time(),kind,0);
            /* 4 槽非阻塞队列：入队失败就结束会话而不是丢判决——判决丢失会让本地底噪与云端
             * 判定口径不一致（本地继续用旧底噪分段），宁可重连后重新握手。 */
            if (kind && xQueueSend(verdicts, &value, 0) != pdTRUE) wss_transport_fail_session();
        }
    }
    cJSON_Delete(o);
    return true;
}

/* 采音任务在处理当前帧之前传入状态快照；0 表示不启用 S4 未起音期限。 */
void voice_local_capture_listen_window(uint32_t revision)
{
    if (!s || s->listen_revision == revision) return;
    /* 唤醒候选或上一轮对话不得算作本次 S4 的起音。 */
    if (revision) lc_set_mode(&s->capture, LC_OFF);
    s->listen_revision = revision;
    s->idle_ms = 0;
    s->idle_expired = false;
    s->listen_started = false;
}

/* 每 20 ms 输入完整 PCM1 帧；期望模式还需 capture_ready 放行。 */
void voice_local_capture_frame(const uint8_t *frame, size_t len)
{
    if (!s || len != 656 || memcmp(frame, "PCM1", 4)) return;
    int64_t wall_ms = esp_timer_get_time() / 1000;
    uint32_t current = atomic_load(&epoch);
    if (producer_epoch != current) {
        /* 换代即复位：模式、失败位、判决历史和采样时间轴全部重来，旧代数据不得参与新连接。 */
        lc_set_mode(&s->capture, LC_OFF);
        s->capture.voice_reset_pending = true;
        s->capture.failed = false;
        memset(s->history, 0, sizeof(s->history));
        producer_epoch = current; last_applied = 0;
        s->sample_ms = wall_ms - 20;
        s->idle_ms = 0;
        s->listen_started = s->idle_expired = false;
    }
    /* 采样时间轴按 20 ms/帧单调推进，与云端 floor_epoch + total_samples/sr 同口径；
     * 网络和任务抖动不改变帧间隔。采音停顿超过 100 ms 时按当前挂钟重新对齐，阈值来源未确认，
     * 这里只保证“停顿后不把积压的墙钟时间算进统计”。 */
    if (wall_ms - s->last_wall_ms > 100) s->capture.voice_reset_pending = true;
    if (s->capture.mode == LC_OFF && wall_ms - s->last_wall_ms > 100)
        s->sample_ms = wall_ms - 20;
    s->last_wall_ms = wall_ms;
    s->sample_ms += 20;
    /* 只有握手成功且上层给出模式时才启用本地判决，否则保持 LC_OFF 空转。 */
    lc_set_mode(&s->capture, atomic_load(&ready) ? atomic_load(&wanted_mode) : LC_OFF);
    /* 上报之后要一直扣住这个窗口，直到 FSM 消费掉这条带 revision 校验的事件；
     * 否则后面的帧会在已排队的超时事件之前起音。 */
    if (s->idle_expired) return;
    verdict_t v;
    /* 判决队列非阻塞取空：网络任务只投递，真正的回填顺序由本任务在帧边界决定。 */
    while (xQueueReceive(verdicts, &v, 0) == pdTRUE) {
        history_t *h = &s->history[v.id % 4];
        /* 逐项校验：判决代次、槽位代次、槽位话语编号、该段已完整结束、编号晚于已应用值、
         * 尚未应用过；任一不符即丢弃（槽位被新段覆盖的判决就此淘汰，不再补上）。 */
        if (v.epoch == producer_epoch && h->epoch == v.epoch && h->id == v.id &&
            h->complete && v.id > last_applied && !h->verdict) h->verdict = v.verdict;
    }
    /* 判决只在没有活动段时应用：下一段已经起音就先冻结它，迟到裁决留到该段结束后再处理
     * （应用范围见下方：只在仍保留的判决里按话语编号排序），避免把上一段的底噪修正混进
     * 当前段。 */
    if (!s->capture.active) {
        for (unsigned pass = 0; pass < 4; ++pass) {
            history_t *h = NULL;
            /* 只在“已经到达且槽位仍保留”的判决里按话语编号从小到大应用：编号更早的判决若
             * 尚未到达或槽位已被新段覆盖，就不会再影响底噪，也不会阻塞后面的判决，因此
             * “先发生的判决一定先影响底噪”并不成立。 */
            for (unsigned i = 0; i < 4; ++i)
                if (s->history[i].verdict && s->history[i].id > last_applied &&
                    (!h || s->history[i].id < h->id)) h = &s->history[i];
            if (!h) break;
            double previous_bg = s->capture.floor.bg;
            if (h->verdict == 1) {
                /* noise：用该段所有已上传帧电平的 P10 重锚底噪，并限幅到 −80～−35 dB。 */
                memcpy(s->scratch, h->db, h->count * sizeof(double));
                lc_floor_reset(&s->capture.floor,
                    fmin(-35.0, fmax(-80.0, lc_percentile10(s->scratch, h->count))));
            } else if (h->verdict == 2 && h->count >= 20) {
                /* empty：段长至少 20 帧（400 ms）才按原采集顺序回填；不足 20 帧时本判决
                 * 不改动底噪。 */
                lc_floor_feed_segment(&s->capture.floor, h->db, h->count, esp_timer_get_time() / 1000);
            }
            /* 判决 1/2 都清空起音累计窗（window_*，wake 25 帧/500 ms、dialog 15 帧/300 ms 的
             * 活跃度统计）。它与底噪快慢窗是两回事：empty 不足 20 帧、没有回填底噪时也照样清空。 */
            if (h->verdict == 1 || h->verdict == 2)
                s->capture.window_head = s->capture.window_count = s->capture.window_active = 0;
            ESP_LOGI("local_capture", "verdict=%u epoch=%" PRIu32 " id=%" PRIu32
                     " frames=%u bg=%.2f->%.2f", h->verdict, producer_epoch,
                     h->id, h->count, previous_bg, s->capture.floor.bg);
            last_applied = h->id; h->verdict = 0;
        }
    }
    /* PCM1 负载是 little-endian PCM16，逐样本展开到本地缓冲后再交给算法。 */
    for (unsigned i = 0; i < LC_FRAME_SAMPLES; ++i)
        s->pcm[i] = (int16_t)((uint16_t)frame[16 + 2*i] | ((uint16_t)frame[17 + 2*i] << 8));
    /* 处理失败意味着本帧发不出去或历史已不可靠：结束会话重连，不跳过该段继续识别。
     * 换代说明失败属于旧连接，此时不再上报。 */
#if CONFIG_JULIA_VOICE_TIMING
    unsigned tail_entries = s->capture.noise_tail_entries;
    unsigned tail_recoveries = s->capture.noise_tail_recoveries;
#endif
    bool capture_ok = lc_process(&s->capture, s->pcm, s->sample_ms);
#if CONFIG_JULIA_VOICE_TIMING
    if (s->capture.noise_tail_entries > tail_entries)
        voice_timing_record(VT_TAIL_ENTER,producer_epoch,s->capture.id,0,esp_timer_get_time(),
                            s->capture.noise_window.noise,s->capture.noise_window.energy);
    if (s->capture.noise_tail_recoveries > tail_recoveries)
        voice_timing_record(VT_TAIL_RECOVER,producer_epoch,s->capture.id,0,esp_timer_get_time(),
                            s->capture.noise_tail_recoveries,s->capture.noise_tail_hold_frames*20);
#endif
    if (!capture_ok) {
        if (producer_epoch == atomic_load(&epoch)) wss_transport_fail_session();
        return;
    }
    if (s->capture.active) s->listen_started = true;
    if (s->capture.mode != LC_DIALOG) {
        s->idle_ms = 0; /* 提示播放与握手门控都不消耗这个窗口。 */
    } else if (s->listen_revision && !s->listen_started &&
               producer_epoch == atomic_load(&epoch) && atomic_load(&ready)) {
        /* 先由原有起音 gate 判定：最后一帧判为语音即视为已起音；
         * 孤立的已接受帧不会重置计数，因此不会延长 250 帧 / 5 秒窗口。 */
        s->idle_ms += 20;
        if (s->idle_ms >= 5000) {
            s->idle_expired = true;
            ESP_LOGI("local_capture", "no speech onset after 5000 ms revision=%" PRIu32,
                     s->listen_revision);
            state_event(LC_IDLE_TIMEOUT, LC_DIALOG, s->listen_revision);
        }
    }
}
