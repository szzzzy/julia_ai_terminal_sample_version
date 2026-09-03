/**
 * @file    lvgl_port.c
 * @brief   让界面绘制、LCD 传输和显示开关按安全顺序执行。
 *
 * LVGL 会分块生成画面，而 LCD 通过 DMA 异步发送像素。只有硬件确认上一块已经发送
 * 完成后，LVGL 才能复用对应缓冲，否则画面会撕裂或出现随机色块。所有界面对象修改
 * 也必须串行执行，避免两个任务同时改变同一个对象。
 *
 * @section lvgl_dma 数据流与像素格式
 *         framebuffer(LVGL) → lvgl_flush_cb → lvgl_port_draw_bitmap_sync →
 *         esp_lcd_panel_draw_bitmap →（DMA/SPI）→ 面板。像素为 RGB565（LV_COLOR_16_SWAP，
 *         见 build 侧），面板侧按大端接收；若缓冲在外部 RAM 会先做 cache 同步。
 *
 * 一把锁保护界面对象，一把锁保证“发送画面”和“开关面板”不会同时发生；硬件完成
 * 通知用于唤醒正在等待的刷新操作。睡眠时停止产生新画面，但保留界面任务，便于
 * 唤醒后继续使用原有对象。
 *
 * @see    main/display/julia_display.c（panel 的创建与初始化顺序）
 * @see    main/display/esp_lcd_st77916.c（SPI 传输完成回调的触发方）
 */
#include "lvgl_port.h"

#include <string.h>
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_memory_utils.h"

#define TAG "LVGL_PORT"
#define LVGL_TICK_PERIOD_MS         2
#define LVGL_HANDLER_PERIOD_MS      10
#define LVGL_TASK_STACK_SIZE        4096
#define LVGL_TASK_PRIORITY          5

/* 界面对象、面板操作和一次像素传输完成使用独立同步手段，避免长时间发送像素时
 * 阻塞普通界面更新。其余字段只记录睡眠、唤醒和刷新耗时。 */
static SemaphoreHandle_t s_lvgl_mutex;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static esp_timer_handle_t s_tick_timer;
static volatile bool s_display_off;
static volatile bool s_refresh_paused;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_color_done;
static SemaphoreHandle_t s_panel_mutex;
static volatile uint64_t s_flush_count;
static volatile uint64_t s_flush_total_us;
static volatile uint32_t s_flush_max_us;
static volatile int64_t s_wake_started_us;
static volatile bool s_last_flush_was_final;

/**
 * @brief 面板色彩传输完成回调（在 SPI 驱动 ISR 上下文执行，IRAM_ATTR）。
 *
 * @note  挂接为 julia_display.c 里 io_config 的 .on_color_trans_done。它只做一件事：
 *        give 二值信号 s_color_done（FromISR），唤醒在 lvgl_port_draw_bitmap_sync 里
 *        take 等待的 LVGL 任务。返回值表示"是否唤醒了更高优先级任务"，供 SPI 驱动
 *        决定是否触发一次调度（yield）。
 *
 * @context ISR 上下文——不能调用任何会阻塞/拿锁的 API；用户回调需短小。
 * @return true 表示需要让出 CPU（有任务被唤醒），false 表示无需调度。
 */
bool IRAM_ATTR lvgl_port_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io; (void)edata; (void)user_ctx;
    if (!s_color_done) return false;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_color_done, &task_woken);
    return task_woken == pdTRUE;
}

/**
 * @brief 同步绘制一块矩形：提交 DMA 并阻塞等待"色彩传输完成"信号。
 *
 * @note  数据流与同步：先在 s_panel_mutex 下拿面板独占，若 pixels 在外部 RAM（PSRAM），
 *        用 esp_cache_msync 做 C2M cache 一致性同步（确保 DMA 读到的是最新像素）；
 *        再丢弃上一次可能残留的完成信号（xSemaphoreTake(…,0)），随后调用
 *        esp_lcd_panel_draw_bitmap 下发 DMA，最后阻塞等待 ISR 回调 give 的 s_color_done。
 *        这样 LVGL 的 flush 是"发起即等完成"的同步语义，避免复用缓冲区时被 DMA 覆盖。
 *
 * @param[in] panel  面板句柄。
 * @param[in] x1,y1  窗口左上（含）。
 * @param[in] x2,y2  窗口右下（不含），与 LVGL area 传参一致。
 * @param[in] pixels 像素数据（RGB565；内部 DMA RAM 或已同步的 PSRAM）。
 * @return ESP_OK 发送且收到完成；ESP_ERR_TIMEOUT 取锁/等完成超时；
 *         ESP_ERR_INVALID_STATE 未初始化；否则为发送/同步错误。
 */
esp_err_t lvgl_port_draw_bitmap_sync(esp_lcd_panel_handle_t panel, int x1, int y1,
                                     int x2, int y2, const void *pixels)
{
    if (!s_color_done) return ESP_ERR_INVALID_STATE;
    if (!s_panel_mutex || xSemaphoreTake(s_panel_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    size_t bytes = (size_t)(x2 - x1) * (size_t)(y2 - y1) * sizeof(lv_color_t);
    if (esp_ptr_external_ram(pixels)) {
        esp_err_t sync_err = esp_cache_msync((void *)pixels, bytes,
                                             ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                             ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                                             ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (sync_err != ESP_OK) { xSemaphoreGive(s_panel_mutex); return sync_err; }
    }
    xSemaphoreTake(s_color_done, 0);
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, pixels);
    if (err != ESP_OK) { xSemaphoreGive(s_panel_mutex); return err; }
    err = xSemaphoreTake(s_color_done, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    xSemaphoreGive(s_panel_mutex);
    return err;
}

/**
 * @brief LVGL 刷新回调：把 area 区域交给面板并等待完成（同步刷新）。
 *
 * @note  调用上下文：LVGL 任务在持有 s_lvgl_mutex 时调用 lv_timer_handler，
 *        进而触发本回调。它通过 lvgl_port_draw_bitmap_sync 把 area（右/下边缘 +1，
 *        与面板闭区间约定一致）提交 DMA 并同步等待完成；完成后用
 *        lv_disp_flush_is_last 记录当前是否为"整帧最后一块"，再 lv_disp_flush_ready
 *        告知 LVGL 这块缓冲可复用。若处于息屏/暂停态，则直接标记完成以保持 LVGL 流程。
 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    if (s_display_off || s_refresh_paused) {
        lv_disp_flush_ready(drv);
        return;
    }
    if (s_wake_started_us) {
        ESP_LOGI(TAG, "wake first flush latency_ms=%.1f",
                 (double)(esp_timer_get_time() - s_wake_started_us) / 1000.0);
        s_wake_started_us = 0;
    }
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    int64_t started_us = esp_timer_get_time();
    esp_err_t err = lvgl_port_draw_bitmap_sync(panel_handle, area->x1, area->y1,
                                               area->x2 + 1, area->y2 + 1, color_map);
    uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);
    s_flush_count++;
    s_flush_total_us += elapsed_us;
    if (elapsed_us > s_flush_max_us) s_flush_max_us = elapsed_us;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(err));
    }
    s_last_flush_was_final = lv_disp_flush_is_last(drv);
    lv_disp_flush_ready(drv);
}

/**
 * @brief 立即触发一次整屏刷新并等待所有分块完成（同步刷新）。
 *
 * @note  用于立绘首帧等需要"阻塞直到整屏绘制完毕"的场景。实现：取 LVGL 锁 →
 *        复位 last_flush 标记 → lv_refr_now 强制刷新（期间 flush_cb 走同步路径，
 *        每块已在面板上完成）→ 判断是否完整 → 释放锁。返回值体现"最后一块"是否真
 *        被处理，从而判断整屏是否刷新成功。
 *
 * @param[in] timeout_ticks 取锁超时（tick）。
 * @return ESP_OK 整屏刷新完成；ESP_ERR_TIMEOUT 取锁超时；ESP_ERR_INVALID_STATE 无分块刷新。
 */
esp_err_t lvgl_port_refr_now_sync(TickType_t timeout_ticks)
{
    if (!lvgl_port_lock(timeout_ticks)) return ESP_ERR_TIMEOUT;
    s_last_flush_was_final = false;
    lv_refr_now(NULL);
    /* The flush callback uses draw_bitmap_sync, so the last flush has already
     * completed on the panel when lv_refr_now returns. */
    bool complete = s_last_flush_was_final;
    lvgl_port_unlock();
    return complete ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/** @brief 读出累计刷新统计（次数/总耗时/单次最大耗时），任一指针可为 NULL 表示不关心。 */
void lvgl_port_get_flush_metrics(uint64_t *count, uint64_t *total_us, uint32_t *max_us)
{
    if (count) *count = s_flush_count;
    if (total_us) *total_us = s_flush_total_us;
    if (max_us) *max_us = s_flush_max_us;
}

/* LVGL tick 源：由 esp_timer 周期触发（ESP_TIMER_TASK，在定时器任务上下文执行），
 * 每 LVGL_TICK_PERIOD_MS 递增一次 LVGL 内部毫秒计数。 */
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/* LVGL 主循环任务：周期调用 lv_timer_handler（驱动动画/刷新）。在息屏或刷新暂停时
 * 低频空转（200ms）以保持任务可调度、系统 WDT 正常，不持有 LVGL 锁；正常时取锁后
 * 处理一到多次定时器并释放，之后按 LVGL_HANDLER_PERIOD_MS 节拍。 */
static void lvgl_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_display_off || s_refresh_paused) {
            /* 不持有 LVGL 锁，任务保持可调度；系统 idle/task WDT 均可正常运行。 */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (lvgl_port_lock(portMAX_DELAY)) {
            lv_timer_handler();
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(LVGL_HANDLER_PERIOD_MS));
    }
}

/**
 * @brief 开关面板显示（息屏/亮屏）。
 *
 * @note  关屏：先置 s_display_off（阻止新 flush），再在 s_panel_mutex 下调用面板的
 *        disp_on_off(false)。不取 LVGL 锁——它有意与 LVGL 调用序列解耦，避免在
 *        LVGL 任务持有锁时死锁。若面板不支持关屏（返回错误），则退化为"仅关背光"
 *        （backlight-only fallback），仍把开关位置起以停 flush。
 * @note  开屏：同样在 s_panel_mutex 下 disp_on_off(true)，随后记录 s_wake_started_us
 *        于首个 flush 汇报唤醒延迟，最后清 s_display_off。
 *
 * @param[in] off true 关屏；false 开屏。
 * @return ESP_OK 完成；ESP_ERR_INVALID_STATE 面板未初始化；ESP_ERR_TIMEOUT 取面板锁超时；
 *         否则为面板 disp_on_off 错误（已作为警告记录）。
 * @note  本函数禁止用于启动同步：它保持 LVGL 任务存活，并非阻塞式等待。
 */
esp_err_t lvgl_port_set_display_off(bool off)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    if (off == s_display_off) return ESP_OK;
    if (off) {
        /* 先阻止新 flush，再关闭面板；不获取 LVGL mutex。 */
        s_display_off = true;
        if (xSemaphoreTake(s_panel_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, false);
        xSemaphoreGive(s_panel_mutex);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "panel display-off unsupported: %s; backlight-only fallback",
                     esp_err_to_name(err));
        }
        return ESP_OK;
    }
    if (xSemaphoreTake(s_panel_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, true);
    xSemaphoreGive(s_panel_mutex);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "panel display-on unsupported: %s; backlight-only fallback",
                 esp_err_to_name(err));
    s_wake_started_us = esp_timer_get_time();
    s_display_off = false;
    return ESP_OK;
}

bool lvgl_port_display_off(void) { return s_display_off; }

/* "刷新暂停"不同于"息屏"：它只停掉 LVGL 的刷新/动画，但保持 LCD 控制器与 GRAM 供电，
 * 用于"临时冻结画面但保持背光"的场景（见 lvgl_port.h）。 */
void lvgl_port_set_refresh_paused(bool paused) { s_refresh_paused = paused; }
bool lvgl_port_refresh_paused(void) { return s_refresh_paused; }

/**
 * @brief 取 LVGL 递归锁（可重入）。
 * @note  任何访问 LVGL API 的任务都必须先 lock、用完 unlock；允许在同一任务内嵌套。
 * @param[in] timeout_ticks 超时（tick，portMAX_DELAY 可无限等）。
 * @return true 成功持有锁；false 超时。
 */
bool lvgl_port_lock(TickType_t timeout_ticks)
{
    return xSemaphoreTakeRecursive(s_lvgl_mutex, timeout_ticks) == pdTRUE;
}

/** @brief 释放 LVGL 递归锁（与 lvgl_port_lock 成对调用）。 */
void lvgl_port_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

/**
 * @brief 初始化 LVGL 显示端口：建锁/信号量、初始化 LVGL、创建双缓冲、注册显示驱动、
 *        启动 tick 定时器并创建 LVGL 任务。
 *
 * @note  前置条件：panel 已创建并完成 init（由 julia_display_init 保证）。本函数不创建
 *        面板，只做"把面板接到 LVGL"的事。双缓冲分配自内部 DMA 内存（非 PSRAM），
 *        便于直接被 SPI DMA 读取；缓冲区大小 LVGL_PORT_BUFFER_PIXELS（约为整屏 1/10）。
 *
 * @param[in] panel_handle 已初始化好的面板句柄（也作为驱动 user_data 传给 flush_cb）。
 * @return ESP_OK 成功；参数为空 ESP_ERR_INVALID_ARG；信号量/缓冲/任务/timer 任一失败返回错误。
 * @sideeffect 分配两帧双缓冲；启动 esp_timer tick 与独立 lvgl 任务；注册显示驱动。
 */
esp_err_t lvgl_port_init(esp_lcd_panel_handle_t panel_handle)
{
    ESP_RETURN_ON_FALSE(panel_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "panel handle is null");

    s_panel = panel_handle;
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_mutex != NULL, ESP_ERR_NO_MEM, TAG, "lvgl mutex alloc failed");
    s_color_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_color_done != NULL, ESP_ERR_NO_MEM, TAG, "LCD sync semaphore alloc failed");
    s_panel_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_panel_mutex != NULL, ESP_ERR_NO_MEM, TAG, "LCD panel mutex alloc failed");

    lv_init();

    lv_color_t *buf1 = heap_caps_malloc(LVGL_PORT_BUFFER_PIXELS * sizeof(lv_color_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *buf2 = heap_caps_malloc(LVGL_PORT_BUFFER_PIXELS * sizeof(lv_color_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "lvgl draw buffer alloc failed");

    memset(buf1, 0, LVGL_PORT_BUFFER_PIXELS * sizeof(lv_color_t));
    memset(buf2, 0, LVGL_PORT_BUFFER_PIXELS * sizeof(lv_color_t));

    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, LVGL_PORT_BUFFER_PIXELS);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LVGL_PORT_HOR_RES;
    s_disp_drv.ver_res = LVGL_PORT_VER_RES;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&s_disp_drv);

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = false,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &s_tick_timer), TAG, "lvgl tick timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_tick_timer, LVGL_TICK_PERIOD_MS * 1000), TAG, "lvgl tick timer start failed");

    BaseType_t task_ok = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "lvgl task create failed");

    ESP_LOGI(TAG, "LVGL initialized with %d-pixel double buffer", LVGL_PORT_BUFFER_PIXELS);
    return ESP_OK;
}
