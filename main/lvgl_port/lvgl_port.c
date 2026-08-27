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

bool IRAM_ATTR lvgl_port_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io; (void)edata; (void)user_ctx;
    if (!s_color_done) return false;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_color_done, &task_woken);
    return task_woken == pdTRUE;
}

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

void lvgl_port_get_flush_metrics(uint64_t *count, uint64_t *total_us, uint32_t *max_us)
{
    if (count) *count = s_flush_count;
    if (total_us) *total_us = s_flush_total_us;
    if (max_us) *max_us = s_flush_max_us;
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

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

void lvgl_port_set_refresh_paused(bool paused) { s_refresh_paused = paused; }
bool lvgl_port_refresh_paused(void) { return s_refresh_paused; }

bool lvgl_port_lock(TickType_t timeout_ticks)
{
    return xSemaphoreTakeRecursive(s_lvgl_mutex, timeout_ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

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
