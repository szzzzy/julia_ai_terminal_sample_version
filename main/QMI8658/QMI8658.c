/**
 * @file    QMI8658.c
 * @brief   QMI8658 六轴 IMU（加速度+陀螺仪）驱动，经外部 I2C_Driver 组件访问。
 *
 * 硬件连接/配置：
 * - I2C 从机地址：默认 0x6B（QMI8658_L_SLAVE_ADDRESS，SA0 接地）；0x6A 为 SA0 拉高。
 * - 默认量程：加速度 ±4g、陀螺仪 ±64dps；ODR 8000Hz（可在 QMI8658_Init 前改
 *   acc_scale/gyro_scale/acc_odr/gyro_odr 全局默认值，或运行时用 setAccODR/setGyroODR 调整）。
 * - 供电：由板级电源带起；QMI8658_Init 先读 WHO_AM_I 校验芯片，再把 acc/gyro 配到运行态。
 * - 初始化顺序：必须在 julia_context 的 context_task 使用 IMU 之前调用 QMI8658_Init()
 *   （julia_context_init() 内先 PCF85063_Init()、QMI8658_Init()，再创建 context_task）。
 *
 * 数据流（全局变量）：
 *   getAccelerometer()/getGyroscope() 读寄存器 → 换算成物理量 → 写全局 Accel/Gyro；
 *   julia_context 的 motion_detected() 读 Accel/Gyro 做“是否有人靠近/活动”判断。
 * 单位：Accel 为 g（重力加速度），Gyro 为 dps（度/秒）；raw 均按 16-bit 有符号、大端顺序
 * 在两字节中（低字节在前）解析，再乘各自 scale（range/32768）。
 *
 * 注意：I2C_Write/I2C_Read 来自外部组件（阻塞式），本驱动函数均应在任务上下文中调用。
 */

#include "QMI8658.h"

/* 全局传感器数据：由 getAccelerometer()/getGyroscope() 写入，julia_context 读取。 */
IMUdata Accel;
IMUdata Gyro;

uint8_t Device_addr ; /* 当前使用的从机地址：SA0 低=0x6B（默认），SA0 高=0x6A。 */
acc_scale_t acc_scale = ACC_RANGE_4G;         /* 加速度量程（默认 ±4g）。 */
gyro_scale_t gyro_scale = GYR_RANGE_64DPS;    /* 陀螺仪量程（默认 ±64dps）。 */
acc_odr_t acc_odr = acc_odr_norm_8000;        /* 加速度输出率（默认 8000Hz）。 */
gyro_odr_t gyro_odr = gyro_odr_norm_8000;     /* 陀螺仪输出率（默认 8000Hz）。 */
sensor_state_t sensor_state = sensor_default; /* 当前工作状态。 */
lpf_t acc_lpf;                                /* 加速度低通滤波档位。 */

/* accelScales/gyroScales：把原始 16-bit 码换算成物理量的比例（g/LSB、dps/LSB）。 */
float accelScales, gyroScales;
float accelScales = 0;   /* NOTE：与上一行重复声明 accelScales（初始化一次），归属见 QMI8658_Init。 */
uint8_t readings[12];    /* NOTE：遗留缓冲，本驱动未使用（保留未动）。 */
uint32_t reading_timestamp_us; /* NOTE：遗留时间戳（原 Arduino micros() 域），本驱动未使用。 */
/**
 * 初始化 QMI8658 并写入默认配置。
 *
 * 流程：读 WHO_AM_I（REVISION_ID）校验 → 置 sensor_running → 配置加速度（量程/ODR/LPF）
 * → 计算 accelScales(g/LSB) → 配置陀螺仪（量程/ODR/LPF）→ 计算 gyroScales(dps/LSB)。
 * 必须在 context_task 使用 IMU 之前调用。
 * I2C 为阻塞式，须在任务上下文调用。
 * @param addr I2C 地址（通常 0x6A 或 0x6B；这里固定用 QMI8658_L_SLAVE_ADDRESS）。
 */
void QMI8658_Init(void)
{
    uint8_t buf[1];
    Device_addr = QMI8658_L_SLAVE_ADDRESS;     
    I2C_Read(Device_addr, QMI8658_REVISION_ID, buf, 1);
    printf("QMI8658 Device ID: %x\r\n",buf[0]);    // Get chip id
    setState(sensor_running);             

    setAccScale(acc_scale);            
    setAccODR(acc_odr);                    
    setAccLPF(LPF_MODE_0);                  
    switch (acc_scale) {                
        // Possible accelerometer scales (and their register bit settings) are:
        // 2 Gs (00), 4 Gs (01), 8 Gs (10), and 16 Gs  (11).
        // Here's a bit of an algorith to calculate DPS/(ADC tick) based on that
        // 2-bit value:
        case ACC_RANGE_2G:  accelScales = 2.0 / 32768.0; break;
        case ACC_RANGE_4G:  accelScales = 4.0 / 32768.0; break;
        case ACC_RANGE_8G:  accelScales = 8.0 / 32768.0; break;
        case ACC_RANGE_16G: accelScales = 16.0 / 32768.0; break;
    }

    setGyroScale(gyro_scale);              
    setGyroODR(gyro_odr);                       
    setGyroLPF(LPF_MODE_3);                
    switch (gyro_scale) {                  
        // Possible gyro scales (and their register bit settings) are:
        // 250 DPS (00), 500 DPS (01), 1000 DPS (10), and 2000 DPS  (11).
        // Here's a bit of an algorith to calculate DPS/(ADC tick) based on that
        // 2-bit value:
        case GYR_RANGE_16DPS: gyroScales = 16.0 / 32768.0; break;
        case GYR_RANGE_32DPS: gyroScales = 32.0 / 32768.0; break;
        case GYR_RANGE_64DPS: gyroScales = 64.0 / 32768.0; break;
        case GYR_RANGE_128DPS: gyroScales = 128.0 / 32768.0; break;
        case GYR_RANGE_256DPS: gyroScales = 256.0 / 32768.0; break;
        case GYR_RANGE_512DPS: gyroScales = 512.0 / 32768.0; break;
        case GYR_RANGE_1024DPS: gyroScales = 1024.0 / 32768.0; break;
    }
}
/* 周期读取加速度（当前只刷新 Accel；陀螺仪由 julia_context 另行调用 getGyroscope()）。 */
void QMI8658_Loop(void)
{
  getAccelerometer();
}

/**
 * Transmit one uint8_t of data to QMI8658.
 * @param addr address of data to be written
 * @param data the data to be written
 */
void QMI8658_transmit(uint8_t addr, uint8_t data)
{
    I2C_Write(Device_addr, addr, &data, 1);
}

/**
 * Receive one uint8_t of data from QMI8658.
 * @param addr address of data to be read
 * @return the uint8_t of data that was read
 */
uint8_t QMI8658_receive(uint8_t addr)
{
    uint8_t retval;
    I2C_Read(Device_addr, addr, &retval, 1);
    return retval;
}

/**
 * 向 CTRL9（主机命令寄存器）写命令并忙等命令完成。
 * @param command 要执行的命令（如校准/锁定相关指令）。
 * 实现：写命令后轮询 STATUSINT 寄存器的 bit7（命令忙标志），直到为 0。
 * NOTE：这是阻塞式忙等（无超时），若 I2C 异常或命令未完成会卡死；仅用于
 * 较罕见的加锁/校准命令，常规读取不经过这里。
 */
void QMI8658_CTRL9_Write(uint8_t command)
{
    // transmit command
    QMI8658_transmit(QMI8658_CTRL9, command);

    // wait for command to be done
    while (((QMI8658_receive(QMI8658_STATUSINT)) & 0x80) == 0x00);
}

/**
 * 设置加速度输出率（ODR）。
 * @param odr acc_odr_t 表示新的输出率。
 * 实现：仅在非 sensor_default 状态写寄存器（QMI8658_Init 前后行为不同——出厂默认态
 * 下只缓存，不写），写时读 CTRL2、清 AODR_MASK(0x0F) 再或入新值，保留其余位。
 */
void setAccODR(acc_odr_t odr)
{
    if (sensor_state != sensor_default)                     // If the device is not in the default state
    {
        uint8_t ctrl2 = QMI8658_receive(QMI8658_CTRL2);
        ctrl2 &= ~QMI8658_AODR_MASK;                        // clear previous setting
        ctrl2 |= odr;                                       // OR in new setting
        QMI8658_transmit(QMI8658_CTRL2, ctrl2);
    }
    acc_odr = odr;
}

/**
 * 设置陀螺仪输出率（ODR），语义同 setAccODR（作用于 CTRL3，清 GODR_MASK 再或入）。
 * @param odr gyro_odr_t 表示新的输出率。
 */
void setGyroODR(gyro_odr_t odr)
{
    if (sensor_state != sensor_default)
    {
    uint8_t ctrl3 = QMI8658_receive(QMI8658_CTRL3);
    ctrl3 &= ~QMI8658_GODR_MASK; // clear previous setting
    ctrl3 |= odr; // OR in new setting
    QMI8658_transmit(QMI8658_CTRL3, ctrl3);
    }
    gyro_odr = odr;
}

/**
 * 设置加速度量程。
 * @param scale acc_scale_t 表示新的量程（2/4/8/16g）。
 * 实现：读 CTRL2、清 ASCALE_MASK(0x70) 再或入 (scale<<ASCALE_OFFSET(4))，保留其余位。
 * 注意：这里只写寄存器并缓存 acc_scale，accelScales(g/LSB) 只在 QMI8658_Init 里按当前
 * acc_scale 计算一次；若在 Init 之后改量程，需重新计算 accelScales 才能得到正确的 g 值。
 */
void setAccScale(acc_scale_t scale)
{
    if (sensor_state != sensor_default)
    {
    uint8_t ctrl2 = QMI8658_receive(QMI8658_CTRL2);
    ctrl2 &= ~QMI8658_ASCALE_MASK; // clear previous setting
    ctrl2 |= scale << QMI8658_ASCALE_OFFSET; // OR in new setting
    QMI8658_transmit(QMI8658_CTRL2, ctrl2);
    }
    acc_scale = scale;
}

/**
 * 设置陀螺仪量程（作用于 CTRL3，清 GSCALE_MASK 再或入 scale<<GSCALE_OFFSET(4)）。
 * 注意：与 setAccScale 相同，修改后 gyroScales 需在 Init 中重算；见 setAccScale 说明。
 * @param scale gyro_scale_t 表示新的量程。
 */
void setGyroScale(gyro_scale_t scale)
{
    if (sensor_state != sensor_default)
    {
    uint8_t ctrl3 = QMI8658_receive(QMI8658_CTRL3);
    ctrl3 &= ~QMI8658_GSCALE_MASK; // clear previous setting
    ctrl3 |= scale << QMI8658_GSCALE_OFFSET; // OR in new setting
    QMI8658_transmit(QMI8658_CTRL3, ctrl3);
    }
    gyro_scale = scale;
}

/**
 * 设置加速度低通滤波档位（作用于 CTRL5）。
 * @param lp lpf_t 表示低通比例（见 QMI8658.h 的 LPF_MODE_*）。
 * NOTE：这里用 `!QMI8658_ALPF_MASK`（逻辑非）来“清掩码位”，其值为 0，会
 * `ctrl5 &= 0` 把整个 CTRL5 清零——既清掉了保留位，也顺带清掉陀螺仪 LPF 位。
 * 这看起来是笔误（应为按位取反 `~QMI8658_ALPF_MASK`）。由于只在初始化时调用一次，
 * 且随后立即重新写入 acc LPF 位，实际影响有限，但存在破坏其他位的风险。
 */
void setAccLPF(lpf_t lpf)
{
    if (sensor_state != sensor_default)
    {
    uint8_t ctrl5 = QMI8658_receive(QMI8658_CTRL5);
    ctrl5 &= !QMI8658_ALPF_MASK;
    ctrl5 |= lpf << QMI8658_ALPF_OFFSET;
    ctrl5 |= 0x01; // turn on acc low pass filter
    QMI8658_transmit(QMI8658_CTRL5, ctrl5);
    }
    acc_lpf = lpf;
}

/**
 * 设置陀螺仪低通滤波档位（作用于 CTRL5）。
 * @param lp lpf_t 表示低通比例。
 * NOTE：与 setAccLPF 相同，`!QMI8658_GLPF_MASK`（逻辑非）值为 0，会导致
 * `ctrl5 &= 0` 清空整个 CTRL5（包括加速度 LPF 位），再写入陀螺仪 LPF 位。
 * 这是与 setAccLPF 同源的笔误风险，建议改用按位取反 `~`。
 */
void setGyroLPF(lpf_t lpf)
{
    if (sensor_state != sensor_default)
    {
    uint8_t ctrl5 = QMI8658_receive(QMI8658_CTRL5);
    ctrl5 &= !QMI8658_GLPF_MASK;
    ctrl5 |= lpf << QMI8658_GLPF_OFFSET;
    ctrl5 |= 0x10; // turn on gyro low pass filter
    QMI8658_transmit(QMI8658_CTRL5, ctrl5);
    }
}

/**
 * 设置 QMI8658 工作状态（运行/下电/锁定）。
 *
 * 三个分支对 CTRL1/CTRL7/CTRL6/CAL1_L 的不同写做：
 *  - sensor_running：启用 2MHz 振荡器（CTRL1 bit0=0）、地址自增（bit6=1），CTRL7=0x43
 *    （加速度+陀螺仪全速、启用高速内部时钟、关 syncSample），CTRL6=0x00（关 AttitudeEngine）。
 *  - sensor_power_down：CTRL7=0x00（关闭加速/陀螺）、CTRL1 bit0=1（关 2MHz 振荡器）。
 *  - sensor_locking：与 running 相似但 CTRL7=0x83（开 syncSample），并对 CAL1_L 做
 *    “关 AHB 时钟门控 → CTRL9 指令(0x12) → 恢复时钟门控”的锁定握手。
 * @param state 新状态；sensor_default 不处理（保持现状）。
 */
void setState(sensor_state_t state)
{
    uint8_t ctrl1;
    switch (state)
    {
    case sensor_running:
        ctrl1 = QMI8658_receive(QMI8658_CTRL1);
        // enable 2MHz oscillator
        ctrl1 &= 0xFE;
        // enable auto address increment for fast block reads
        ctrl1 |= 0x40;
        QMI8658_transmit(QMI8658_CTRL1, ctrl1);

        // enable high speed internal clock,
        // acc and gyro in full mode, and
        // disable syncSample mode
        QMI8658_transmit(QMI8658_CTRL7, 0x43);

        // disable AttitudeEngine Motion On Demand
        QMI8658_transmit(QMI8658_CTRL6, 0x00);
        break;
    case sensor_power_down:
        // disable high speed internal clock,
        // acc and gyro powered down
        QMI8658_transmit(QMI8658_CTRL7, 0x00);

        ctrl1 = QMI8658_receive(QMI8658_CTRL1);
        // disable 2MHz oscillator
        ctrl1|= 0x01;
        QMI8658_transmit(QMI8658_CTRL1, ctrl1);
        break;
    case sensor_locking:
        ctrl1 = QMI8658_receive(QMI8658_CTRL1);
        // enable 2MHz oscillator
        ctrl1 &= 0xFE;
        // enable auto address increment for fast block reads
        ctrl1 |= 0x40;
        QMI8658_transmit(QMI8658_CTRL1, ctrl1);

        // enable high speed internal clock,
        // acc and gyro in full mode, and
        // enable syncSample mode
        QMI8658_transmit(QMI8658_CTRL7, 0x83);

        // disable AttitudeEngine Motion On Demand
        QMI8658_transmit(QMI8658_CTRL6, 0x00);

        // disable internal AHB clock gating:
        QMI8658_transmit(QMI8658_CAL1_L, 0x01);
        QMI8658_CTRL9_Write(0x12);
        // re-enable clock gating
        QMI8658_transmit(QMI8658_CAL1_L, 0x00);
        QMI8658_CTRL9_Write(0x12);
        break;
    default:
        break;
    }
    sensor_state = state;
}


/**
 * 读取加速度并换算成 g，写入全局 Accel。
 * @note 从 AX_L(0x35) 起连续读 6 字节（每轴低字节在前，16-bit 有符号），组合后再乘
 * accelScales（g/LSB）。会直接改写全局 Accel，调用方读取即可。I2C 为阻塞式。
 */
void getAccelerometer(void)
{

    uint8_t buf[6];
    I2C_Read(Device_addr, QMI8658_AX_L, buf, 6);
    Accel.x = (float)((int16_t)((buf[1]<<8) | (buf[0])));
    Accel.y = (float)((int16_t)((buf[3]<<8) | (buf[2])));
    Accel.z = (float)((int16_t)((buf[5]<<8) | (buf[4])));
    Accel.x = Accel.x * accelScales;
    Accel.y = Accel.y * accelScales;
    Accel.z = Accel.z * accelScales;

}
/**
 * 读取陀螺仪并换算成 dps，写入全局 Gyro。
 * @note 从 GX_L(0x3B) 起连续读 6 字节（低字节在前，16-bit 有符号），乘以 gyroScales
 * （dps/LSB）。会改写全局 Gyro；与 getAccelerometer 一起供 julia_context 做姿态/活动判断。
 */
void getGyroscope(void)
{
    uint8_t buf[6];
    I2C_Read(Device_addr, QMI8658_GX_L, buf, 6);
    Gyro.x = (float)((int16_t)((buf[1]<<8) | (buf[0])));
    Gyro.y = (float)((int16_t)((buf[3]<<8) | (buf[2])));
    Gyro.z = (float)((int16_t)((buf[5]<<8) | (buf[4])));
    Gyro.x = Gyro.x * gyroScales;
    Gyro.y = Gyro.y * gyroScales;
    Gyro.z = Gyro.z * gyroScales;
}

















