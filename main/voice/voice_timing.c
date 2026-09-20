#include "voice_timing.h"
/* 实现要点：固定容量环形缓冲 + 自旋锁；生产者只写入，唯一的消费者是下面的诊断任务
 * （或主机测试）。缓冲满时只累加 dropped，不阻塞、不覆盖未读记录。 */
#if CONFIG_JULIA_VOICE_TIMING
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <inttypes.h>
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t logger_task;
static void timing_task(void *ctx)
{
    (void)ctx;
    /* 每 50 ms 排空一批（每批上限见 flush）：周期与批量共同决定打印不能挤占实时任务。 */
    for (;;) { voice_timing_flush(); vTaskDelay(pdMS_TO_TICKS(50)); }
}
#define LOCK() portENTER_CRITICAL(&lock)
#define UNLOCK() portEXIT_CRITICAL(&lock)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif
/* 固定容量：64 条。容量与批量（flush 每批 8 条）共同决定可承受的瞬时事件密度；
 * 溢出只影响诊断，不改变业务流程。 */
#define CAPACITY 64U
static voice_timing_record_t records[CAPACITY];
static uint32_t boot_id, sequence, dropped, head, count;
static bool enabled;
/* 名字表必须与 voice_timing_kind_t 的枚举顺序逐项对应，否则日志里的 event= 会张冠李戴。 */
static const char *const names[] = {
    "session_start", "session_end", "capture_start", "capture_end",
    "capture_abort", "enqueue_failed", "enqueue_end", "first_pcm_queued",
    "start_tx", "end_tx", "abort_tx", "first_pcm_tx", "last_pcm_tx",
    "tx_failed", "uplink_stats", "verdict", "spks", "first_pcm_rx",
    "spke", "first_i2s", "play_done", "gate_state",
    "tail_config", "tail_enter", "tail_recover", "tail_summary",
    "recovery_shape", "tail_guard"
};
/* 清空环形缓冲并创建诊断任务；重复调用会重建序号与缓冲，但不会创建第二个任务。
 * 任务创建失败时关闭整个模块（enabled=false）并打印 VT_DISABLED：诊断不得影响主流程，
 * 因此这里只降级、不返回错误。 */
void voice_timing_init(uint32_t boot)
{
    LOCK(); boot_id = boot; sequence = dropped = head = count = 0; enabled=true; UNLOCK();
#ifdef ESP_PLATFORM
    if (!logger_task && xTaskCreate(timing_task,"voice_timing",3072,NULL,1,&logger_task)!=pdPASS) {
        LOCK(); enabled=false; UNLOCK();
        ESP_LOGW("voice_timing","VT_DISABLED logger task allocation failed");
    }
#endif
}
const char *voice_timing_name(voice_timing_kind_t kind)
{ return (unsigned)kind < VT_EVENT_COUNT ? names[kind] : "invalid"; }
/* 写一条记录：只在自旋锁内做一次结构体拷贝，不分配、不打印、不等待，因此可从实时任务调用。
 * 缓冲满时只累加 dropped；seq 始终递增，可用于在日志里发现缺口。 */
void voice_timing_record(voice_timing_kind_t kind, uint32_t epoch, uint32_t id,
                         uint32_t play, int64_t us, int64_t a, int64_t b)
{
    LOCK();
    if (!enabled) { UNLOCK(); return; }
    ++sequence;
    if (count == CAPACITY) ++dropped;
    else {
        records[(head + count) % CAPACITY] = (voice_timing_record_t){
            boot_id, sequence, epoch, id, play, dropped, kind, us, a, b};
        ++count;
    }
    UNLOCK();
}
/* 取出一条最旧记录（FIFO）。只允许单一消费者调用（诊断任务，或主机测试）：多消费者会
 * 各自取走一部分记录，使时序不可还原。 */
bool voice_timing_take(voice_timing_record_t *r)
{
    LOCK();
    bool have = count != 0;
    if (have) { *r = records[head]; head = (head + 1) % CAPACITY; --count; }
    UNLOCK();
    return have;
}
void voice_timing_flush(void)
{
#ifdef ESP_PLATFORM
    /* 每批最多 8 条，保证单次 flush 有界；绝不在持有生产者锁或 WSS owner 上打印。 */
    voice_timing_record_t r;
    for (unsigned i=0; i<8 && voice_timing_take(&r); ++i)
        ESP_LOGI("voice_timing", "VT1 boot=%08" PRIx32 " seq=%" PRIu32
            " event=%s epoch=%" PRIu32 " id=%" PRIu32 " play=%" PRIu32
            " us=%" PRIi64 " a=%" PRIi64 " b=%" PRIi64 " dropped=%" PRIu32,
            r.boot,r.seq,voice_timing_name(r.kind),r.epoch,r.id,r.play,r.us,r.a,r.b,r.dropped);
    static uint32_t reported_drops;
    LOCK(); uint32_t lost=dropped; UNLOCK();
    if (lost != reported_drops) {
        ESP_LOGW("voice_timing", "VT_LOSS boot=%08" PRIx32 " dropped=%" PRIu32,boot_id,lost);
        reported_drops=lost;
    }
#endif
}
#endif
