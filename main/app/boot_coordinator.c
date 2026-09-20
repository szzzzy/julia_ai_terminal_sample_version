/**
 * @file    boot_coordinator.c
 * @brief   boot_coordinator 的实现：上下文引用计数、每分支一个 worker Task、结果汇总与云门控。
 *
 * 实现要点与不变量：
 * - 协调者与每个成功创建的 worker 各持一份上下文引用，只有把引用减到 0 的一方销毁完成
 *   队列和上下文；因此协调者超时退出后，晚到的 worker 仍能安全发布结果并回收。
 * - 完成队列固定 4 槽（1 个资源里程碑 + 3 个终态），正好覆盖“只有一个分支设置 finish”
 *   的约定，所以 worker 用 0 等待发送不会因队列满而失败。
 * - worker 只把 branches[i] 复制进自己的工作参数，发布自己的终态结果，结束时自删；
 *   协调者从不删除 worker，也不在超时后抢占或回收分支资源。
 *
 * 本文件不判断业务故障类型、不记录故障、不触发复位：这些由调用方（app/main.c）根据
 * results[] 决定。模块边界与启动预算见 docs/BOOT_INITIALIZATION.md。
 */
#include "boot_coordinator.h"
#include <stdlib.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

typedef struct boot_context boot_context_t;
typedef struct {
    boot_context_t *owner;
    boot_branch_t branch;
    unsigned index;
} boot_worker_t;
struct boot_context {
    QueueHandle_t completions;
    atomic_uint references;
    boot_worker_t workers[3];
};
typedef struct { unsigned index; esp_err_t error; bool prepared; } completion_t;

/* coordinator 与每个已创建 worker 各持一份引用；最后一方释放队列和参数。 */
static void release(boot_context_t *context)
{
    if (atomic_fetch_sub(&context->references, 1) == 1) {
        vQueueDelete(context->completions);
        free(context);
    }
}
static void worker(void *argument)
{
    boot_worker_t *job = argument;
    boot_context_t *context = job->owner;
    ESP_LOGI("BOOT", "branch=%s start t=%lldms", job->branch.name,
             (long long)(esp_timer_get_time()/1000));
    completion_t result = {.index = job->index, .error = job->branch.run()};
    if (result.error == ESP_OK && job->branch.finish != NULL) {
        result.prepared = true;
        (void)xQueueSend(context->completions, &result, 0);
        result.prepared = false;
        result.error = job->branch.finish();
    }
    ESP_LOGI("BOOT", "branch=%s end result=%s t=%lldms stack_free=%u", job->branch.name,
             esp_err_to_name(result.error), (long long)(esp_timer_get_time()/1000),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    /* 四槽容纳显示资源里程碑和三个终态；即使 coordinator 超时也不阻塞。 */
    (void)xQueueSend(context->completions, &result, 0);
    release(context);
    vTaskDelete(NULL);
}
esp_err_t boot_coordinator_run(const boot_branch_t branches[3],
                               boot_result_t results[3], uint32_t timeout_ms,
                               boot_branch_fn_t resources_ready)
{
    for (unsigned i=0; i<3; ++i) results[i] = (boot_result_t){false, ESP_ERR_INVALID_STATE};
    boot_context_t *context = calloc(1, sizeof(*context));
    if (!context) return ESP_ERR_NO_MEM;
    context->completions = xQueueCreate(4, sizeof(completion_t));
    if (!context->completions) { free(context); return ESP_ERR_NO_MEM; }
    atomic_init(&context->references, 1);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (unsigned i=0; i<3; ++i) {
        context->workers[i] = (boot_worker_t){context, branches[i], i};
        atomic_fetch_add(&context->references, 1);
        if (xTaskCreate(worker, branches[i].name, branches[i].stack_bytes,
                        &context->workers[i], 3, NULL) != pdPASS) {
            ESP_LOGE("BOOT", "branch=%s create_failed t=%lldms", branches[i].name,
                     (long long)(esp_timer_get_time()/1000));
            release(context);
            completion_t failure = {i, ESP_ERR_NO_MEM, false};
            (void)xQueueSend(context->completions, &failure, 0);
        }
    }
    esp_err_t error = ESP_OK;
    bool prepared[3] = {false};
    bool notified = false;
    unsigned count = 0;
    while (count < 3) {
        int64_t remaining = deadline - esp_timer_get_time();
        completion_t result;
        if (remaining <= 0 || xQueueReceive(context->completions, &result,
                pdMS_TO_TICKS((remaining+999)/1000)) != pdTRUE) {
            error = ESP_ERR_TIMEOUT;
            break;
        }
        prepared[result.index] = result.error == ESP_OK;
        if (!result.prepared) {
            results[result.index] = (boot_result_t){true, result.error};
            ++count;
        }
        if (!notified && prepared[0] && prepared[1] && prepared[2]) {
            notified = true;
            if (resources_ready != NULL) {
                error = resources_ready();
                if (error != ESP_OK) break;
                if (esp_timer_get_time() >= deadline) { error = ESP_ERR_TIMEOUT; break; }
            }
        }
    }
    release(context);
    return error;
}
