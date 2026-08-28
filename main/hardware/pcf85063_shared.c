/**
 * @file    pcf85063_shared.c
 * @brief   PCF85063 RTC 的“共享/封装”层：使用 ESP-IDF i2c_master 驱动访问 RTC。
 *
 * 与 main/PCF85063/PCF85063.c 的区别：
 *   - 本文件是面向 ESP-IDF 原生驱动的轻量封装，复用 tca9554 创建的 i2c_master 总线，
 *     仅供 julia_time.c（SNTP 校时/启动时从 RTC 恢复系统时间）使用；
 *   - main/PCF85063/PCF85063.c 是较完整的 Waveshare 驱动，经外部 I2C_Driver 组件
 *     访问，供 julia_context.c 使用。两者都写同一颗 RTC（地址 0x51），但走不同总线栈。
 *
 * 硬件连接：
 * - PCF85063 位于共享 I2C 总线（I2C_NUM_0：SCL=IO10、SDA=IO11）上，从机地址 0x51。
 * - 初始化时必须依赖 tca9554_init() 先创建总线（本函数会主动调用它以幂等自举）。
 * - 上电后向 CTRL1 写 CAP_SEL=1，选择内部 12.5pF 负载电容（匹配晶振规格）。
 *
 * 上游：julia_time.c 的 julia_time_init() → board_rtc_init()；ip_ready 后由
 * SNTP sync 回调 board_rtc_set_time() 写回。启动早期 board_rtc_read_time() 用于
 * 恢复系统时间。数据流：RTC 的 BCD 时间寄存器 → bcd_to_dec → board_rtc_datetime_t。
 */

#include "pcf85063_shared.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "tca9554.h"

/* —— PCF85063 寄存器/位定义（仅本封装用到的几项） —— */
#define PCF85063_ADDRESS        0x51  /* 7 位从机地址。 */
#define PCF85063_CTRL1_REG      0x00  /* 控制/状态寄存器 1。 */
#define PCF85063_SECONDS_REG    0x04  /* 秒寄存器（也是“时间块”起始：秒.分.时.日.星期.月.年 共 7 字节，地址自动递增）。 */
#define PCF85063_CTRL1_CAP_SEL  0x01  /* 内部负载电容选择：1=12.5pF，0=7pF。 */
#define PCF85063_YEAR_OFFSET    1970  /* 年寄存器只存 0~99，真实年份 = 1970 + 寄存器值。 */

static const char *TAG = "PCF85063";
static i2c_master_dev_handle_t s_dev;

/** 十进制 → BCD：把 0~99 拆成两个半字节（高 4 位为十位）。 */
static uint8_t dec_to_bcd(unsigned value)
{
    return (uint8_t)(((value / 10U) << 4U) | (value % 10U));
}

/** BCD → 十进制：从两个半字节还原 0~99。 */
static uint8_t bcd_to_dec(uint8_t value)
{
    return (uint8_t)(((value >> 4U) * 10U) + (value & 0x0fU));
}

/**
 * @brief 从指定寄存器起连续写 length 字节（阻塞式，超时 100ms）。
 * @param[in] reg    起始寄存器地址。
 * @param[in] data   待写数据。
 * @param[in] length 字节数（<=7）。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法（未初始化/空指针/长度超限）。
 */
static esp_err_t write_regs(uint8_t reg, const uint8_t *data, size_t length)
{
    if (s_dev == NULL || data == NULL || length > 7U) return ESP_ERR_INVALID_ARG;
    uint8_t buffer[8];
    buffer[0] = reg;
    for (size_t i = 0; i < length; ++i) buffer[i + 1U] = data[i];
    return i2c_master_transmit(s_dev, buffer, length + 1U, 100);
}

/**
 * @brief 从指定寄存器起连续读 length 字节（阻塞式，超时 100ms）。
 * @param[in]  reg    起始寄存器地址。
 * @param[out] data   读出数据缓冲区。
 * @param[in]  length 字节数（>0）。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法。
 */
static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t length)
{
    if (s_dev == NULL || data == NULL || length == 0U) return ESP_ERR_INVALID_ARG;
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, length, 100);
}

/**
 * @brief 初始化共享 RTC（幂等）：确保 i2c 总线存在并把 PCF85063 挂上去，写 CTRL1。
 *
 * 前置条件：需先 tca9554_init() 创建共享总线（本函数通过调用它来幂等自举）。
 * 流程：添加设备（100 kHz）→ 写 CTRL1=CAP_SEL（选内部 12.5pF）。写失败会移除设备
 * 并置 s_dev=NULL，返回错误（避免后续用到一个半初始化设备）。
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

/** 查询 RTC 是否已初始化（s_dev != NULL）。 */
bool board_rtc_ready(void)
{
    return s_dev != NULL;
}

/**
 * @brief 读取 RTC 时间（7 字节，从秒寄存器 0x04 起连续读）。
 *
 * PCF85063 时间寄存器是 BCD 编码且带保留/标志位，解码时要清掉：
 *   秒  &0x7f（bit7=OS 振荡停止标志）；
 *   分  &0x7f（bit7 保留）；
 *   时  &0x3f（bit6=12/24 制标志，bit5=AM/PM；当前 24 制只用低 6 位）；
 *   日  &0x3f；
 *   星期 &0x07（0=周日…6=周六）；
 *   月  &0x1f（bit5=世纪标志）；
 *   年   = BCD + 1970。
 * 注意：这里没有“先停振/再启振”的多步读法，直接一次连续读，调用方应自行判断
 * 读出的时间是否合理（见 julia_time.c 的 datetime_valid）。
 * @param[out] time 解析后的时间结构。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG time 为空。
 */
esp_err_t board_rtc_read_time(board_rtc_datetime_t *time)
{
    if (time == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t data[7];
    ESP_RETURN_ON_ERROR(read_regs(PCF85063_SECONDS_REG, data, sizeof(data)),
                        TAG, "read time failed");
    time->second = bcd_to_dec(data[0] & 0x7fU);
    time->minute = bcd_to_dec(data[1] & 0x7fU);
    time->hour = bcd_to_dec(data[2] & 0x3fU);
    time->day = bcd_to_dec(data[3] & 0x3fU);
    time->dotw = bcd_to_dec(data[4] & 0x07U);
    time->month = bcd_to_dec(data[5] & 0x1fU);
    time->year = (uint16_t)(bcd_to_dec(data[6]) + PCF85063_YEAR_OFFSET);
    return ESP_OK;
}

/**
 * @brief 写 RTC 时间（7 字节，从秒寄存器 0x04 起连续写，BCD 编码）。
 *
 * @param[in] time 目标时间；year 必须在 [1970, 2069]（否则返回 ESP_ERR_INVALID_ARG）。
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法。
 * 注意：写入过程中 RTC 振荡器不停（未置 STOP 位），秒计数继续走；若写入恰好跨过
 * 秒进位，可能出现秒/分略有偏差。若要更稳妥的整时间写入应在调用方先停振并加校验
 * （本工程为简化起见未做）。
 */
esp_err_t board_rtc_set_time(const board_rtc_datetime_t *time)
{
    if (time == NULL || time->year < PCF85063_YEAR_OFFSET ||
        time->year > PCF85063_YEAR_OFFSET + 99U) return ESP_ERR_INVALID_ARG;
    uint8_t data[7] = {
        dec_to_bcd(time->second), dec_to_bcd(time->minute), dec_to_bcd(time->hour),
        dec_to_bcd(time->day), dec_to_bcd(time->dotw), dec_to_bcd(time->month),
        dec_to_bcd((unsigned)time->year - PCF85063_YEAR_OFFSET),
    };
    return write_regs(PCF85063_SECONDS_REG, data, sizeof(data));
}

