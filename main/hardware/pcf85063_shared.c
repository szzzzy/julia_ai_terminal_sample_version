/**
 * @file    pcf85063_shared.c
 * @brief   在共享板载 I2C 上读写 RTC，并把芯片格式转换为普通日期时间。
 *
 * 当前时间服务通过这里在开机时恢复时间，并在网络校准后写回。同一颗 RTC 只能有
 * 一个活跃驱动；其它参考接口不得同时访问地址 0x51。
 *
 * 硬件连接：
 * - PCF85063 位于共享 I2C 总线（I2C_NUM_0：SCL=IO10、SDA=IO11）上，从机地址 0x51。
 * - 初始化时必须依赖 tca9554_init() 先创建总线（本函数会主动调用它以幂等自举）。
 * - 上电后向 CTRL1 写 CAP_SEL=1，选择项目当前采用的 12.5 pF 负载设置；该值来自
 *   板级配置，变更前必须重新核对晶振与模组资料。
 *
 * 芯片使用 BCD 保存时间，本模块在边界完成转换；其它模块始终使用普通十进制日期，
 * 不需要了解寄存器格式。
 */

#include "pcf85063_shared.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "tca9554.h"

/* 寄存器地址和位值来自 PCF85063 数据手册；连续时间块从秒寄存器开始。 */
#define PCF85063_ADDRESS        0x51
#define PCF85063_CTRL1_REG      0x00
#define PCF85063_SECONDS_REG    0x04
#define PCF85063_CTRL1_CAP_SEL  0x01
/* 芯片只存两位年份；1970 是本项目的软件 epoch，不是芯片固有世纪规则。 */
#define PCF85063_YEAR_OFFSET    1970

static const char *TAG = "PCF85063";
static i2c_master_dev_handle_t s_dev;

/* I2C helper 最多阻塞 100 ms，只能在 s_dev 完成初始化后由任务上下文调用。 */
static esp_err_t write_regs(uint8_t reg, const uint8_t *data, size_t length)
{
    if (s_dev == NULL || data == NULL || length > 7U) return ESP_ERR_INVALID_ARG;
    uint8_t buffer[8];
    buffer[0] = reg;
    for (size_t i = 0; i < length; ++i) buffer[i + 1U] = data[i];
    return i2c_master_transmit(s_dev, buffer, length + 1U, 100);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t length)
{
    if (s_dev == NULL || data == NULL || length == 0U) return ESP_ERR_INVALID_ARG;
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, length, 100);
}

/**
 * @brief 初始化共享 RTC（幂等）：确保 i2c 总线存在并把 PCF85063 挂上去，写 CTRL1。
 *
 * 本函数通过 tca9554_init() 幂等取得共享 bus。CTRL1 写失败时必须移除设备并清空
 * s_dev，保证“非 NULL 即初始化完整”的不变量。
 * @return ESP_OK 就绪；其他 esp_err_t 总线/设备/寄存器初始化失败。
 * I2C 为阻塞式，须在任务上下文调用。
 */
esp_err_t board_rtc_init(void)
{
    if (s_dev != NULL) return ESP_OK;
    ESP_RETURN_ON_ERROR(tca9554_init(), TAG, "shared I2C bus unavailable");
    i2c_master_bus_handle_t bus = tca9554_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "shared I2C bus is null");

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDRESS,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &s_dev),
                        TAG, "add RTC device failed");
    uint8_t control = PCF85063_CTRL1_CAP_SEL;
    esp_err_t err = write_regs(PCF85063_CTRL1_REG, &control, 1);
    if (err != ESP_OK) {
        (void)i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    ESP_LOGI(TAG, "ready at 0x%02x on shared I2C bus", PCF85063_ADDRESS);
    return ESP_OK;
}

bool board_rtc_ready(void)
{
    return s_dev != NULL;
}

/**
 * @brief 读取 RTC 时间（7 字节，从秒寄存器 0x04 起连续读）。
 *
 * 连续读取完整日期；OS 停振标志、非法 BCD 或非法公历日期均不可用于恢复墙钟。
 * @param[out] time 解析后的时间结构。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG time 为空。
 */
esp_err_t board_rtc_read_time(board_rtc_datetime_t *time)
{
    if (time == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t data[7];
    ESP_RETURN_ON_ERROR(read_regs(PCF85063_SECONDS_REG, data, sizeof(data)),
                        TAG, "read time failed");
    return board_rtc_decode(data, PCF85063_YEAR_OFFSET, time)
               ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/**
 * @brief 写 RTC 时间（7 字节，从秒寄存器 0x04 起连续写，BCD 编码）。
 *
 * 校验完整公历日期，保留旧固件的 1970～2069 年份编码。写入时
 * 不停止振荡器，跨秒边界可能产生轻微偏差，不能把成功返回理解为精确校时确认。
 */
esp_err_t board_rtc_set_time(const board_rtc_datetime_t *time)
{
    uint8_t data[7];
    if (!board_rtc_encode(time, data)) return ESP_ERR_INVALID_ARG;
    return write_regs(PCF85063_SECONDS_REG, data, sizeof(data));
}
