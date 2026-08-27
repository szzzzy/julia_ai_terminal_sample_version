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
#define RMT_RESOLUTION_HZ 10000000
#define UPDATE_MS 80

typedef enum { LED_OFF, LED_SOLID, LED_BREATHING } led_mode_t;
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes;
    rmt_encoder_t *copy;
    rmt_symbol_word_t reset;
    int state;
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

static void configure(led_mode_t mode, uint8_t lo, uint8_t hi, uint16_t period, uint32_t color)
{ xSemaphoreTake(s_lock, portMAX_DELAY); s_mode=mode; s_min=lo; s_max=hi; s_solid=hi; s_period=period; s_color=color; xSemaphoreGive(s_lock); }
void julia_led_set_breathing(uint8_t lo, uint8_t hi, uint16_t ms, uint32_t color)
{ if (lo > 100) lo=100; if (hi > 100) hi=100; if (lo > hi) { uint8_t t=lo; lo=hi; hi=t; } configure(LED_BREATHING,lo,hi,ms,color); }
void julia_led_set_solid(uint8_t brightness, uint32_t color)
{ if (brightness > 100) brightness=100; configure(LED_SOLID,brightness,brightness,0,color); }
void julia_led_set_off(void) { configure(LED_OFF,0,0,0,0); }

void julia_led_set_emotion(emotion_t emotion)
{
    static const uint32_t colors[] = {0xFFEE00,0x0088FF,0xFF0000,0xFFAA88,0xFF8A35,0xC8A8FF};
    if (emotion > JULIA_EMOTION_WORRIED) {
        return;
    }
    s_emotion = emotion;
    julia_led_set_solid(70, colors[emotion]);
}

uint32_t julia_led_hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v)
{
    h %= 360; if (s > 100) s=100; if (v > 100) v=100;
    uint8_t max=v*255/100, min=max*(100-s)/100, d=(max-min)*(h%60)/60; uint8_t r,g,b;
    switch(h/60){case 0:r=max;g=min+d;b=min;break;case 1:r=max-d;g=max;b=min;break;case 2:r=min;g=max;b=min+d;break;case 3:r=min;g=max-d;b=max;break;case 4:r=min+d;g=min;b=max;break;default:r=max;g=min;b=max-d;}
    return ((uint32_t)r<<16)|((uint32_t)g<<8)|b;
}
