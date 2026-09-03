/**
 * @file    lvgl_port.h
 * @brief   把 LVGL 画面安全送到 LCD，并防止多个任务同时修改界面或面板。
 *
 * @note  必须先完成 LCD 初始化，再启动本模块。LVGL 不是多任务安全的，任何直接
 *        修改界面对象的代码都必须先取得本模块的界面锁。
 * @note  “关闭显示”会让面板停止显示；“暂停刷新”只冻结当前画面，面板内容和背光
 *        仍然保留。两者用途不同，不能互换。
 * @see   main/display/julia_display.h（面板创建与初始化顺序）
 */
#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

/* 屏幕为 360×360；使用两块约十分之一屏的缓冲交替刷新，降低连续内存占用。 */
#define LVGL_PORT_HOR_RES           360
#define LVGL_PORT_VER_RES           360
#define LVGL_PORT_BUFFER_PIXELS     (LVGL_PORT_HOR_RES * LVGL_PORT_VER_RES / 10)

esp_err_t lvgl_port_init(esp_lcd_panel_handle_t panel_handle);
bool lvgl_port_lock(TickType_t timeout_ticks);
void lvgl_port_unlock(void);
/* 进入睡眠显示时使用；界面任务仍存活，但不再持续生成和发送新画面。 */
esp_err_t lvgl_port_set_display_off(bool off);
bool lvgl_port_display_off(void);
/* 冻结动画和刷新但保留当前画面，适合短时独占显示，不代表屏幕已经关闭。 */
void lvgl_port_set_refresh_paused(bool paused);
bool lvgl_port_refresh_paused(void);
bool lvgl_port_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
esp_err_t lvgl_port_draw_bitmap_sync(esp_lcd_panel_handle_t panel, int x1, int y1,
                                     int x2, int y2, const void *pixels);
void lvgl_port_get_flush_metrics(uint64_t *count, uint64_t *total_us, uint32_t *max_us);
esp_err_t lvgl_port_refr_now_sync(TickType_t timeout_ticks);
