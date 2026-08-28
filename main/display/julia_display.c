/**
 * @file    julia_display.c
 * @brief   板级显示封装：装配 ST77916(QSPI) 面板并把 LVGL 显示端口(LVGL_PORT)接上。
 *
 * @section jd_scope 职责与边界
 *         本文件是"板级接线 + 初始化编排"层：
 *           - 选定板载引脚（SPI2、SCK/数据/CS、时钟 40MHz、360x360）；
 *           - 通过现有 TCA9554 做面板复位（reset_panel_via_existing_tca9554）；
 *           - 初始化 SPI 总线 → panel IO → 创建 ST77916 面板 → reset/init/disp_on；
 *           - 最后调用 lvgl_port_init 把面板接给 LVGL（此时才建 LVGL 任务/锁/tick）。
 *         它不直接绘制像素：所有渲染走 LVGL，经 lvgl_port 的 flush_cb 落到面板。
 *
 * @section jd_order 初始化顺序（panel → lvgl_port → ui）
 *         本函数只完成前两段；`main.c` 在 julia_display_init 成功后，再接 julia_avatar_init
 *         （创建立绘并做首次全屏刷新）。因此顺序严格为 panel → lvgl_port → ui，
 *         这与移植文档 §6.3 一致：LVGL 初始化的前提是 panel 已创建。
 *
 * @section jd_backlight 背光
 *         背光（LEDC PWM，julia_backlight 模块）与本文件独立，由 main.c 在立绘首帧
 *         渲染成功后才点亮；本函数日志中的 "backlight held off" 即反映这一点。
 *
 * @see    main/lvgl_port/lvgl_port.c（LVGL 显示端口与刷新回调）
 * @see    main/display/esp_lcd_st77916.c（通用 ST77916 面板驱动）
 * @see    main/app/main.c（顶层初始化顺序：显示 → 立绘）
 */
#include "julia_display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_st77916.h"
#include "lvgl_port.h"
#include "tca9554.h"

#define LCD_HOST                  SPI2_HOST
#define LCD_SCK_GPIO              GPIO_NUM_40
#define LCD_DATA0_GPIO            GPIO_NUM_46
#define LCD_DATA1_GPIO            GPIO_NUM_45
#define LCD_DATA2_GPIO            GPIO_NUM_42
#define LCD_DATA3_GPIO            GPIO_NUM_41
#define LCD_CS_GPIO               GPIO_NUM_21
#define LCD_PIXEL_CLOCK_HZ        (40U * 1000U * 1000U)
#define LCD_WIDTH                 360
#define LCD_HEIGHT                360

static const char *TAG = "julia_display";
static esp_lcd_panel_handle_t s_panel;
static bool s_ready;

/* 针对本模组调校过的厂商初始化序列（与 esp_lcd_st77916 的默认序列、以及
 * st77916_qspi.c 的序列同源，但部分寄存器（伽马表 0xE0/0xE1、亮度/相位等）取值不同，
 * 以本项目烧录验证为准）。寄存器语义由厂商决定，这里只按序透传。
 *
 * NOTE：需结合模组/供应商确认：个别寄存器位（伽马、电源时序）的精确含义无法在本层
 *       推断，改动请在真机上对比验证后再锁定。 */
static const st77916_lcd_init_cmd_t vendor_specific_init_new[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0},
    {0xEE, (uint8_t[]){0x00}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x06}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDA}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0C}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD7}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x09}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0},
};


/**
 * @brief 复用已存在的 TCA9554 驱动做面板硬件复位（active-low）。
 *
 * @note  为什么走 TCA9554 而不是直接 GPIO：本板把 LCD 复位信号接到板载 EXIO
 *        （原理图标注 EXIO2，TCA9554 引脚从 0 起，因此是 P1），且 TCA9554 已被其他
 *        设备使用（I2C_NUM_0），复用其驱动可避免安装第二个共享冲突的所有者。此函数
 *        与 st77916_qspi_reset 的语义等价（同为 EXIO 复位），是当前实际编译的复位路径。
 * @sideeffect 初始化 TCA9554；RST 拉低 20ms → 拉高 120ms；阻塞约 140ms。
 */
static esp_err_t reset_panel_via_existing_tca9554(void)
{
    /* The board labels this signal EXIO2.  TCA9554 pins are zero-based here,
     * therefore EXIO2 is P1.  Reusing this driver avoids installing a second
     * owner for I2C_NUM_0. */
    ESP_RETURN_ON_ERROR(tca9554_init(), TAG, "TCA9554 init failed");
    ESP_RETURN_ON_ERROR(tca9554_write_pin(TCA9554_PIN_LCD_RST, false),
                        TAG, "assert LCD reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(tca9554_write_pin(TCA9554_PIN_LCD_RST, true),
                        TAG, "release LCD reset failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

/**
 * @brief 初始化板级 ST77916 显示并注册 LVGL 显示端口（幂等）。
 *
 * @note  初始化顺序（严格按此）：
 *         1. 通过 TCA9554 硬件复位面板；
 *         2. 初始化 SPI2 总线（QSPI 四线，DMA 自动选）；
 *         3. 创建 panel IO（CLK 40MHz、命令位宽 32、quad_mode=1、传输完成回调挂到
 *            lvgl_port_color_trans_done）——这个回调是 LVGL flush 与 DMA 异步完成同步的关键；
 *         4. 创建 ST77916 面板（vendor_config 注入本模组初始化序列 + 开启 QSPI 接口）；
 *         5. reset → init → disp_on_off(true) 让面板真正工作；
 *         6. lvgl_port_init(s_panel) 把面板接给 LVGL（建 LVGL 任务/锁/tick）。
 *        之后 main.c 再调用 julia_avatar_init 走 UI。任一步失败即返回，s_ready 保持 false。
 *
 * @note  关键配置说明：
 *         - `.reset_gpio_num = -1`：复位已由 TCA9554 完成，面板驱动不再管理 RST GPIO。
 *         - `.trans_queue_depth = 1`：只允许一个在途 SPI 传输，与
 *           lvgl_port_draw_bitmap_sync 的阻塞式等待一拍完成回调匹配，避免队列过深。
 *         - SPI 总线 `max_trans_sz` 取一行 LVGL 缓冲（BUFFER_PIXELS 像素），同 DMA 块一致。
 *
 * @pre  背光模块、TCA9554 驱动可用（tca9554_init 在函数内调用）。
 * @return ESP_OK 成功；否则某一步的 esp_err_t（部分资源可能已分配，但 s_ready 不变）。
 * @sideeffect 占用 SPI2 主机与相关 GPIO；启动 LVGL 任务与 tick 定时器（最终一步成功时）。
 */
esp_err_t julia_display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }


    ESP_RETURN_ON_ERROR(reset_panel_via_existing_tca9554(), TAG, "panel reset failed");

    const spi_bus_config_t bus_config = {
        .data0_io_num = LCD_DATA0_GPIO,
        .data1_io_num = LCD_DATA1_GPIO,
        .sclk_io_num = LCD_SCK_GPIO,
        .data2_io_num = LCD_DATA2_GPIO,
        .data3_io_num = LCD_DATA3_GPIO,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = LVGL_PORT_BUFFER_PIXELS * sizeof(lv_color_t),
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .intr_flags = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "LCD SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS_GPIO,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .on_color_trans_done = lvgl_port_color_trans_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = 1,
        },
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io),
        TAG, "LCD panel IO init failed");

    const st77916_vendor_config_t vendor_config = {
        .init_cmds = vendor_specific_init_new,
        .init_cmds_size = sizeof(vendor_specific_init_new) /
                          sizeof(vendor_specific_init_new[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(io, &panel_config, &s_panel),
                        TAG, "create ST77916 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset op failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                        TAG, "panel display-on failed");
    ESP_RETURN_ON_ERROR(lvgl_port_init(s_panel), TAG, "LVGL port init failed");

    s_ready = true;
    ESP_LOGI(TAG, "ST77916 + LVGL ready (%dx%d QSPI, backlight held off)",
             LCD_WIDTH, LCD_HEIGHT);
    return ESP_OK;
}

/** @brief 查询显示是否已完成初始化（供上层 main.c 决定是否继续接立绘/点亮背光）。 */
bool julia_display_is_ready(void)
{
    return s_ready;
}
