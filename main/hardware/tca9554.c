/**
 * @file    tca9554.c
 * @brief   初始化板载共享 I2C，并安全改变扩展器控制的 LCD 与 SD 卡信号。
 *
 * 写输出时先准备目标电平，再把引脚切成输出，避免方向切换瞬间出现错误脉冲。
 * 寄存器读改写必须串行，防止 LCD 复位和 SD 卡控制同时修改时覆盖彼此位。
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#include "tca9554.h"

static const char *TAG = "tca9554";

/* 0x20 是 7-bit slave address，R/W 位由 I2C driver 生成；以下为 datasheet register map。 */
#define TCA9554_REG_INPUT     0x00
#define TCA9554_REG_OUTPUT    0x01
#define TCA9554_REG_POLARITY  0x02
#define TCA9554_REG_CONFIG    0x03

/* 本板使用的 I2C 主控制器引脚（I2C_NUM_0）。 */
#define TCA9554_I2C_SCL GPIO_NUM_10
#define TCA9554_I2C_SDA GPIO_NUM_11

/* 扩展器首先创建板载 I2C；RTC 和运动传感器复用同一总线，避免重复初始化控制器。 */
static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
/* 保证一次“读取旧值、修改一位、写回”完整执行，避免另一任务的引脚变化被覆盖。 */
static SemaphoreHandle_t s_lock = NULL;

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

/**
 * @brief 建立板载共享 I2C 并接入扩展器；重复调用直接复用现有连接。
 *
 * RTC/IMU 随后借用这条 bus。启动阶段串行调用；只有锁、总线与设备全部创建成功
 * 才发布句柄，失败时回收本次申请，允许随后重试。
 *
 * @return ESP_OK 已就绪；其他 esp_err_t 总线或设备初始化失败。
 */
esp_err_t tca9554_init(void)
{
    if (s_dev && s_lock) {
        return ESP_OK;
    }
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock == NULL) return ESP_ERR_NO_MEM;
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TCA9554_I2C_SDA,
        .scl_io_num = TCA9554_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        vSemaphoreDelete(lock);
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        (void)i2c_del_master_bus(bus);
        vSemaphoreDelete(lock);
        return err;
    }
    s_lock = lock;
    s_bus = bus;
    s_dev = dev;
    ESP_LOGI(TAG, "ready at 0x%02x (scl=%d, sda=%d)", TCA9554_ADDR, TCA9554_I2C_SCL,
             TCA9554_I2C_SDA);
    return ESP_OK;
}

/**
 * @brief 把引脚配置为推挽输出并设置电平（读-改-写，带互斥锁）。
 *
 * 关键顺序（可防止使能瞬间引脚跳变）：
 *   1) 读 CONFIG 与 OUTPUT 寄存器；
 *   2) 先写 OUTPUT 输出锁存（设/清目标位），
 *   3) 再写 CONFIG 把该引脚从“输入”切到“输出”。
 * 这样在方向切换前输出值已就位，避免引脚一变为输出就因为锁存值未定而闪一下。
 *
 * @param[in] pin   引脚号 0~7（>7 或未初始化返回 ESP_ERR_INVALID_ARG）。
 * @param[in] level true 高电平；false 低电平。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法；其他 esp_err_t 总线读写失败。
 * 注意：I2C 是阻塞式（300ms 级超时），且内部会持有 s_lock，不能在中断里调用；
 * 调用前需已 tca9554_init()。
 */
esp_err_t tca9554_write_pin(uint8_t pin, bool level)
{
    if (pin > 7 || !s_dev || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t cfg = 0, out = 0;
    esp_err_t err = read_reg(TCA9554_REG_CONFIG, &cfg);
    if (err == ESP_OK) {
        err = read_reg(TCA9554_REG_OUTPUT, &out);
    }
    if (err == ESP_OK) {
        /* 先写输出锁存、再切方向：使能瞬间引脚不会输出意外电平。 */
        if (level) {
            out |= (uint8_t)(1u << pin);
        } else {
            out &= (uint8_t)~(1u << pin);
        }
        err = write_reg(TCA9554_REG_OUTPUT, out);
        if (err == ESP_OK) {
            cfg &= (uint8_t)~(1u << pin);
            err = write_reg(TCA9554_REG_CONFIG, cfg);
        }
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write pin %u failed: %s", pin, esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 读取引脚电平（读 INPUT 寄存器，输入/输出引脚均可读）。
 *
 * @param[in]  pin   引脚号 0~7。
 * @param[out] level 读出电平（true 高 / false 低）。仅当传输成功时写入。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法；其他 esp_err_t 总线读失败。
 * I2C 为阻塞式且内部持锁，非中断上下文调用。
 */
esp_err_t tca9554_read_pin(uint8_t pin, bool *level)
{
    if (pin > 7 || !s_dev || !s_lock || !level) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t in = 0;
    esp_err_t err = read_reg(TCA9554_REG_INPUT, &in);
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        *level = ((in >> pin) & 1u) != 0;
    }
    return err;
}

/** 返回由 tca9554 创建、并被 RTC 等板载外设复用的共享 I2C 总线句柄。 */
i2c_master_bus_handle_t tca9554_i2c_bus(void)
{
    return s_bus;
}
