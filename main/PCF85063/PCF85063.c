/*****************************************************************************
* | File      	:   PCF85063.c
* | Author      :   Waveshare team
* | Function    :   PCF85063 driver
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2024-02-02
* | Info        :   Basic version
*
******************************************************************************/
/*
 * 当前构建不包含本文件，运行时使用 hardware/pcf85063_shared.c。两套实现操作同一
 * 0x51 设备且采用不同 bus API，不得同时初始化。以下供应商头注和函数保留作参考，
 * 不代表当前 RTC 行为。
 *
 * 补充说明（参考实现上下文）：
 * - 本文件是 Waveshare 移植的 PCF85063 完整驱动（含闹钟、复位、报警等），
 *   经外部 I2C_Driver 组件（I2C_Write/I2C_Read）访问 RTC，从机地址 0x51。
 * - 旧上游 main/context/julia_context.c 同样未参与当前构建。
 * - 时间字段均为十进制（decToBcd/bcdToDec 已转换），年份为 0~99 存寄存器，真实
 *   年份 = 1970 + 寄存器值（YEAR_OFFSET）。
 */
#include "PCF85063.h"

/* 全局时间缓存：PCF85063_Loop()/Read_Time() 写；供外部读取当前时间。 */
datetime_t datetime= {0};

static uint8_t decToBcd(int val);
static int bcdToDec(uint8_t val);

const unsigned char MonthStr[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov","Dec"};
/******************************************************************************
function:	PCF85063 initialized
parameter:
            
Info:Initiate Normal Mode, RTC Run, NO reset, No correction , 24hr format, Internal load capacitane 12.5pf
******************************************************************************/
/* 初始化：写 CTRL1 = CAP_SEL（12.5pF），即普通模式、RTC 运行、无复位、无校正、24 小时制。 */
void PCF85063_Init()
{
	uint8_t Value = RTC_CTRL_1_DEFAULT|RTC_CTRL_1_CAP_SEL;

	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_CTRL_1_ADDR, &Value, 1));

	// datetime_t Now_datetime= {0};
	// Now_datetime.year = 2024;
	// Now_datetime.month = 9;
	// Now_datetime.day = 20;
	// Now_datetime.dotw = 5;
	// Now_datetime.hour = 9;
	// Now_datetime.minute = 50;
	// Now_datetime.second = 0;
	// PCF85063_Set_All(Now_datetime);
}

/* 周期读取并把结果写入全局 datetime，供外部获取当前时间。 */
void PCF85063_Loop(void)
{
  PCF85063_Read_Time(&datetime);
}
/******************************************************************************
function:	Reset PCF85063
parameter:
Info:		
******************************************************************************/
void PCF85063_Reset()
{
	uint8_t Value = RTC_CTRL_1_DEFAULT|RTC_CTRL_1_CAP_SEL|RTC_CTRL_1_SR;
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_CTRL_1_ADDR, &Value, 1));
}

/******************************************************************************
function:	Set Time 
parameter:
Info:		
******************************************************************************/
/* 仅写“时分秒”三字节（从秒寄存器 0x04 起）。字段传十进制，内部转 BCD。 */
void PCF85063_Set_Time(datetime_t time)
{
	uint8_t buf[3] = {decToBcd(time.second),
					  decToBcd(time.minute),
					  decToBcd(time.hour)};
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_SECOND_ADDR, buf, 3));
}

/******************************************************************************
function:	Set Date
parameter:
Info:		
******************************************************************************/
/* 仅写“日/星期/月/年”四字节（从日寄存器 0x07 起）。年份 = 传入 year - YEAR_OFFSET。 */
void PCF85063_Set_Date(datetime_t date)
{
	uint8_t buf[4] = {decToBcd(date.day),
					  decToBcd(date.dotw),
					  decToBcd(date.month),
					  decToBcd(date.year - YEAR_OFFSET)};
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_DAY_ADDR, buf, 4));
}

/******************************************************************************
function:	Set Time And Date
parameter:
Info:		
******************************************************************************/
/*
 * 一次性写完整时间+日期（7 字节，从秒寄存器 0x04 起连续写：秒.分.时.日.星期.月.年）。
 * 注意并不停振/启振，RTC 在写入期间继续走秒；写入若跨秒进位会导致秒/分轻微不一致，
 * 但一般校时场景可接受（更严谨应先在 CTRL1 置 STOP 再写、写完清 STOP）。
 */
void PCF85063_Set_All(datetime_t time)
{
	uint8_t buf[7] = {decToBcd(time.second),
					  decToBcd(time.minute),
					  decToBcd(time.hour),
					  decToBcd(time.day),
					  decToBcd(time.dotw),
					  decToBcd(time.month),
					  decToBcd(time.year - YEAR_OFFSET)};
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_SECOND_ADDR, buf, 7));
}

/******************************************************************************
function:	Read Time And Date
parameter:
Info:		
******************************************************************************/
/*
 * 读(04h)起 7 字节 → BCD 解码。各字段掩码含义（参考 PCF85063 数据手册）：
 *   秒 &0x7F：bit7 是 OS（振荡停止）标志，读数时清零；
 *   分 &0x7F：bit7 保留，清零；
 *   时 &0x3F：bit6=12/24 制、bit5=AM/PM；24 制下只用低 6 位；
 *   日 &0x3F；星期 &0x07（0=周日…6=周六）；月 &0x1F（bit5 为世纪标志）；
 *   年= 解码值 + YEAR_OFFSET(1970)。
 */
void PCF85063_Read_Time(datetime_t *time)
{
	uint8_t buf[7] = {0};
	ESP_ERROR_CHECK(I2C_Read(PCF85063_ADDRESS, RTC_SECOND_ADDR, buf, 7));
	time->second = bcdToDec(buf[0] & 0x7F);
	time->minute = bcdToDec(buf[1] & 0x7F);
	time->hour = bcdToDec(buf[2] & 0x3F);
	time->day = bcdToDec(buf[3] & 0x3F);
	time->dotw = bcdToDec(buf[4] & 0x07);
	time->month = bcdToDec(buf[5] & 0x1F);
	time->year = bcdToDec(buf[6])+YEAR_OFFSET;
}

/******************************************************************************
function:	Enable Alarm and Clear Alarm flag
parameter:			
Info:		
******************************************************************************/
void PCF85063_Enable_Alarm()
{
	uint8_t Value = RTC_CTRL_2_DEFAULT | RTC_CTRL_2_AIE;
	Value &= ~RTC_CTRL_2_AF;
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_CTRL_2_ADDR, &Value, 1));
}

/******************************************************************************
function:	Get Alarm flay
parameter:			
Info:		
******************************************************************************/
uint8_t PCF85063_Get_Alarm_Flag()
{
	uint8_t Value = 0;
	ESP_ERROR_CHECK(I2C_Read(PCF85063_ADDRESS, RTC_CTRL_2_ADDR, &Value, 1));
	//printf("Value = 0x%x",Value);
	Value &= RTC_CTRL_2_AF | RTC_CTRL_2_AIE;
	return Value;
}

/******************************************************************************
function:	Set Alarm
parameter:			
Info:		
******************************************************************************/
/*
 * 写闹钟（从 0x0B 起，6 字节）。前 3 字节为秒/分/时闹钟值（清掉各自 AEN_ 位表示启用该字段），
 * 第 4/5 字节为日/星期闹钟，这里用 RTC_ALARM(0x80) 置 AEN_ 位=1 表示“禁用日/星期闹钟”。
 * NOTE：本函数把 buf 声明为 5 字节却调用 I2C_Write(..., 6)，第 6 字节（星期闹钟寄存器）
 * 读的是数组越界（栈上无效值）。若要用到日/星期闹钟请修正该长度/缓冲区。
 */
void PCF85063_Set_Alarm(datetime_t time)
{

	uint8_t buf[5] ={
		decToBcd(time.second)&(~RTC_ALARM),
		decToBcd(time.minute)&(~RTC_ALARM),
		decToBcd(time.hour)&(~RTC_ALARM),
		//decToBcd(time.day)&(~RTC_ALARM),
		//decToBcd(time.dotw)&(~RTC_ALARM)
		RTC_ALARM, 	//disalbe day
		RTC_ALARM	//disalbe weekday
	};
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_SECOND_ALARM, buf, 6));
}

/******************************************************************************
function:	Read Alarm
parameter:			
Info:		
******************************************************************************/
/* 读闹钟寄存器（0x0B 起 6 字节）并解码（掩码与 Read_Time 一致，排除各 AEN_ 位）。 */
void PCF85063_Read_Alarm(datetime_t *time)
{
	uint8_t bufss[6] = {0};
	ESP_ERROR_CHECK(I2C_Read(PCF85063_ADDRESS, RTC_SECOND_ALARM, bufss, 6));
	time->second = bcdToDec(bufss[0] & 0x7F);
	time->minute = bcdToDec(bufss[1] & 0x7F);
	time->hour = bcdToDec(bufss[2] & 0x3F);
	time->day = bcdToDec(bufss[3] & 0x3F);
	time->dotw = bcdToDec(bufss[4] & 0x07);
}


/******************************************************************************
function:	Convert normal decimal numbers to binary coded decimal
parameter:			
Info:		
******************************************************************************/
/* 十进制 → BCD：val/10 为十位（×16 即左移 4 位），val%10 为个位。val 须在 0~99。 */
static uint8_t decToBcd(int val)
{
	return (uint8_t)((val / 10 * 16) + (val % 10));
}

/******************************************************************************
function:	Convert binary coded decimal to normal decimal numbers
parameter:			
Info:		
******************************************************************************/
/* BCD → 十进制：高半字节 ×10 + 低半字节。 */
static int bcdToDec(uint8_t val)
{
	return (int)((val / 16 * 10) + (val % 16));
}

/******************************************************************************
function:	
parameter:	
Info:		
******************************************************************************/
/* 把时间结构格式化为可读字符串写入 datetime_str（调用方需保证缓冲区足够大）。 */
void datetime_to_str(char *datetime_str,datetime_t time)
{
	sprintf(datetime_str, " %d.%d.%d  %d %d:%d:%d ", time.year, time.month, 
			time.day, time.dotw, time.hour, time.minute, time.second);
}
