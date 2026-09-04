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

/* 1/10 屏是当前内存／刷新折中，不是 LVGL 或面板协议要求。 */
#define LVGL_PORT_HOR_RES           360
#define LVGL_PORT_VER_RES           360
#define LVGL_PORT_BUFFER_PIXELS     (LVGL_PORT_HOR_RES * LVGL_PORT_VER_RES / 10)

/** 创建双缓冲、同步对象、LVGL task 和 tick timer；失败也可能留下资源，不可直接重试。 */
esp_err_t lvgl_port_init(esp_lcd_panel_handle_t panel_handle);
/** init 成功后取得递归 LVGL mutex；timeout_ticks 使用 FreeRTOS tick。 */
bool lvgl_port_lock(TickType_t timeout_ticks);
/** 只能由持有递归 LVGL mutex 的同一任务配对调用。 */
void lvgl_port_unlock(void);
/* 只控制 panel 显示和 flush 门控，不控制背光，也不销毁 LVGL task。 */
esp_err_t lvgl_port_set_display_off(bool off);
bool lvgl_port_display_off(void);
/* 冻结动画和刷新但保留当前画面，适合短时独占显示，不代表屏幕已经关闭。 */
void lvgl_port_set_refresh_paused(bool paused);
bool lvgl_port_refresh_paused(void);
/** SPI ISR callback：只发送完成信号，不得访问 LVGL 对象。 */
bool lvgl_port_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
/** 同步提交半开坐标矩形；pixels 在函数返回前必须有效，调用者不得持有 panel mutex。 */
esp_err_t lvgl_port_draw_bitmap_sync(esp_lcd_panel_handle_t panel, int x1, int y1,
                                     int x2, int y2, const void *pixels);
/** 返回无锁统计快照；三个输出指针均可为 NULL。 */
void lvgl_port_get_flush_metrics(uint64_t *count, uint64_t *total_us, uint32_t *max_us);
/** 在递归 LVGL mutex 内请求立即刷新；成功表示同步 flush 已返回。 */
esp_err_t lvgl_port_refr_now_sync(TickType_t timeout_ticks);
