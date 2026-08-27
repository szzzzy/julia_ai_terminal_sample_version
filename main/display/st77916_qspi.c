#include "st77916_qspi.h"

#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_log.h"

#define ST77916_QSPI_BITS_PER_PIXEL 16
#define ST77916_EXIO_OUTPUT_REG 0x01
#define ST77916_EXIO_CONFIG_REG 0x03
#define LCD_OPCODE_WRITE_CMD 0x02ULL
#define LCD_OPCODE_READ_CMD 0x0BULL
#define LCD_OPCODE_WRITE_COLOR 0x32ULL
#define ST77916_CHUNK_LINES 20
#define TAG "ST77916"

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_bytes;
    uint16_t delay_ms;
} st77916_lcd_init_cmd_t;

static const st77916_lcd_init_cmd_t st77916_init_cmds[] = {
    {0xF0, {0x28}, 1, 0},
    {0xF2, {0x28}, 1, 0},
    {0x7C, {0xD1}, 1, 0},
    {0x83, {0xE0}, 1, 0},
    {0x84, {0x61}, 1, 0},
    {0xF2, {0x82}, 1, 0},
    {0xF0, {0x00}, 1, 0},
    {0xF0, {0x01}, 1, 0},
    {0xF1, {0x01}, 1, 0},
    {0xB0, {0x49}, 1, 0},
    {0xB1, {0x4A}, 1, 0},
    {0xB2, {0x1F}, 1, 0},
    {0xB4, {0x46}, 1, 0},
    {0xB5, {0x34}, 1, 0},
    {0xB6, {0xD5}, 1, 0},
    {0xB7, {0x30}, 1, 0},
    {0xB8, {0x04}, 1, 0},
    {0xBA, {0x00}, 1, 0},
    {0xBB, {0x08}, 1, 0},
    {0xBC, {0x08}, 1, 0},
    {0xBD, {0x00}, 1, 0},
    {0xC0, {0x80}, 1, 0},
    {0xC1, {0x10}, 1, 0},
    {0xC2, {0x37}, 1, 0},
    {0xC3, {0x80}, 1, 0},
    {0xC4, {0x10}, 1, 0},
    {0xC5, {0x37}, 1, 0},
    {0xC6, {0xA9}, 1, 0},
    {0xC7, {0x41}, 1, 0},
    {0xC8, {0x01}, 1, 0},
    {0xC9, {0xA9}, 1, 0},
    {0xCA, {0x41}, 1, 0},
    {0xCB, {0x01}, 1, 0},
    {0xD0, {0x91}, 1, 0},
    {0xD1, {0x68}, 1, 0},
    {0xD2, {0x68}, 1, 0},
    {0xF5, {0x00, 0xA5}, 2, 0},
    {0xF1, {0x10}, 1, 0},
    {0xF0, {0x00}, 1, 0},
    {0xF0, {0x02}, 1, 0},
    {0xE0, {0x70, 0x09, 0x12, 0x0C, 0x0B, 0x27, 0x38, 0x54, 0x4E, 0x19, 0x15, 0x15, 0x2C, 0x2F}, 14, 0},
    {0xE1, {0x70, 0x08, 0x11, 0x0C, 0x0B, 0x27, 0x38, 0x43, 0x4C, 0x18, 0x14, 0x14, 0x2B, 0x2D}, 14, 0},
    {0xF0, {0x10}, 1, 0},
    {0xF3, {0x10}, 1, 0},
    {0xE0, {0x08}, 1, 0},
    {0xE1, {0x00}, 1, 0},
    {0xE2, {0x0B}, 1, 0},
    {0xE3, {0x00}, 1, 0},
    {0xE4, {0xE0}, 1, 0},
    {0xE5, {0x06}, 1, 0},
    {0xE6, {0x21}, 1, 0},
    {0xE7, {0x00}, 1, 0},
    {0xE8, {0x05}, 1, 0},
    {0xE9, {0x82}, 1, 0},
    {0xEA, {0xDF}, 1, 0},
    {0xEB, {0x89}, 1, 0},
    {0xEC, {0x20}, 1, 0},
    {0xED, {0x14}, 1, 0},
    {0xEE, {0xFF}, 1, 0},
    {0xEF, {0x00}, 1, 0},
    {0xF8, {0xFF}, 1, 0},
    {0xF9, {0x00}, 1, 0},
    {0xFA, {0x00}, 1, 0},
    {0xFB, {0x30}, 1, 0},
    {0xFC, {0x00}, 1, 0},
    {0xFD, {0x00}, 1, 0},
    {0xFE, {0x00}, 1, 0},
    {0xFF, {0x00}, 1, 0},
    {0x21, {0}, 0, 0},
    {0x11, {0}, 0, 120},
    {0x29, {0}, 0, 0},
};

static int st77916_pack_cmd(uint8_t opcode, int lcd_cmd)
{
    return ((int)opcode << 24) | ((lcd_cmd & 0xFF) << 8);
}

static uint16_t st77916_swap16(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static esp_err_t st77916_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    return esp_lcd_panel_io_tx_param(io, st77916_pack_cmd(LCD_OPCODE_WRITE_CMD, lcd_cmd), param, param_size);
}

static esp_err_t st77916_tx_color(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    return esp_lcd_panel_io_tx_color(io, st77916_pack_cmd(LCD_OPCODE_WRITE_COLOR, lcd_cmd), param, param_size);
}

static esp_err_t st77916_qspi_set_window(esp_lcd_panel_io_handle_t io, int x0, int y0, int x1, int y1)
{
    uint8_t params[4];

    params[0] = (x0 >> 8) & 0xFF;
    params[1] = x0 & 0xFF;
    params[2] = (x1 >> 8) & 0xFF;
    params[3] = x1 & 0xFF;
    ESP_RETURN_ON_ERROR(st77916_tx_param(io, 0x2A, params, sizeof(params)), "st77916", "set column address failed");

    params[0] = (y0 >> 8) & 0xFF;
    params[1] = y0 & 0xFF;
    params[2] = (y1 >> 8) & 0xFF;
    params[3] = y1 & 0xFF;
    ESP_RETURN_ON_ERROR(st77916_tx_param(io, 0x2B, params, sizeof(params)), "st77916", "set row address failed");

    return st77916_tx_param(io, 0x2C, NULL, 0);
}

static esp_err_t st77916_qspi_draw_pixel(esp_lcd_panel_io_handle_t io, int x, int y, uint16_t color)
{
    ESP_RETURN_ON_ERROR(st77916_qspi_set_window(io, x, y, x, y), "st77916", "set pixel window failed");
    return st77916_tx_color(io, 0x2C, &color, sizeof(color));
}

esp_err_t st77916_qspi_new(const st77916_qspi_config_t *config, esp_lcd_panel_io_handle_t *ret_io)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, "st77916", "config is null");
    ESP_RETURN_ON_FALSE(ret_io != NULL, ESP_ERR_INVALID_ARG, "st77916", "ret_io is null");

    spi_bus_config_t bus_conf = {
        .data0_io_num = config->data0_gpio_num,
        .data1_io_num = config->data1_gpio_num,
        .data2_io_num = config->data2_gpio_num,
        .data3_io_num = config->data3_gpio_num,
        .sclk_io_num = config->sck_gpio_num,
        .max_transfer_sz = 2048,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(config->spi_host, &bus_conf, SPI_DMA_CH_AUTO), "st77916", "spi bus init failed");

    esp_lcd_panel_io_spi_config_t io_conf = {
        .cs_gpio_num = config->cs_gpio_num,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = config->pclk_hz,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .quad_mode = 1,
        },
    };

    return esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)config->spi_host, &io_conf, ret_io);
}

static esp_err_t st77916_exio_write_reg(i2c_port_t port, uint8_t addr, uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_write_to_device(port, addr, payload, sizeof(payload), pdMS_TO_TICKS(100));
}

static esp_err_t st77916_exio_read_reg(i2c_port_t port, uint8_t addr, uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(port, addr, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

static void st77916_log_panel_id(esp_lcd_panel_io_handle_t io)
{
    uint8_t id[4] = {0};
    esp_err_t err = esp_lcd_panel_io_rx_param(io, st77916_pack_cmd(LCD_OPCODE_READ_CMD, 0x04), id, sizeof(id));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LCD ID(0x04): %02X %02X %02X %02X", id[0], id[1], id[2], id[3]);
    } else {
        ESP_LOGW(TAG, "LCD ID read failed: %s", esp_err_to_name(err));
    }

    uint8_t status[4] = {0};
    err = esp_lcd_panel_io_rx_param(io, st77916_pack_cmd(LCD_OPCODE_READ_CMD, 0x09), status, sizeof(status));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LCD STATUS(0x09): %02X %02X %02X %02X", status[0], status[1], status[2], status[3]);
    } else {
        ESP_LOGW(TAG, "LCD status read failed: %s", esp_err_to_name(err));
    }
}

esp_err_t st77916_qspi_reset(const st77916_qspi_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, "st77916", "config is null");
    ESP_RETURN_ON_FALSE(config->lcd_rst_exio_bit < 8, ESP_ERR_INVALID_ARG, "st77916", "invalid exio bit");

    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = config->i2c_sda_gpio_num,
        .scl_io_num = config->i2c_scl_gpio_num,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(config->i2c_port, &i2c_conf), "st77916", "i2c param config failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(config->i2c_port, i2c_conf.mode, 0, 0, 0), "st77916", "i2c driver install failed");

    uint8_t exio_addr = config->exio_i2c_addr;
    if (exio_addr == 0) {
        const uint8_t candidates[] = {0x20, 0x21, 0x22, 0x23};
        for (size_t i = 0; i < sizeof(candidates); i++) {
            uint8_t value = 0;
            if (st77916_exio_read_reg(config->i2c_port, candidates[i], ST77916_EXIO_CONFIG_REG, &value) == ESP_OK) {
                exio_addr = candidates[i];
                break;
            }
        }
    }
    ESP_RETURN_ON_FALSE(exio_addr != 0, ESP_ERR_NOT_FOUND, "st77916", "tca9554 not found on i2c bus");
    ESP_LOGI(TAG, "TCA9554 detected at 0x%02X", exio_addr);

    uint8_t config_reg = 0xFF;
    ESP_RETURN_ON_ERROR(st77916_exio_read_reg(config->i2c_port, exio_addr, ST77916_EXIO_CONFIG_REG, &config_reg), "st77916", "read exio config failed");
    config_reg &= (uint8_t)~(1U << config->lcd_rst_exio_bit);
    ESP_RETURN_ON_ERROR(st77916_exio_write_reg(config->i2c_port, exio_addr, ST77916_EXIO_CONFIG_REG, config_reg), "st77916", "write exio config failed");
    ESP_LOGI(TAG, "EXIO config=0x%02X, reset bit=%u", config_reg, config->lcd_rst_exio_bit);

    uint8_t output_reg = 0xFF;
    ESP_RETURN_ON_ERROR(st77916_exio_read_reg(config->i2c_port, exio_addr, ST77916_EXIO_OUTPUT_REG, &output_reg), "st77916", "read exio output failed");
    output_reg &= (uint8_t)~(1U << config->lcd_rst_exio_bit);
    ESP_RETURN_ON_ERROR(st77916_exio_write_reg(config->i2c_port, exio_addr, ST77916_EXIO_OUTPUT_REG, output_reg), "st77916", "assert lcd reset failed");
    ESP_LOGI(TAG, "LCD reset asserted, EXIO output=0x%02X", output_reg);
    vTaskDelay(pdMS_TO_TICKS(20));

    output_reg |= (uint8_t)(1U << config->lcd_rst_exio_bit);
    ESP_RETURN_ON_ERROR(st77916_exio_write_reg(config->i2c_port, exio_addr, ST77916_EXIO_OUTPUT_REG, output_reg), "st77916", "release lcd reset failed");
    ESP_LOGI(TAG, "LCD reset released, EXIO output=0x%02X", output_reg);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

esp_err_t st77916_qspi_init(esp_lcd_panel_io_handle_t io)
{
    ESP_RETURN_ON_FALSE(io != NULL, ESP_ERR_INVALID_ARG, "st77916", "io is null");

    ESP_RETURN_ON_ERROR(st77916_tx_param(io, 0x36, (uint8_t[]){0x00}, 1), "st77916", "madctl init failed");
    ESP_RETURN_ON_ERROR(st77916_tx_param(io, 0x3A, (uint8_t[]){0x55}, 1), "st77916", "colmod init failed");

    for (size_t i = 0; i < sizeof(st77916_init_cmds) / sizeof(st77916_init_cmds[0]); i++) {
        const st77916_lcd_init_cmd_t *cmd = &st77916_init_cmds[i];
        const void *data = cmd->data_bytes ? cmd->data : NULL;
        ESP_RETURN_ON_ERROR(st77916_tx_param(io, cmd->cmd, data, cmd->data_bytes), "st77916", "init command failed");
        if (cmd->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }

    st77916_log_panel_id(io);

    return ESP_OK;
}

esp_err_t st77916_qspi_fill_color(esp_lcd_panel_io_handle_t io, uint16_t width, uint16_t height, uint16_t color)
{
    uint16_t *chunk = NULL;
    uint16_t color_be;

    ESP_RETURN_ON_FALSE(io != NULL, ESP_ERR_INVALID_ARG, "st77916", "io is null");
    if (width > ST77916_QSPI_LCD_WIDTH) {
        width = ST77916_QSPI_LCD_WIDTH;
    }
    if (height > ST77916_QSPI_LCD_HEIGHT) {
        height = ST77916_QSPI_LCD_HEIGHT;
    }
    ESP_RETURN_ON_FALSE(width > 0 && height > 0, ESP_ERR_INVALID_ARG, "st77916", "invalid frame size");

    chunk = heap_caps_malloc((size_t)width * ST77916_CHUNK_LINES * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(chunk != NULL, ESP_ERR_NO_MEM, "st77916", "no mem for chunk buffer");
    color_be = st77916_swap16(color);

    for (size_t i = 0; i < (size_t)width * ST77916_CHUNK_LINES; i++) {
        chunk[i] = color_be;
    }

    esp_err_t err = ESP_OK;
    for (uint16_t y = 0; y < height && err == ESP_OK; y += ST77916_CHUNK_LINES) {
        uint16_t lines = (height - y > ST77916_CHUNK_LINES) ? ST77916_CHUNK_LINES : (height - y);
        err = st77916_qspi_set_window(io, 0, y, width - 1, y + lines - 1);
        if (err == ESP_OK) {
            err = st77916_tx_color(io, 0x2C, chunk, (size_t)width * lines * sizeof(uint16_t));
        }
    }
    free(chunk);

    return err;
}
