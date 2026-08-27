#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#define LVGL_PORT_HOR_RES           360
#define LVGL_PORT_VER_RES           360
#define LVGL_PORT_BUFFER_PIXELS     (LVGL_PORT_HOR_RES * LVGL_PORT_VER_RES / 10)

esp_err_t lvgl_port_init(esp_lcd_panel_handle_t panel_handle);
bool lvgl_port_lock(TickType_t timeout_ticks);
void lvgl_port_unlock(void);
/* 息屏专用：任务保持存活，handler/flush 低占空跳过，禁止用于启动同步。 */
esp_err_t lvgl_port_set_display_off(bool off);
bool lvgl_port_display_off(void);
/* Pause LVGL timers/flushes while keeping the LCD controller and GRAM on. */
void lvgl_port_set_refresh_paused(bool paused);
bool lvgl_port_refresh_paused(void);
bool lvgl_port_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
esp_err_t lvgl_port_draw_bitmap_sync(esp_lcd_panel_handle_t panel, int x1, int y1,
                                     int x2, int y2, const void *pixels);
void lvgl_port_get_flush_metrics(uint64_t *count, uint64_t *total_us, uint32_t *max_us);
esp_err_t lvgl_port_refr_now_sync(TickType_t timeout_ticks);
