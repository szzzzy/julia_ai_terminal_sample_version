#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef esp_err_t (*boot_branch_fn_t)(void);
typedef struct {
    const char *name;
    boot_branch_fn_t run;
    uint32_t stack_bytes;
    boot_branch_fn_t finish; /* 仅分支 0 可用：显示资源成功后继续动画。 */
} boot_branch_t;
typedef struct {
    bool completed;
    esp_err_t error;
} boot_result_t;

/* 固定的显示/音频/资源三分支。resources_ready 在协调者上下文中仅调用一次，
 * 等所有必要资源成功但不等显示尾段；超时不删除工作任务，晚到结果自行回收。 */
esp_err_t boot_coordinator_run(const boot_branch_t branches[3],
                               boot_result_t results[3], uint32_t timeout_ms,
                               boot_branch_fn_t resources_ready);
