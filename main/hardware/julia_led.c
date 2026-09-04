/**
 * @file    julia_led.c
 * @brief   用 RMT 发送 WS2812 时序，并由唯一刷新任务异步应用亮度配置。
 *
 * set_* 调用方只写配置，led_task 是 RMT channel 和发送时序的 owner。单帧发送期间
 * 持有 ESP_PM_NO_LIGHT_SLEEP lock，防止 Light-sleep 破坏波形；错误后永久停发，避免
 * 每 80 ms 重复冲击故障外设和日志。
 *
 * 当前应用没有调用 julia_led_init()，因此本模块虽参与编译仍不是已接通能力。主工程
 * 把 JULIA_LED_GPIO 覆盖为 GPIO4；若其它 target 落回头文件的 GPIO21，会与 LCD CS 冲突。
 */

#include "julia_led.h"

#include <math.h>
#include <stdlib.h>
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "JULIA_LED"

/** RMT 时钟源分辨率：10 MHz，即每个 tick = 0.1 us。WS2812 的 bit 占空比都用它换算。 */
#define RMT_RESOLUTION_HZ 10000000
/* 80 ms 是当前动画采样周期，不是 WS2812 协议要求或实测最小值。 */
#define UPDATE_MS 80

/* led_task 使用的三种输出模式。 */
typedef enum { LED_OFF, LED_SOLID, LED_BREATHING } led_mode_t;

/**
 * @brief 自定义 RMT 编码器：把 3 字节 GRB 数据编码成 WS2812 帧。
 *
 * 结构上它“组合”两个子编码器，按固定顺序在每一帧里依次触发：
 *   bytes —— rmt_bytes_encoder，负责把 3 字节像素编成高/低电平片段；
 *   copy  —— rmt_copy_encoder，负责把 reset（帧尾低电平）原样地追加在后面。
 * base 必须是首成员，RMT 才能由公共 handle 还原组合对象。state 保证数据未完整编码时
 * 保留阶段，只有完整 GRB 后才追加 reset，不能在 MEM_FULL 时提前结束帧。
 */
typedef struct {
    rmt_encoder_t base;        /* RMT 编码器基类（必须放在第一个成员）。 */
    rmt_encoder_t *bytes;      /* 比特编码器：把 GRB 像素数据编成高低位。 */
    rmt_encoder_t *copy;       /* 拷贝编码器：追加帧尾 reset 符号。 */
    rmt_symbol_word_t reset;   /* 帧尾低电平符号（WS2812 复位）。 */
    int state;                 /* 0 = 待发数据；1 = 数据已发、待发 reset。 */
} ws2812_encoder_t;

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static SemaphoreHandle_t s_lock;
static esp_pm_lock_handle_t s_pm_lock;
static led_mode_t s_mode;
static uint8_t s_min, s_max, s_solid;
static uint16_t s_period;
static uint32_t s_color;
static emotion_t s_emotion = JULIA_EMOTION_CALM;

/**
 * @brief RMT 编码回调（每次 DMA 搬运一帧时被调用）。
 *
 * 一次完整的 WS2812 帧 = 3 个字节的位序列 + 一个帧尾 reset。state 机解释：
 *   state 0：先把 3 字节数据交给 bytes 子编码器；若一次性编完则置 state=1 准备发 reset。
 *   state 1：把 reset 符号交给 copy 子编码器；发完即复位 state=0。
 * RMT_ENCODING_MEM_FULL 表示 RMT 内存已满需要等下轮继续编，此时不置 COMPLETE 位，
 * 交由 RMT 驱动在下一轮继续调用。
 */
static size_t IRAM_ATTR ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                      const void *data, size_t size, rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session = RMT_ENCODING_RESET, state = RMT_ENCODING_RESET;
    size_t symbols = 0;
    if (ws->state == 0) {
        symbols += ws->bytes->encode(ws->bytes, channel, data, size, &session);
        if (session & RMT_ENCODING_COMPLETE) ws->state = 1;
        if (session & RMT_ENCODING_MEM_FULL) goto out;
    }
    symbols += ws->copy->encode(ws->copy, channel, &ws->reset, sizeof(ws->reset), &session);
    if (session & RMT_ENCODING_COMPLETE) { ws->state = 0; state |= RMT_ENCODING_COMPLETE; }
out:
    if (session & RMT_ENCODING_MEM_FULL) state |= RMT_ENCODING_MEM_FULL;
    *ret_state = state;
    return symbols;
}

static esp_err_t ws2812_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(ws->bytes); rmt_del_encoder(ws->copy); free(ws);
    return ESP_OK;
}

static esp_err_t IRAM_ATTR ws2812_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(ws->bytes); rmt_encoder_reset(ws->copy); ws->state = 0;
    return ESP_OK;
}

/**
 * @brief 初始化 WS2812 专用编码器并分配其子编码器。
 *
 * WS2812 时序（以 RMT_RESOLUTION_HZ=10MHz、1 tick=0.1us 为单位）：
 *   bit0 = 高 3 tick（0.3us） + 低 9 tick（0.9us）；
 *   bit1 = 高 9 tick（0.9us） + 低 3 tick（0.3us）；
 *   先发高有效位（msb_first）。
 * 帧尾 reset 为 250 tick 低 + 250 tick 低（合计 50us 低电平，满足 >24us 复位要求）。
 * 失败时逐个释放已分配的子编码器并释放本结构。
 */
static esp_err_t new_ws2812_encoder(rmt_encoder_handle_t *result)
{
    ws2812_encoder_t *ws = rmt_alloc_encoder_mem(sizeof(*ws));
    ESP_RETURN_ON_FALSE(ws, ESP_ERR_NO_MEM, TAG, "encoder allocation failed");
    ws->base.encode = ws2812_encode; ws->base.del = ws2812_del; ws->base.reset = ws2812_reset;
    rmt_bytes_encoder_config_t bytes = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    esp_err_t err = rmt_new_bytes_encoder(&bytes, &ws->bytes);
    if (err == ESP_OK) { rmt_copy_encoder_config_t copy = {}; err = rmt_new_copy_encoder(&copy, &ws->copy); }
    if (err != ESP_OK) { if (ws->bytes) rmt_del_encoder(ws->bytes); free(ws); return err; }
    ws->reset = (rmt_symbol_word_t){.level0 = 0, .duration0 = 250, .level1 = 0, .duration1 = 250};
    *result = &ws->base;
    return ESP_OK;
}

/**
 * @brief 把一次亮度/颜色输出写入 RMT（核心发送例程，阻塞等待发送完成）。
 *
 * 处理流程：
 *  - 先把颜色按 brightness（0~100）缩放到 r/g/b（都是 0~255），再按 WS2812 的
 *    像素格式拼成 {g, r, b} —— 注意是 GRB 而非常见的 RGB，顺序写错会颜色错位。
 *  - 发送前取 ESP_PM_NO_LIGHT_SLEEP 功率锁，发送完成后再释放，避免 Light-sleep
 *    在发送中途切入而打断 DMA。
 *  - 用 rmt_tx_wait_all_done 阻塞至多 100ms 等通道发完。
 *
 * @param[in] brightness 亮度百分比（0~100），会作用到三个颜色通道上。
 * @param[in] color      颜色 0xRRGGBB。
 * 失败路径：一旦 RMT 发送出错就置 output_failed 并永久停用输出（避免每 80ms 反复
 * 报错刷日志）；此后本函数直接返回。
 */
static void output(uint8_t brightness, uint32_t color)
{
    static bool output_failed;
    if (output_failed) return;
    uint8_t r = ((color >> 16) & 0xff) * brightness / 100;
    uint8_t g = ((color >> 8) & 0xff) * brightness / 100;
    uint8_t b = (color & 0xff) * brightness / 100;
    uint8_t grb[3] = {g, r, b};
    rmt_transmit_config_t tx = {.loop_count = 0};
    esp_pm_lock_acquire(s_pm_lock);
    esp_err_t err = rmt_transmit(s_channel, s_encoder, grb, sizeof(grb), &tx);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(s_channel, pdMS_TO_TICKS(100));
    esp_pm_lock_release(s_pm_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED output disabled after RMT error: %s", esp_err_to_name(err));
        output_failed = true;
    }
}

/**
 * @brief LED 刷新任务主体（FreeRTOS 任务，频率 = UPDATE_MS=80ms 一个周期）。
 *
 * 每次循环先在锁内快照一份当前配置（mode/lo/hi/period/color），拿到配置后才释放锁，
 * 之后在本地计算亮度，配置对象不会被别的任务修改，因此无需全程持锁：
 *   - LED_OFF：亮度 0；
 *   - LED_SOLID：固定 brightness=solid；
 *   - LED_BREATHING：沿 period 做三角波，并用二次缓动（ease-in/out）得到
 *     更柔和的“呼吸”亮度曲线——q10 是 0~1024 的相位系数，brightness = lo + (hi-lo)*q10/1024。
 * 中途调用 output() 会阻塞至多 100ms（RMT 等待完成），因此本任务可以被抢占，
 * 但不应在中断上下文里调用。
 */
static void led_task(void *arg)
{
    (void)arg; uint32_t elapsed = 0;
    while (1) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        led_mode_t mode = s_mode; uint8_t lo = s_min, hi = s_max, solid = s_solid;
        uint16_t period = s_period; uint32_t color = s_color;
        xSemaphoreGive(s_lock);
        uint8_t brightness = 0;
        if (mode == LED_SOLID) brightness = solid;
        else if (mode == LED_BREATHING && period) {
            uint32_t phase = elapsed % period;
            uint32_t half = period / 2U;
            uint32_t q10 = (phase <= half ? phase : period - phase) * 1024U / half;
            q10 = q10 < 512U ? 2U * q10 * q10 / 1024U
                              : 1024U - 2U * (1024U - q10) * (1024U - q10) / 1024U;
            brightness = lo + (uint8_t)((hi - lo) * q10 / 1024U);
        }
        output(brightness, color); elapsed += UPDATE_MS;
        vTaskDelay(pdMS_TO_TICKS(UPDATE_MS));
    }
}

/**
 * @brief 初始化 LED（RMT 通道 + 编码器 + 功率锁 + 后台刷新任务）。
 *
 * 一次性初始化，成功后即可调用 julia_led_set_*。初始化失败会返回 esp_err_t，
 * 且每一段失败都会提前返回（不继续执行，避免半初始化状态被使用）。
 * 注意：本函数须在任务上下文调用（内部会创建互斥锁、创建任务），不要在中断里调用。
 * 后续所有 julia_led_set_* 都通过 led_task 输出，因此调用方无需再手动触发发送。
 */
esp_err_t julia_led_init(void)
{
    rmt_tx_channel_config_t cfg = {.clk_src = RMT_CLK_SRC_DEFAULT, .gpio_num = JULIA_LED_GPIO,
        .mem_block_symbols = 64, .resolution_hz = RMT_RESOLUTION_HZ, .trans_queue_depth = 4};
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&cfg, &s_channel), TAG, "RMT channel failed");
    ESP_RETURN_ON_ERROR(new_ws2812_encoder(&s_encoder), TAG, "encoder failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_channel), TAG, "RMT enable failed");
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "julia_led", &s_pm_lock),
                        TAG, "PM lock failed");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(led_task, "julia_led", 3072, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task failed");
    ESP_LOGI(TAG, "WS2812 initialized on GPIO %d", JULIA_LED_GPIO);
    return ESP_OK;
}

/**
 * @brief 在锁内写入一组 LED 参数（配置的单一写入口）。
 *
 * 所有 julia_led_set_* 最终都汇聚到此，通过 s_lock 串行化写配置，
 * 避免与 led_task 快照配置竞争。这里只存参数；真正的亮度计算/发送由 led_task
 * 在下一个周期完成，因此调用方得到的是“已请求”，不是“已输出”。
 */
static void configure(led_mode_t mode, uint8_t lo, uint8_t hi, uint16_t period, uint32_t color)
{ xSemaphoreTake(s_lock, portMAX_DELAY); s_mode=mode; s_min=lo; s_max=hi; s_solid=hi; s_period=period; s_color=color; xSemaphoreGive(s_lock); }

/**
 * @brief 请求呼吸灯效果。
 *
 * @param[in] lo     亮度下限 0~100（超界会被钳到 100）。
 * @param[in] hi     亮度上限 0~100。
 * @param[in] ms     呼吸周期（ms）。
 * @param[in] color  颜色 0xRRGGBB。
 * 若 lo>hi 会自动交换。period=0 时刷新任务不会进入呼吸分支，输出为 0；调用者需要
 * 常亮时必须使用 julia_led_set_solid()。
 */
void julia_led_set_breathing(uint8_t lo, uint8_t hi, uint16_t ms, uint32_t color)
{ if (lo > 100) lo=100; if (hi > 100) hi=100; if (lo > hi) { uint8_t t=lo; lo=hi; hi=t; } configure(LED_BREATHING,lo,hi,ms,color); }

/**
 * @brief 请求固定亮度常量亮（period 为 0，呼吸曲线退化为常亮）。
 * @param[in] brightness 0~100（超界钳到 100）。
 * @param[in] color      颜色 0xRRGGBB。
 */
void julia_led_set_solid(uint8_t brightness, uint32_t color)
{ if (brightness > 100) brightness=100; configure(LED_SOLID,brightness,brightness,0,color); }

/** 请求关闭 LED（亮度为 0）。 */
void julia_led_set_off(void) { configure(LED_OFF,0,0,0,0); }

/**
 * @brief 按情感枚举设置对应常亮配色（内部映射到固定颜色表，亮度固定 70%）。
 *
 * @param[in] emotion 情感枚举，范围 [JULIA_EMOTION_HAPPY, JULIA_EMOTION_WORRIED]，
 *                    超出范围会被忽略（保持当前设定不变）。这是 UI/状态机用来
 *                    表达“当前情绪”的快捷入口，颜色值与字母一一对应。
 */
void julia_led_set_emotion(emotion_t emotion)
{
    static const uint32_t colors[] = {0xFFEE00,0x0088FF,0xFF0000,0xFFAA88,0xFF8A35,0xC8A8FF};
    if (emotion > JULIA_EMOTION_WORRIED) {
        return;
    }
    s_emotion = emotion;
    julia_led_set_solid(70, colors[emotion]);
}

/**
 * @brief HSV → RGB 转换（供配色计算）。
 *
 * @param[in] h 色相 0~360（超出按 360 取模）。
 * @param[in] s 饱和度 0~100 百分比（超界钳到 100）。
 * @param[in] v 明度 0~100 百分比（超界钳到 100）。
 * @return 颜色 0xRRGGBB。
 * 注意 HSV 的 v 是“百分比”，内部先换算成 0~255 通道值再插值，避免整数溢出。
 * 这是纯计算函数，无副作用、可重入。
 */
uint32_t julia_led_hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v)
{
    h %= 360; if (s > 100) s=100; if (v > 100) v=100;
    uint8_t max=v*255/100, min=max*(100-s)/100, d=(max-min)*(h%60)/60; uint8_t r,g,b;
    switch(h/60){case 0:r=max;g=min+d;b=min;break;case 1:r=max-d;g=max;b=min;break;case 2:r=min;g=max;b=min+d;break;case 3:r=min;g=max-d;b=max;break;case 4:r=min+d;g=min;b=max;break;default:r=max;g=min;b=max-d;}
    return ((uint32_t)r<<16)|((uint32_t)g<<8)|b;
}
