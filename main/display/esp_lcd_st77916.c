/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file    esp_lcd_st77916.c
 * @brief   ST77916 TFT LCD 面板驱动的 ESP-IDF 实现（esp_lcd_panel_t 接口）。
 *
 * @section drv_bounds 模块边界
 *         本文件是"通用面板驱动"层：只关心如何把一帧像素交给 ST77916 并在其上做
 *         命令/参数往返，以及基础的复位/开窗/几何变换/开关显示。它不持有任何板级
 *         引脚或背光信息（RST 引脚号与活动电平由 panel_dev_config 传入），也不依赖
 *         LVGL。板级引脚、SPI 总线、I2C 复位、背光由 julia_display.c / lvgl_port.c 提供。
 *
 * @section drv_iface 与 ESP-IDF 的契约
 *         driver 通过 `esp_lcd_panel_io_*`（esp_lcd_panel_io_spi）与面板通信，实现
 *         `esp_lcd_panel_t` 接口回调：reset/init/draw_bitmap/invert_color/mirror/
 *         swap_xy/set_gap/disp_on_off/del。这些回调的调用顺序由上层遵循
 *         panel → reset → init → disp_on →（此后可 draw_bitmap）的确立流程约束。
 *
 * @section drv_qspi QSPI 特性
 *         QSPI（use_qspi_interface=1）下，命令字被扩展成 32-bit，把协议 opcode
 *         放入高字节、LCD 命令放入 bits[15:8]，从而让 esp_lcd_panel_io_spi 在
 *         无 DC 引脚的情况下正确路由"写命令/读命令/写颜色"事务（见 tx_param/tx_color）。
 *
 * @note   初始化序列与寄存器含义是面板厂商相关的；驱动不假设寄存器语义，只按
 *         （可覆盖的）命令表逐条下发。延时与个别寄存器含义无法在本层确认，见
 *         vendor_specific_init_default 附近说明。
 *
 * @see    esp_lcd_st77916.h
 * @see    main/display/julia_display.c
 */
#include <stdlib.h>
#include <sys/cdefs.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_log.h"

#include "esp_lcd_st77916.h"

/* QSPI 协议 opcode：非 QSPI 模式下，esp_lcd_panel_io_spi 通过 DC 引脚区分命令与数据；
 * 而在 quad_mode（32-bit 命令字）下，命令字的高字节携带这些 opcode，告诉 LCD 控制器
 * 这一事务是"写命令/读命令/写颜色"，从而把指令路由到正确的协议阶段。低字节才放 LCD
 * 命令码（见 tx_param/tx_color）。 */
#define LCD_OPCODE_WRITE_CMD        (0x02ULL)
#define LCD_OPCODE_READ_CMD         (0x0BULL)
#define LCD_OPCODE_WRITE_COLOR      (0x32ULL)

/* ST77916 厂商"命令集"切换命令及其子入口：有些厂商初始化序列是分段的，先用
 * CMD_SET=0xF0 进入扩展命令组，下发子命令组后用 PARAM_SET=0x00 退出。驱动依赖
 * 这一约定来判定当前是否处于"扩展命令集"（见 panel_st77916_init）。 */
#define ST77916_CMD_SET             (0xF0)
#define ST77916_PARAM_SET           (0x00)

static const char *TAG = "st77916";

static esp_err_t panel_st77916_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st77916_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st77916_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st77916_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_st77916_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st77916_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st77916_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st77916_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st77916_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save surrent value of LCD_CMD_COLMOD register
    const st77916_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int use_qspi_interface: 1;
        unsigned int reset_level: 1;
    } flags;
} st77916_panel_t;

/**
 * @brief 创建一个 ST77916 面板实例并返回通用面板句柄。
 *
 * @note  只做"分配 + 记录配置 + 挂回调"，不发送任何 SPI/i2c 事务；真正的
 *        数据搬运要等调用方随后调用 reset/init。
 *
 * @param[in]  io 已初始化好的面板 IO（esp_lcd_panel_io_handle_t）。
 * @param[in]  panel_dev_config 面板设备配置：RST GPIO、RGB/BGR 顺序、像素位宽、
 *               以及可选的 vendor_config（厂商初始化命令覆盖 + QSPI 开关）。
 * @param[out] ret_panel 返回的面板句柄，失败时不写。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG io/配置/ret_panel 为空；
 *         ESP_ERR_NO_MEM 结构分配失败；ESP_ERR_NOT_SUPPORTED 颜色序或像素位宽不支持。
 *
 * @pre  io 已由 esp_lcd_new_panel_io_spi() 创建，SPI 总线已初始化。
 * @sideeffect 若 reset_gpio_num >= 0，会将该引脚配置为推挽输出（不拉电平）。
 *         失败时若已配置 RST 引脚会 `gpio_reset_pin` 并释放结构。
 */
esp_err_t esp_lcd_new_panel_st77916(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    esp_err_t ret = ESP_OK;
    st77916_panel_t *st77916 = NULL;
    st77916 = calloc(1, sizeof(st77916_panel_t));
    ESP_GOTO_ON_FALSE(st77916, ESP_ERR_NO_MEM, err, TAG, "no mem for st77916 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st77916->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st77916->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color element order");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st77916->colmod_val = 0x55;
        st77916->fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        st77916->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        st77916->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st77916->io = io;
    st77916->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st77916->flags.reset_level = panel_dev_config->flags.reset_active_high;
    st77916_vendor_config_t *vendor_config = (st77916_vendor_config_t *)panel_dev_config->vendor_config;
    if (vendor_config) {
        st77916->init_cmds = vendor_config->init_cmds;
        st77916->init_cmds_size = vendor_config->init_cmds_size;
        st77916->flags.use_qspi_interface = vendor_config->flags.use_qspi_interface;
    }
    st77916->base.del = panel_st77916_del;
    st77916->base.reset = panel_st77916_reset;
    st77916->base.init = panel_st77916_init;
    st77916->base.draw_bitmap = panel_st77916_draw_bitmap;
    st77916->base.invert_color = panel_st77916_invert_color;
    st77916->base.set_gap = panel_st77916_set_gap;
    st77916->base.mirror = panel_st77916_mirror;
    st77916->base.swap_xy = panel_st77916_swap_xy;
    st77916->base.disp_on_off = panel_st77916_disp_on_off;
    *ret_panel = &(st77916->base);
    ESP_LOGD(TAG, "new st77916 panel @%p", st77916);

    // ESP_LOGI(TAG, "LCD panel create success, version: %d.%d.%d", ESP_LCD_ST77916_VER_MAJOR, ESP_LCD_ST77916_VER_MINOR, ESP_LCD_ST77916_VER_PATCH);

    return ESP_OK;

err:
    if (st77916) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st77916);
    }
    return ret;
}

/* 发送一个 LCD 参数/命令。标准 SPI 下直接透传命令码；QSPI 下把命令码左移 8 位并入
 * 高字节 opcode，组成 32-bit 命令字交 esp_lcd_panel_io_tx_param 处理。 */
static esp_err_t tx_param(st77916_panel_t *st77916, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    if (st77916->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
    }
    return esp_lcd_panel_io_tx_param(io, lcd_cmd, param, param_size);
}

/* 发送 LCD 颜色（像素）数据。与 tx_param 的区别仅是 opcode 用 WRITE_COLOR（0x32），
 * 让面板进入传递像素数据的状态（配合先发的 RAMWR 命令）。 */
static esp_err_t tx_color(st77916_panel_t *st77916, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    if (st77916->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_COLOR << 24;
    }
    return esp_lcd_panel_io_tx_color(io, lcd_cmd, param, param_size);
}

/**
 * @brief 释放面板实例：若曾配置 RST 引脚则复位该引脚并释放结构。
 * @note  不主动关闭面板/回填任何发送状态；调用前上层应已做完垃圾收尾。
 */
static esp_err_t panel_st77916_del(esp_lcd_panel_t *panel)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);

    if (st77916->reset_gpio_num >= 0) {
        gpio_reset_pin(st77916->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st77916 panel @%p", st77916);
    free(st77916);
    return ESP_OK;
}

/**
 * @brief 复位 ST77916（硬件或软件）。
 *
 * @note  时序说明：
 *         - 硬件路径：RST 拉低 >=10ms 再拉高，随后等待 120ms 让面板完成上电内部
 *           初始化（此延时来自面板 datasheet，是上电/复位后显示可用的最短时间）。
 *         - 软件路径：发送 SWRESET(0x01) 命令 + 等 120ms，用于 RST 引脚未接的场合。
 * @pre  io 已初始化；若走软件复位则要求 SPI 已就绪。
 * @sideeffect 翻转 RST 引脚电平（若配置了）；阻塞当前任务数十至百余毫秒。
 */
static esp_err_t panel_st77916_reset(esp_lcd_panel_t *panel)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = st77916->io;

    // Perform hardware reset
    if (st77916->reset_gpio_num >= 0) {
        gpio_set_level(st77916->reset_gpio_num, st77916->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st77916->reset_gpio_num, !st77916->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else { // Perform software reset
        ESP_RETURN_ON_ERROR(tx_param(st77916, io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

/* 厂商特定初始化序列（vendor_specific_init_default）。
 *
 * 作用：ST77916 不同的批次/模组厂商对电源、Gamma、扫描方向等寄存器取值不同，
 *       因此不能在驱动里假设一组"通用"值，而是把整套命令表作为基线序列下发，
 *       调用方也可通过 vendor_config.init_cmds 整体覆盖。
 *
 * 下面这一段（~0xF0~0xFF 组合）实际上是一份"已完成伽马调校、可点亮本模组"的序列。
 * 数组里被 `//` 注释掉的若干行是调试/备选值（不同亮度、相位、扫描顺序的变体），
 * 保留它们是为了后续烧录调参时能快速对比，而不必重新查阅厂商手册。
 *
 * 语义说明（不逐字节展开）：
 *   - 0xF0/0xF1/0xF2 ... 多数是厂商"命令扩展组"入口与电源/时序/伽马配置寄存器；
 *   - 0x21 进入反显、0x11 睡眠退出、0x29 打开显示——全屏正常显示所必需的"开关"指令；
 *   - 0x11 后带 120ms 延时：让面板从睡眠退出并完成内部自举（datasheet 要求）。
 *
 * NOTE：需结合调用方/模组供应商确认：个别寄存器位（如 0xE0/0xE1 伽马表）的精确含义，
 *       本层仅负责按序透传，不解读其色温/伽马语义。
 */
static const st77916_lcd_init_cmd_t vendor_specific_init_default[] = {
    // {0xF0, (uint8_t []){0x08}, 1, 0},
    // {0xF2, (uint8_t []){0x08}, 1, 0},
    // {0x9B, (uint8_t []){0x51}, 1, 0},
    // {0x86, (uint8_t []){0x53}, 1, 0},
    // {0xF2, (uint8_t []){0x80}, 1, 0},
    // {0xF0, (uint8_t []){0x00}, 1, 0},
    // {0xF0, (uint8_t []){0x01}, 1, 0},
    // {0xF1, (uint8_t []){0x01}, 1, 0},
    // {0xB0, (uint8_t []){0x54}, 1, 0},
    // {0xB1, (uint8_t []){0x3F}, 1, 0},
    // {0xB2, (uint8_t []){0x2A}, 1, 0},
    // {0xB4, (uint8_t []){0x46}, 1, 0},
    // {0xB5, (uint8_t []){0x34}, 1, 0},
    // {0xB6, (uint8_t []){0xD5}, 1, 0},
    // {0xB7, (uint8_t []){0x30}, 1, 0},
    // // {0xB8, (uint8_t []){0x04}, 1, 0},
    // {0xBA, (uint8_t []){0x00}, 1, 0},
    // {0xBB, (uint8_t []){0x08}, 1, 0},
    // {0xBC, (uint8_t []){0x08}, 1, 0},
    // {0xBD, (uint8_t []){0x00}, 1, 0},
    // {0xC0, (uint8_t []){0x80}, 1, 0},
    // {0xC1, (uint8_t []){0x10}, 1, 0},
    // {0xC2, (uint8_t []){0x37}, 1, 0},
    // {0xC3, (uint8_t []){0x80}, 1, 0},
    // {0xC4, (uint8_t []){0x10}, 1, 0},
    // {0xC5, (uint8_t []){0x37}, 1, 0},
    // {0xC6, (uint8_t []){0xA9}, 1, 0},
    // {0xC7, (uint8_t []){0x41}, 1, 0},
    // {0xC8, (uint8_t []){0x51}, 1, 0},
    // {0xC9, (uint8_t []){0xA9}, 1, 0},
    // {0xCA, (uint8_t []){0x41}, 1, 0},
    // {0xCB, (uint8_t []){0x51}, 1, 0},
    // {0xD0, (uint8_t []){0x91}, 1, 0},
    // {0xD1, (uint8_t []){0x68}, 1, 0},
    // {0xD2, (uint8_t []){0x69}, 1, 0},
    // {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    // // {0xDD, (uint8_t []){0x35}, 1, 0},
    // // {0xDE, (uint8_t []){0x35}, 1, 0},
    // {0xDD, (uint8_t []){0x3F}, 1, 0},
    // {0xDE, (uint8_t []){0x3F}, 1, 0},
    // {0xF1, (uint8_t []){0x10}, 1, 0},
    // {0xF0, (uint8_t []){0x00}, 1, 0},
    // {0xF0, (uint8_t []){0x02}, 1, 0},
    // // {0xE0, (uint8_t []){0x70, 0x09, 0x12, 0x0C, 0x0B, 0x27, 0x38, 0x54, 0x4E, 0x19, 0x15, 0x15, 0x2C, 0x2F}, 14, 0},
    // // {0xE1, (uint8_t []){0x70, 0x08, 0x11, 0x0C, 0x0B, 0x27, 0x38, 0x43, 0x4C, 0x18, 0x14, 0x14, 0x2B, 0x2D}, 14, 0},
    // {0xE0, (uint8_t []){0xF0, 0x06, 0x0B, 0x09, 0x09, 0x16, 0x32, 0x44, 0x4A, 0x37, 0x13, 0x13, 0x2E, 0x34}, 14, 0},
    // {0xE1, (uint8_t []){0xF0, 0x06, 0x0B, 0x09, 0x08, 0x05, 0x32, 0x33, 0x49, 0x17, 0x13, 0x13, 0x2E, 0x34}, 14, 0},
    // {0xF0, (uint8_t []){0x10}, 1, 0},
    // {0xF3, (uint8_t []){0x10}, 1, 0},
    // {0xE0, (uint8_t []){0x0A}, 1, 0},
    // {0xE1, (uint8_t []){0x00}, 1, 0},
    // {0xE2, (uint8_t []){0x00}, 1, 0},
    // {0xE3, (uint8_t []){0x00}, 1, 0},
    // {0xE4, (uint8_t []){0xE0}, 1, 0},
    // {0xE5, (uint8_t []){0x06}, 1, 0},
    // {0xE6, (uint8_t []){0x21}, 1, 0},
    // {0xE7, (uint8_t []){0x00}, 1, 0},
    // {0xE8, (uint8_t []){0x05}, 1, 0},
    // {0xE9, (uint8_t []){0x82}, 1, 0},
    // {0xEA, (uint8_t []){0xDF}, 1, 0},
    // {0xEB, (uint8_t []){0x89}, 1, 0},
    // {0xEC, (uint8_t []){0x20}, 1, 0},
    // {0xED, (uint8_t []){0x14}, 1, 0},
    // {0xEE, (uint8_t []){0xFF}, 1, 0},
    // {0xEF, (uint8_t []){0x00}, 1, 0},
    // {0xF8, (uint8_t []){0xFF}, 1, 0},
    // {0xF9, (uint8_t []){0x00}, 1, 0},
    // {0xFA, (uint8_t []){0x00}, 1, 0},
    // {0xFB, (uint8_t []){0x30}, 1, 0},
    // {0xFC, (uint8_t []){0x00}, 1, 0},
    // {0xFD, (uint8_t []){0x00}, 1, 0},
    // {0xFE, (uint8_t []){0x00}, 1, 0},
    // {0xFF, (uint8_t []){0x00}, 1, 0},
    // {0x60, (uint8_t []){0x42}, 1, 0},
    // {0x61, (uint8_t []){0xE0}, 1, 0},
    // {0x62, (uint8_t []){0x40}, 1, 0},
    // {0x63, (uint8_t []){0x40}, 1, 0},
    // {0x64, (uint8_t []){0x02}, 1, 0},
    // {0x65, (uint8_t []){0x00}, 1, 0},
    // {0x66, (uint8_t []){0x40}, 1, 0},
    // {0x67, (uint8_t []){0x03}, 1, 0},
    // {0x68, (uint8_t []){0x00}, 1, 0},
    // {0x69, (uint8_t []){0x00}, 1, 0},
    // {0x6A, (uint8_t []){0x00}, 1, 0},
    // {0x6B, (uint8_t []){0x00}, 1, 0},
    // {0x70, (uint8_t []){0x42}, 1, 0},
    // {0x71, (uint8_t []){0xE0}, 1, 0},
    // {0x72, (uint8_t []){0x40}, 1, 0},
    // {0x73, (uint8_t []){0x40}, 1, 0},
    // {0x74, (uint8_t []){0x02}, 1, 0},
    // {0x75, (uint8_t []){0x00}, 1, 0},
    // {0x76, (uint8_t []){0x40}, 1, 0},
    // {0x77, (uint8_t []){0x03}, 1, 0},
    // {0x78, (uint8_t []){0x00}, 1, 0},
    // {0x79, (uint8_t []){0x00}, 1, 0},
    // {0x7A, (uint8_t []){0x00}, 1, 0},
    // {0x7B, (uint8_t []){0x00}, 1, 0},
    // // {0x80, (uint8_t []){0x38}, 1, 0},
    // {0x80, (uint8_t []){0x48}, 1, 0},
    // {0x81, (uint8_t []){0x00}, 1, 0},
    // // {0x82, (uint8_t []){0x04}, 1, 0},
    // {0x82, (uint8_t []){0x05}, 1, 0},
    // {0x83, (uint8_t []){0x02}, 1, 0},
    // // {0x84, (uint8_t []){0xDC}, 1, 0},
    // {0x84, (uint8_t []){0xDD}, 1, 0},
    // {0x85, (uint8_t []){0x00}, 1, 0},
    // {0x86, (uint8_t []){0x00}, 1, 0},
    // {0x87, (uint8_t []){0x00}, 1, 0},
    // // {0x88, (uint8_t []){0x38}, 1, 0},
    // {0x88, (uint8_t []){0x48}, 1, 0},
    // {0x89, (uint8_t []){0x00}, 1, 0},
    // // {0x8A, (uint8_t []){0x06}, 1, 0},
    // {0x8A, (uint8_t []){0x07}, 1, 0},
    // {0x8B, (uint8_t []){0x02}, 1, 0},
    // // {0x8C, (uint8_t []){0xDE}, 1, 0},
    // {0x8C, (uint8_t []){0xDF}, 1, 0},
    // {0x8D, (uint8_t []){0x00}, 1, 0},
    // {0x8E, (uint8_t []){0x00}, 1, 0},
    // {0x8F, (uint8_t []){0x00}, 1, 0},
    // // {0x90, (uint8_t []){0x38}, 1, 0},
    // {0x90, (uint8_t []){0x48}, 1, 0},
    // {0x91, (uint8_t []){0x00}, 1, 0},
    // // {0x92, (uint8_t []){0x08}, 1, 0},
    // {0x92, (uint8_t []){0x09}, 1, 0},
    // {0x93, (uint8_t []){0x02}, 1, 0},
    // // {0x94, (uint8_t []){0xE0}, 1, 0},
    // {0x94, (uint8_t []){0xE1}, 1, 0},
    // {0x95, (uint8_t []){0x00}, 1, 0},
    // {0x96, (uint8_t []){0x00}, 1, 0},
    // {0x97, (uint8_t []){0x00}, 1, 0},
    // // {0x98, (uint8_t []){0x38}, 1, 0},
    // {0x98, (uint8_t []){0x48}, 1, 0},
    // {0x99, (uint8_t []){0x00}, 1, 0},
    // // {0x9A, (uint8_t []){0x0A}, 1, 0},
    // {0x9A, (uint8_t []){0x0B}, 1, 0},
    // {0x9B, (uint8_t []){0x02}, 1, 0},
    // // {0x9C, (uint8_t []){0xE2}, 1, 0},
    // {0x9C, (uint8_t []){0xE3}, 1, 0},
    // {0x9D, (uint8_t []){0x00}, 1, 0},
    // {0x9E, (uint8_t []){0x00}, 1, 0},
    // {0x9F, (uint8_t []){0x00}, 1, 0},
    // // {0xA0, (uint8_t []){0x38}, 1, 0},
    // {0xA0, (uint8_t []){0x48}, 1, 0},
    // {0xA1, (uint8_t []){0x00}, 1, 0},
    // // {0xA2, (uint8_t []){0x03}, 1, 0},
    // {0xA2, (uint8_t []){0x04}, 1, 0},
    // {0xA3, (uint8_t []){0x02}, 1, 0},
    // // {0xA4, (uint8_t []){0xDB}, 1, 0},
    // {0xA4, (uint8_t []){0xDC}, 1, 0},
    // {0xA5, (uint8_t []){0x00}, 1, 0},
    // {0xA6, (uint8_t []){0x00}, 1, 0},
    // {0xA7, (uint8_t []){0x00}, 1, 0},
    // // {0xA8, (uint8_t []){0x38}, 1, 0},
    // {0xA8, (uint8_t []){0x48}, 1, 0},
    // {0xA9, (uint8_t []){0x00}, 1, 0},
    // // {0xAA, (uint8_t []){0x05}, 1, 0},
    // {0xAA, (uint8_t []){0x06}, 1, 0},
    // {0xAB, (uint8_t []){0x02}, 1, 0},
    // // {0xAC, (uint8_t []){0xDD}, 1, 0},
    // {0xAC, (uint8_t []){0xDE}, 1, 0},
    // {0xAD, (uint8_t []){0x00}, 1, 0},
    // {0xAE, (uint8_t []){0x00}, 1, 0},
    // {0xAF, (uint8_t []){0x00}, 1, 0},
    // // {0xB0, (uint8_t []){0x38}, 1, 0},
    // {0xB0, (uint8_t []){0x48}, 1, 0},
    // {0xB1, (uint8_t []){0x00}, 1, 0},
    // // {0xB2, (uint8_t []){0x07}, 1, 0},
    // {0xB2, (uint8_t []){0x08}, 1, 0},
    // {0xB3, (uint8_t []){0x02}, 1, 0},
    // // {0xB4, (uint8_t []){0xDF}, 1, 0},
    // {0xB4, (uint8_t []){0xE0}, 1, 0},
    // {0xB5, (uint8_t []){0x00}, 1, 0},
    // {0xB6, (uint8_t []){0x00}, 1, 0},
    // {0xB7, (uint8_t []){0x00}, 1, 0},
    // // {0xB8, (uint8_t []){0x38}, 1, 0},
    // {0xB8, (uint8_t []){0x48}, 1, 0},
    // {0xB9, (uint8_t []){0x00}, 1, 0},
    // // {0xBA, (uint8_t []){0x09}, 1, 0},
    // {0xBA, (uint8_t []){0x0A}, 1, 0},
    // {0xBB, (uint8_t []){0x02}, 1, 0},
    // // {0xBC, (uint8_t []){0xE1}, 1, 0},
    // {0xBC, (uint8_t []){0xE2}, 1, 0},
    // {0xBD, (uint8_t []){0x00}, 1, 0},
    // {0xBE, (uint8_t []){0x00}, 1, 0},
    // {0xBF, (uint8_t []){0x00}, 1, 0},
    // // {0xC0, (uint8_t []){0x22}, 1, 0},
    // {0xC0, (uint8_t []){0x12}, 1, 0},
    // {0xC1, (uint8_t []){0xAA}, 1, 0},
    // {0xC2, (uint8_t []){0x65}, 1, 0},
    // {0xC3, (uint8_t []){0x74}, 1, 0},
    // {0xC4, (uint8_t []){0x47}, 1, 0},
    // {0xC5, (uint8_t []){0x56}, 1, 0},
    // {0xC6, (uint8_t []){0x00}, 1, 0},
    // {0xC7, (uint8_t []){0x88}, 1, 0},
    // {0xC8, (uint8_t []){0x99}, 1, 0},
    // {0xC9, (uint8_t []){0x33}, 1, 0},
    // // {0xD0, (uint8_t []){0x11}, 1, 0},
    // {0xD0, (uint8_t []){0x21}, 1, 0},
    // {0xD1, (uint8_t []){0xAA}, 1, 0},
    // {0xD2, (uint8_t []){0x65}, 1, 0},
    // {0xD3, (uint8_t []){0x74}, 1, 0},
    // {0xD4, (uint8_t []){0x47}, 1, 0},
    // {0xD5, (uint8_t []){0x56}, 1, 0},
    // {0xD6, (uint8_t []){0x00}, 1, 0},
    // {0xD7, (uint8_t []){0x88}, 1, 0},
    // {0xD8, (uint8_t []){0x99}, 1, 0},
    // {0xD9, (uint8_t []){0x33}, 1, 0},
    // {0xF3, (uint8_t []){0x01}, 1, 0},
    // {0xF0, (uint8_t []){0x00}, 1, 0},
    // {0xF0, (uint8_t []){0x01}, 1, 0},
    // {0xF1, (uint8_t []){0x01}, 1, 0},
    // {0xA0, (uint8_t []){0x0B}, 1, 0},
    // {0xA3, (uint8_t []){0x2A}, 1, 0},
    // {0xA5, (uint8_t []){0xC3}, 1, 1},
    // {0xA3, (uint8_t []){0x2B}, 1, 0},
    // {0xA5, (uint8_t []){0xC3}, 1, 1},
    // {0xA3, (uint8_t []){0x2C}, 1, 0},
    // {0xA5, (uint8_t []){0xC3}, 1, 1},
    // {0xA3, (uint8_t []){0x2D}, 1, 0},
    // {0xA5, (uint8_t []){0xC3}, 1, 1},
    // {0xA3, (uint8_t []){0x2E}, 1, 0},
    // {0xA5, (uint8_t []){0xC3}, 1, 1},
    // {0xA3, (uint8_t []){0x2F}, 1, 0},
    // {0xA5, (uint8_t []){0xC3}, 1, 1},
    // {0xA3, (uint8_t []){0x30}, 1, 0},
    // {0xA5, (uint8_t []){0xC3}, 1, 1},
    // {0xA3, (uint8_t []){0x31}, 1, 0},
    // {0xA5, (uint8_t []){0xC3}, 1, 1},
    // {0xA3, (uint8_t []){0x32}, 1, 0},
    // {0xA5, (uint8_t []){0xC3}, 1, 1},
    // {0xA3, (uint8_t []){0x33}, 1, 0},
    // {0xA5, (uint8_t []){0xC3}, 1, 1},
    // {0xA0, (uint8_t []){0x09}, 1, 0},
    // {0xF1, (uint8_t []){0x10}, 1, 0},
    // {0xF0, (uint8_t []){0x00}, 1, 0},
    // {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0x67}, 4, 0},
    // {0x2B, (uint8_t []){0x01, 0x68, 0x01, 0x68}, 4, 0},
    // {0x4D, (uint8_t []){0x00}, 1, 0},
    // {0x4E, (uint8_t []){0x00}, 1, 0},
    // {0x4F, (uint8_t []){0x00}, 1, 0},
    // {0x4C, (uint8_t []){0x01}, 1, 10},
    // {0x4C, (uint8_t []){0x00}, 1, 0},
    // {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0x67}, 4, 0},
    // {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0x67}, 4, 0},
    // // {0x3A, (uint8_t []){0x55}, 1, 0},
    // {0x21, (uint8_t []){0x00}, 0, 0},
    // {0x11, (uint8_t []){0x00}, 0, 120},
    // {0x29, (uint8_t []){0x00}, 0, 0},
    // ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // {0xF0, (uint8_t []){0x28}, 1, 0},
    // {0xF2, (uint8_t []){0x28}, 1, 0},
    // {0x7C, (uint8_t []){0xD1}, 1, 0},
    // {0x83, (uint8_t []){0xE0}, 1, 0},
    // {0x84, (uint8_t []){0x61}, 1, 0},
    // {0xF2, (uint8_t []){0x82}, 1, 0},
    // {0xF0, (uint8_t []){0x00}, 1, 0},
    // {0xF0, (uint8_t []){0x01}, 1, 0},
    // {0xF1, (uint8_t []){0x01}, 1, 0},
    // {0xB0, (uint8_t []){0x49}, 1, 0},
    // {0xB1, (uint8_t []){0x4A}, 1, 0},
    // {0xB2, (uint8_t []){0x1F}, 1, 0},
    // {0xB4, (uint8_t []){0x46}, 1, 0},
    // {0xB5, (uint8_t []){0x34}, 1, 0},
    // {0xB6, (uint8_t []){0xD5}, 1, 0},
    // {0xB7, (uint8_t []){0x30}, 1, 0},
    // {0xB8, (uint8_t []){0x04}, 1, 0},
    // {0xBA, (uint8_t []){0x00}, 1, 0},
    // {0xBB, (uint8_t []){0x08}, 1, 0},
    // {0xBC, (uint8_t []){0x08}, 1, 0},
    // {0xBD, (uint8_t []){0x00}, 1, 0},
    // {0xC0, (uint8_t []){0x80}, 1, 0},
    // {0xC1, (uint8_t []){0x10}, 1, 0},
    // {0xC2, (uint8_t []){0x37}, 1, 0},
    // {0xC3, (uint8_t []){0x80}, 1, 0},
    // {0xC4, (uint8_t []){0x10}, 1, 0},
    // {0xC5, (uint8_t []){0x37}, 1, 0},
    // {0xC6, (uint8_t []){0xA9}, 1, 0},
    // {0xC7, (uint8_t []){0x41}, 1, 0},
    // {0xC8, (uint8_t []){0x01}, 1, 0},
    // {0xC9, (uint8_t []){0xA9}, 1, 0},
    // {0xCA, (uint8_t []){0x41}, 1, 0},
    // {0xCB, (uint8_t []){0x01}, 1, 0},
    // {0xD0, (uint8_t []){0x91}, 1, 0},
    // {0xD1, (uint8_t []){0x68}, 1, 0},
    // {0xD2, (uint8_t []){0x68}, 1, 0},
    // {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    // // {0xDD, (uint8_t []){0x35}, 1, 0},
    // // {0xDE, (uint8_t []){0x35}, 1, 0},
    // // {0xDD, (uint8_t []){0x3F}, 1, 0},
    // // {0xDE, (uint8_t []){0x3F}, 1, 0},
    // {0xF1, (uint8_t []){0x10}, 1, 0},
    // {0xF0, (uint8_t []){0x00}, 1, 0},
    // {0xF0, (uint8_t []){0x02}, 1, 0},
    // // {0xE0, (uint8_t []){0x70, 0x09, 0x12, 0x0C, 0x0B, 0x27, 0x38, 0x54, 0x4E, 0x19, 0x15, 0x15, 0x2C, 0x2F}, 14, 0},
    // // {0xE1, (uint8_t []){0x70, 0x08, 0x11, 0x0C, 0x0B, 0x27, 0x38, 0x43, 0x4C, 0x18, 0x14, 0x14, 0x2B, 0x2D}, 14, 0},
    // {0xE0, (uint8_t []){0xF0, 0x0E, 0x15, 0x0B, 0x0B, 0x07, 0x3C, 0x44, 0x51, 0x38, 0x15, 0x15, 0x32, 0x36}, 14, 0},
    // {0xE1, (uint8_t []){0xF0, 0x0D, 0x15, 0x0A, 0x0A, 0x26, 0x3B, 0x43, 0x50, 0x37, 0x14, 0x15, 0x31, 0x36}, 14, 0},
    // {0xF0, (uint8_t []){0x10}, 1, 0},
    // {0xF3, (uint8_t []){0x10}, 1, 0},
    // {0xE0, (uint8_t []){0x08}, 1, 0},
    // {0xE1, (uint8_t []){0x00}, 1, 0},
    // {0xE2, (uint8_t []){0x0B}, 1, 0},
    // {0xE3, (uint8_t []){0x00}, 1, 0},
    // {0xE4, (uint8_t []){0xE0}, 1, 0},
    // {0xE5, (uint8_t []){0x06}, 1, 0},
    // {0xE6, (uint8_t []){0x21}, 1, 0},
    // {0xE7, (uint8_t []){0x00}, 1, 0},
    // {0xE8, (uint8_t []){0x05}, 1, 0},
    // {0xE9, (uint8_t []){0x82}, 1, 0},
    // {0xEA, (uint8_t []){0xDF}, 1, 0},
    // {0xEB, (uint8_t []){0x89}, 1, 0},
    // {0xEC, (uint8_t []){0x20}, 1, 0},
    // {0xED, (uint8_t []){0x14}, 1, 0},
    // {0xEE, (uint8_t []){0xFF}, 1, 0},
    // {0xEF, (uint8_t []){0x00}, 1, 0},
    // {0xF8, (uint8_t []){0xFF}, 1, 0},
    // {0xF9, (uint8_t []){0x00}, 1, 0},
    // {0xFA, (uint8_t []){0x00}, 1, 0},
    // {0xFB, (uint8_t []){0x30}, 1, 0},
    // {0xFC, (uint8_t []){0x00}, 1, 0},
    // {0xFD, (uint8_t []){0x00}, 1, 0},
    // {0xFE, (uint8_t []){0x00}, 1, 0},
    // {0xFF, (uint8_t []){0x00}, 1, 0},
    // {0x60, (uint8_t []){0x42}, 1, 0},
    // {0x61, (uint8_t []){0xE0}, 1, 0},
    // {0x62, (uint8_t []){0x40}, 1, 0},
    // {0x63, (uint8_t []){0x40}, 1, 0},
    // {0x64, (uint8_t []){0x02}, 1, 0},
    // {0x65, (uint8_t []){0x00}, 1, 0},
    // {0x66, (uint8_t []){0x40}, 1, 0},
    // {0x67, (uint8_t []){0x03}, 1, 0},
    // {0x68, (uint8_t []){0x00}, 1, 0},
    // {0x69, (uint8_t []){0x00}, 1, 0},
    // {0x6A, (uint8_t []){0x00}, 1, 0},
    // {0x6B, (uint8_t []){0x00}, 1, 0},
    // {0x70, (uint8_t []){0x42}, 1, 0},
    // {0x71, (uint8_t []){0xE0}, 1, 0},
    // {0x72, (uint8_t []){0x40}, 1, 0},
    // {0x73, (uint8_t []){0x40}, 1, 0},
    // {0x74, (uint8_t []){0x02}, 1, 0},
    // {0x75, (uint8_t []){0x00}, 1, 0},
    // {0x76, (uint8_t []){0x40}, 1, 0},
    // {0x77, (uint8_t []){0x03}, 1, 0},
    // {0x78, (uint8_t []){0x00}, 1, 0},
    // {0x79, (uint8_t []){0x00}, 1, 0},
    // {0x7A, (uint8_t []){0x00}, 1, 0},
    // {0x7B, (uint8_t []){0x00}, 1, 0},
    // // {0x80, (uint8_t []){0x38}, 1, 0},
    // {0x80, (uint8_t []){0x38}, 1, 0},
    // {0x81, (uint8_t []){0x00}, 1, 0},
    // // {0x82, (uint8_t []){0x04}, 1, 0},
    // {0x82, (uint8_t []){0x04}, 1, 0},
    // {0x83, (uint8_t []){0x02}, 1, 0},
    // // {0x84, (uint8_t []){0xDC}, 1, 0},
    // {0x84, (uint8_t []){0xDC}, 1, 0},
    // {0x85, (uint8_t []){0x00}, 1, 0},
    // {0x86, (uint8_t []){0x00}, 1, 0},
    // {0x87, (uint8_t []){0x00}, 1, 0},
    // // {0x88, (uint8_t []){0x38}, 1, 0},
    // {0x88, (uint8_t []){0x38}, 1, 0},
    // {0x89, (uint8_t []){0x00}, 1, 0},
    // // {0x8A, (uint8_t []){0x06}, 1, 0},
    // {0x8A, (uint8_t []){0x06}, 1, 0},
    // {0x8B, (uint8_t []){0x02}, 1, 0},
    // // {0x8C, (uint8_t []){0xDE}, 1, 0},
    // {0x8C, (uint8_t []){0xDE}, 1, 0},
    // {0x8D, (uint8_t []){0x00}, 1, 0},
    // {0x8E, (uint8_t []){0x00}, 1, 0},
    // {0x8F, (uint8_t []){0x00}, 1, 0},
    // // {0x90, (uint8_t []){0x38}, 1, 0},
    // {0x90, (uint8_t []){0x38}, 1, 0},
    // {0x91, (uint8_t []){0x00}, 1, 0},
    // // {0x92, (uint8_t []){0x08}, 1, 0},
    // {0x92, (uint8_t []){0x08}, 1, 0},
    // {0x93, (uint8_t []){0x02}, 1, 0},
    // // {0x94, (uint8_t []){0xE0}, 1, 0},
    // {0x94, (uint8_t []){0xE0}, 1, 0},
    // {0x95, (uint8_t []){0x00}, 1, 0},
    // {0x96, (uint8_t []){0x00}, 1, 0},
    // {0x97, (uint8_t []){0x00}, 1, 0},
    // // {0x98, (uint8_t []){0x38}, 1, 0},
    // {0x98, (uint8_t []){0x38}, 1, 0},
    // {0x99, (uint8_t []){0x00}, 1, 0},
    // // {0x9A, (uint8_t []){0x0A}, 1, 0},
    // {0x9A, (uint8_t []){0x0A}, 1, 0},
    // {0x9B, (uint8_t []){0x02}, 1, 0},
    // // {0x9C, (uint8_t []){0xE2}, 1, 0},
    // {0x9C, (uint8_t []){0xE2}, 1, 0},
    // {0x9D, (uint8_t []){0x00}, 1, 0},
    // {0x9E, (uint8_t []){0x00}, 1, 0},
    // {0x9F, (uint8_t []){0x00}, 1, 0},
    // // {0xA0, (uint8_t []){0x38}, 1, 0},
    // {0xA0, (uint8_t []){0x38}, 1, 0},
    // {0xA1, (uint8_t []){0x00}, 1, 0},
    // // {0xA2, (uint8_t []){0x03}, 1, 0},
    // {0xA2, (uint8_t []){0x03}, 1, 0},
    // {0xA3, (uint8_t []){0x02}, 1, 0},
    // // {0xA4, (uint8_t []){0xDB}, 1, 0},
    // {0xA4, (uint8_t []){0xDB}, 1, 0},
    // {0xA5, (uint8_t []){0x00}, 1, 0},
    // {0xA6, (uint8_t []){0x00}, 1, 0},
    // {0xA7, (uint8_t []){0x00}, 1, 0},
    // // {0xA8, (uint8_t []){0x38}, 1, 0},
    // {0xA8, (uint8_t []){0x38}, 1, 0},
    // {0xA9, (uint8_t []){0x00}, 1, 0},
    // // {0xAA, (uint8_t []){0x05}, 1, 0},
    // {0xAA, (uint8_t []){0x05}, 1, 0},
    // {0xAB, (uint8_t []){0x02}, 1, 0},
    // // {0xAC, (uint8_t []){0xDD}, 1, 0},
    // {0xAC, (uint8_t []){0xDD}, 1, 0},
    // {0xAD, (uint8_t []){0x00}, 1, 0},
    // {0xAE, (uint8_t []){0x00}, 1, 0},
    // {0xAF, (uint8_t []){0x00}, 1, 0},
    // // {0xB0, (uint8_t []){0x38}, 1, 0},
    // {0xB0, (uint8_t []){0x38}, 1, 0},
    // {0xB1, (uint8_t []){0x00}, 1, 0},
    // // {0xB2, (uint8_t []){0x07}, 1, 0},
    // {0xB2, (uint8_t []){0x07}, 1, 0},
    // {0xB3, (uint8_t []){0x02}, 1, 0},
    // // {0xB4, (uint8_t []){0xDF}, 1, 0},
    // {0xB4, (uint8_t []){0xDF}, 1, 0},
    // {0xB5, (uint8_t []){0x00}, 1, 0},
    // {0xB6, (uint8_t []){0x00}, 1, 0},
    // {0xB7, (uint8_t []){0x00}, 1, 0},
    // // {0xB8, (uint8_t []){0x38}, 1, 0},
    // {0xB8, (uint8_t []){0x38}, 1, 0},
    // {0xB9, (uint8_t []){0x00}, 1, 0},
    // // {0xBA, (uint8_t []){0x09}, 1, 0},
    // {0xBA, (uint8_t []){0x09}, 1, 0},
    // {0xBB, (uint8_t []){0x02}, 1, 0},
    // // {0xBC, (uint8_t []){0xE1}, 1, 0},
    // {0xBC, (uint8_t []){0xE1}, 1, 0},
    // {0xBD, (uint8_t []){0x00}, 1, 0},
    // {0xBE, (uint8_t []){0x00}, 1, 0},
    // {0xBF, (uint8_t []){0x00}, 1, 0},
    // // {0xC0, (uint8_t []){0x22}, 1, 0},
    // {0xC0, (uint8_t []){0x22}, 1, 0},
    // {0xC1, (uint8_t []){0xAA}, 1, 0},
    // {0xC2, (uint8_t []){0x65}, 1, 0},
    // {0xC3, (uint8_t []){0x74}, 1, 0},
    // {0xC4, (uint8_t []){0x47}, 1, 0},
    // {0xC5, (uint8_t []){0x56}, 1, 0},
    // {0xC6, (uint8_t []){0x00}, 1, 0},
    // {0xC7, (uint8_t []){0x88}, 1, 0},
    // {0xC8, (uint8_t []){0x99}, 1, 0},
    // {0xC9, (uint8_t []){0x33}, 1, 0},
    // // {0xD0, (uint8_t []){0x11}, 1, 0},
    // {0xD0, (uint8_t []){0x11}, 1, 0},
    // {0xD1, (uint8_t []){0xAA}, 1, 0},
    // {0xD2, (uint8_t []){0x65}, 1, 0},
    // {0xD3, (uint8_t []){0x74}, 1, 0},
    // {0xD4, (uint8_t []){0x47}, 1, 0},
    // {0xD5, (uint8_t []){0x56}, 1, 0},
    // {0xD6, (uint8_t []){0x00}, 1, 0},
    // {0xD7, (uint8_t []){0x88}, 1, 0},
    // {0xD8, (uint8_t []){0x99}, 1, 0},
    // {0xD9, (uint8_t []){0x33}, 1, 0},
    // {0xF3, (uint8_t []){0x01}, 1, 0},
    // {0xF0, (uint8_t []){0x00}, 1, 0},
    // // {0x3A, (uint8_t []){0x55}, 1, 0},
    // {0x21, (uint8_t []){0x00}, 0, 0},
    // {0x11, (uint8_t []){0x00}, 0, 120},
    // {0x29, (uint8_t []){0x00}, 0, 0},
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x49}, 1, 0},
    {0xB1, (uint8_t []){0x4A}, 1, 0},
    {0xB2, (uint8_t []){0x1F}, 1, 0},
    {0xB4, (uint8_t []){0x46}, 1, 0},
    {0xB5, (uint8_t []){0x34}, 1, 0},
    {0xB6, (uint8_t []){0xD5}, 1, 0},
    {0xB7, (uint8_t []){0x30}, 1, 0},
    {0xB8, (uint8_t []){0x04}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    // {0xDD, (uint8_t []){0x35}, 1, 0},
    // {0xDE, (uint8_t []){0x35}, 1, 0},
    // {0xDD, (uint8_t []){0x3F}, 1, 0},
    // {0xDE, (uint8_t []){0x3F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x70, 0x09, 0x12, 0x0C, 0x0B, 0x27, 0x38, 0x54, 0x4E, 0x19, 0x15, 0x15, 0x2C, 0x2F}, 14, 0},
    {0xE1, (uint8_t []){0x70, 0x08, 0x11, 0x0C, 0x0B, 0x27, 0x38, 0x43, 0x4C, 0x18, 0x14, 0x14, 0x2B, 0x2D}, 14, 0},
    // {0xE0, (uint8_t []){0xF0, 0x0E, 0x15, 0x0B, 0x0B, 0x07, 0x3C, 0x44, 0x51, 0x38, 0x15, 0x15, 0x32, 0x36}, 14, 0},
    // {0xE1, (uint8_t []){0xF0, 0x0D, 0x15, 0x0A, 0x0A, 0x26, 0x3B, 0x43, 0x50, 0x37, 0x14, 0x15, 0x31, 0x36}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x08}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x0B}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x00}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x82}, 1, 0},
    {0xEA, (uint8_t []){0xDF}, 1, 0},
    {0xEB, (uint8_t []){0x89}, 1, 0},
    {0xEC, (uint8_t []){0x20}, 1, 0},
    {0xED, (uint8_t []){0x14}, 1, 0},
    {0xEE, (uint8_t []){0xFF}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0xFF}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x30}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x42}, 1, 0},
    {0x61, (uint8_t []){0xE0}, 1, 0},
    {0x62, (uint8_t []){0x40}, 1, 0},
    {0x63, (uint8_t []){0x40}, 1, 0},
    {0x64, (uint8_t []){0x02}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x40}, 1, 0},
    {0x67, (uint8_t []){0x03}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x42}, 1, 0},
    {0x71, (uint8_t []){0xE0}, 1, 0},
    {0x72, (uint8_t []){0x40}, 1, 0},
    {0x73, (uint8_t []){0x40}, 1, 0},
    {0x74, (uint8_t []){0x02}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x40}, 1, 0},
    {0x77, (uint8_t []){0x03}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    // {0x80, (uint8_t []){0x38}, 1, 0},
    {0x80, (uint8_t []){0x38}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    // {0x82, (uint8_t []){0x04}, 1, 0},
    {0x82, (uint8_t []){0x04}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    // {0x84, (uint8_t []){0xDC}, 1, 0},
    {0x84, (uint8_t []){0xDC}, 1, 0},
    {0x85, (uint8_t []){0x00}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    // {0x88, (uint8_t []){0x38}, 1, 0},
    {0x88, (uint8_t []){0x38}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    // {0x8A, (uint8_t []){0x06}, 1, 0},
    {0x8A, (uint8_t []){0x06}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    // {0x8C, (uint8_t []){0xDE}, 1, 0},
    {0x8C, (uint8_t []){0xDE}, 1, 0},
    {0x8D, (uint8_t []){0x00}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    // {0x90, (uint8_t []){0x38}, 1, 0},
    {0x90, (uint8_t []){0x38}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    // {0x92, (uint8_t []){0x08}, 1, 0},
    {0x92, (uint8_t []){0x08}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    // {0x94, (uint8_t []){0xE0}, 1, 0},
    {0x94, (uint8_t []){0xE0}, 1, 0},
    {0x95, (uint8_t []){0x00}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    // {0x98, (uint8_t []){0x38}, 1, 0},
    {0x98, (uint8_t []){0x38}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    // {0x9A, (uint8_t []){0x0A}, 1, 0},
    {0x9A, (uint8_t []){0x0A}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    // {0x9C, (uint8_t []){0xE2}, 1, 0},
    {0x9C, (uint8_t []){0xE2}, 1, 0},
    {0x9D, (uint8_t []){0x00}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    // {0xA0, (uint8_t []){0x38}, 1, 0},
    {0xA0, (uint8_t []){0x38}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    // {0xA2, (uint8_t []){0x03}, 1, 0},
    {0xA2, (uint8_t []){0x03}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    // {0xA4, (uint8_t []){0xDB}, 1, 0},
    {0xA4, (uint8_t []){0xDB}, 1, 0},
    {0xA5, (uint8_t []){0x00}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    // {0xA8, (uint8_t []){0x38}, 1, 0},
    {0xA8, (uint8_t []){0x38}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    // {0xAA, (uint8_t []){0x05}, 1, 0},
    {0xAA, (uint8_t []){0x05}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    // {0xAC, (uint8_t []){0xDD}, 1, 0},
    {0xAC, (uint8_t []){0xDD}, 1, 0},
    {0xAD, (uint8_t []){0x00}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    // {0xB0, (uint8_t []){0x38}, 1, 0},
    {0xB0, (uint8_t []){0x38}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    // {0xB2, (uint8_t []){0x07}, 1, 0},
    {0xB2, (uint8_t []){0x07}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    // {0xB4, (uint8_t []){0xDF}, 1, 0},
    {0xB4, (uint8_t []){0xDF}, 1, 0},
    {0xB5, (uint8_t []){0x00}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    // {0xB8, (uint8_t []){0x38}, 1, 0},
    {0xB8, (uint8_t []){0x38}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    // {0xBA, (uint8_t []){0x09}, 1, 0},
    {0xBA, (uint8_t []){0x09}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    // {0xBC, (uint8_t []){0xE1}, 1, 0},
    {0xBC, (uint8_t []){0xE1}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    // {0xC0, (uint8_t []){0x22}, 1, 0},
    {0xC0, (uint8_t []){0x22}, 1, 0},
    {0xC1, (uint8_t []){0xAA}, 1, 0},
    {0xC2, (uint8_t []){0x65}, 1, 0},
    {0xC3, (uint8_t []){0x74}, 1, 0},
    {0xC4, (uint8_t []){0x47}, 1, 0},
    {0xC5, (uint8_t []){0x56}, 1, 0},
    {0xC6, (uint8_t []){0x00}, 1, 0},
    {0xC7, (uint8_t []){0x88}, 1, 0},
    {0xC8, (uint8_t []){0x99}, 1, 0},
    {0xC9, (uint8_t []){0x33}, 1, 0},
    // {0xD0, (uint8_t []){0x11}, 1, 0},
    {0xD0, (uint8_t []){0x11}, 1, 0},
    {0xD1, (uint8_t []){0xAA}, 1, 0},
    {0xD2, (uint8_t []){0x65}, 1, 0},
    {0xD3, (uint8_t []){0x74}, 1, 0},
    {0xD4, (uint8_t []){0x47}, 1, 0},
    {0xD5, (uint8_t []){0x56}, 1, 0},
    {0xD6, (uint8_t []){0x00}, 1, 0},
    {0xD7, (uint8_t []){0x88}, 1, 0},
    {0xD8, (uint8_t []){0x99}, 1, 0},
    {0xD9, (uint8_t []){0x33}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    // {0x3A, (uint8_t []){0x55}, 1, 0},
    {0x21, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 0},
};

/**
 * @brief 初始化 ST77916：先设置地址模式与像素格式，再下发厂商特定初始化序列。
 *
 * @note  为什么要先发 MADCTL/COLMOD：这两条决定了后续所有像素数据的解读方式——
 *        MADCTL 决定扫描方向/镜像/交换，COLMOD 决定每像素位宽（RGB565）。它们与
 *        "厂商初始化序列"解耦：即便调用方覆盖了 init_cmds，这里也能保证在进入
 *        厂商命令表之前，面板已知晓正确的地图与像素格式。
 *
 * @note  init_cmds 与内部覆盖逻辑：
 *         - 若调用方通过 vendor_config 提供了 init_cmds，则用之；否则用内置默认序列。
 *         - 厂商序列里如果再次出现 MADCTL/COLMOD，会覆盖这里把过的值（is_cmd_overwritten），
 *           并同步更新 st77916->madctl_val / colmod_val，保证后续镜像/交换仍基于最终值。
 *         - ST77916_CMD_SET(0xF0) 是"扩展命令组"切换命令：值为 PARAM_SET(0x00) 表示
 *           已退出扩展组（回到普通命令集），非 0 表示进入扩展组。is_user_set 跟踪这一
 *           状态，仅当处于"普通命令集"时才允许覆盖 MADCTL/COLMOD。
 *
 * @pre  面板需先 reset（硬件或软件复位），SPI/IO 就绪。
 * @sideeffect 阻塞若干毫秒（逐条命令 + 每条指令自身延时），会改写面板内部寄存器。
 */
static esp_err_t panel_st77916_init(esp_lcd_panel_t *panel)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = st77916->io;
    const st77916_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_user_set = true;
    bool is_cmd_overwritten = false;

    ESP_RETURN_ON_ERROR(tx_param(st77916, io, LCD_CMD_MADCTL, (uint8_t[]) {
        st77916->madctl_val,
    }, 1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(tx_param(st77916, io, LCD_CMD_COLMOD, (uint8_t[]) {
        st77916->colmod_val,
    }, 1), TAG, "send command failed");

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (st77916->init_cmds) {
        init_cmds = st77916->init_cmds;
        init_cmds_size = st77916->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(st77916_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        if (is_user_set && (init_cmds[i].data_bytes > 0)) {
            switch (init_cmds[i].cmd) {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                st77916->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            case LCD_CMD_COLMOD:
                is_cmd_overwritten = true;
                st77916->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            default:
                is_cmd_overwritten = false;
                break;
            }

            if (is_cmd_overwritten) {
                is_cmd_overwritten = false;
                ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence", init_cmds[i].cmd);
            }
        }

        // Send command
        ESP_RETURN_ON_ERROR(tx_param(st77916, io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));

        // Check if the current cmd is the "command set" cmd
        if ((init_cmds[i].cmd == ST77916_CMD_SET)) {
            is_user_set = ((uint8_t *)init_cmds[i].data)[0] == ST77916_PARAM_SET ? true : false;
        }
    }
    ESP_LOGD(TAG, "send init commands success");

    return ESP_OK;
}

/**
 * @brief 以 RAMWR 方式绘制一块矩形像素区。
 *
 * @note  窗口坐标约定：`x_start/x_end`、`y_start/y_end` 是左闭右开区间（x_end-1、
 *        y_end-1 才是最后一个像素），与 LVGL 的 area 传参一致——上层 LVGL flush
 *        经常把 area 的 x2+1/y2+1 传入。像素为 RGB565（fb_bits_per_pixel=16）。
 *        会先加 x_gap/y_gap 偏置（set_gap 设置的面板显示区偏移），再依次下发
 *        CASET(列地址窗)/RASET(行地址窗)/RAMWR(写显存)。
 *
 * @pre  面板已 init，SPI/IO 就绪；color_data 指向 DMA 可达的内存（内部 RAM 或已同步
 *       的 PSRAM），长度需满足 (x_end-x_start)*(y_end-y_start)*bpp/8，否则可能超限。
 * @sideeffect 向面板写入该矩形窗口的像素；错误（队列/DMA）会被回传给调用方。
 */
static esp_err_t panel_st77916_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = st77916->io;

    x_start += st77916->x_gap;
    x_end += st77916->x_gap;
    y_start += st77916->y_gap;
    y_end += st77916->y_gap;

    // define an area of frame memory where MCU can access
    ESP_RETURN_ON_ERROR(tx_param(st77916, io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(tx_param(st77916, io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * st77916->fb_bits_per_pixel / 8;
    /* Propagate queue/DMA errors to the caller.  Ignoring this result made an
     * oversized transfer look successful while only part of the LCD changed. */
    return tx_color(st77916, io, LCD_CMD_RAMWR, color_data, len);
}

/** @brief 反显开关：发送 INVON(0x21)/INVOFF(0x20)。只改面板内部反显位，不动显存。 */
static esp_err_t panel_st77916_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = st77916->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(tx_param(st77916, io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

/**
 * @brief 镜像开关：修改 MADCTL 寄存器的 BIT(6)(水平)/BIT(7)(垂直) 并立即回写。
 * @note  这是驱动层的"几何变换"，不重排显存；只改变面板扫描方向。
 *        会在本地缓存 madctl_val（struct 字段）以保证后续其他变换基于最新值。
 */
static esp_err_t panel_st77916_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = st77916->io;
    esp_err_t ret = ESP_OK;

    if (mirror_x) {
        st77916->madctl_val |= BIT(6);
    } else {
        st77916->madctl_val &= ~BIT(6);
    }
    if (mirror_y) {
        st77916->madctl_val |= BIT(7);
    } else {
        st77916->madctl_val &= ~BIT(7);
    }
    ESP_RETURN_ON_ERROR(tx_param(st77916, io, LCD_CMD_MADCTL, (uint8_t[]) {
        st77916->madctl_val
    }, 1), TAG, "send command failed");
    return ret;
}

/** @brief 交换 X/Y 轴（面板旋转 90°）：切换 MADCTL 的 MV 位并回写。 */
static esp_err_t panel_st77916_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = st77916->io;
    if (swap_axes) {
        st77916->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        st77916->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st77916->madctl_val
    }, 1);
    return ESP_OK;
}

/** @brief 设置显示区偏移（gap）：用于把逻辑坐标平移到面板显示区起点。 */
static esp_err_t panel_st77916_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    st77916->x_gap = x_gap;
    st77916->y_gap = y_gap;
    return ESP_OK;
}

/**
 * @brief 开关显示：进入/退出睡眠。
 *
 * @note  开屏路径会先发 SLPOUT(0x11)（退出睡眠，随后 120ms 让面板稳定）再发 DISPON；
 *        关屏路径发 DISPOFF + SLPIN。这里的 120ms 是面板退出睡眠的稳定时间要求。
 */
static esp_err_t panel_st77916_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    esp_lcd_panel_io_handle_t io = st77916->io;
    int command = 0;

    if (on_off) {
        ESP_RETURN_ON_ERROR(tx_param(st77916, io, LCD_CMD_SLPOUT, NULL, 0), TAG,
                            "send sleep-out failed");
        vTaskDelay(pdMS_TO_TICKS(120));
        command = LCD_CMD_DISPON;
    } else {
        ESP_RETURN_ON_ERROR(tx_param(st77916, io, LCD_CMD_DISPOFF, NULL, 0), TAG,
                            "send display-off failed");
        command = LCD_CMD_SLPIN;
    }
    ESP_RETURN_ON_ERROR(tx_param(st77916, io, command, NULL, 0), TAG, "send power command failed");
    return ESP_OK;
}
