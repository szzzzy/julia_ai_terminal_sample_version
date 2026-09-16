/**
 * @file board_audio.c
 * @brief 实现板级 I2S 采集、PCM1 fanout 和互斥的 Speaker 输出。
 *
 * MIC task 独占 mic_raw、mic_pcm、预录 ring 和 PCM1 组帧缓冲。注册的 sink 在该任务
 * 上下文同步执行，不能保留传入指针。Speaker channel 只能在 s_spk_lock 下配置、写入
 * 或销毁；上层播放 owner 仍负责 SPKS/PCM/SPKE 的业务顺序。
 *
 * MIC 数字增益（CONFIG_JULIA_MIC_GAIN_PERCENT）在本层、I2S 原始采样转成 PCM16 时施加一次，
 * AFE 与 WSS 上行共用这份已经缩放的数据，上层不得再加一次增益。
 */

#include "board_audio.h"
#include "local_capture.h"

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TAG "board_audio"

#define MIC_BCLK GPIO_NUM_15
#define MIC_WS GPIO_NUM_2
#define MIC_DIN GPIO_NUM_39
#define SPK_BCLK GPIO_NUM_48
#define SPK_WS GPIO_NUM_38
#define SPK_DOUT GPIO_NUM_47
#define MIC_SAMPLES 320
/* 单次 board_audio_speaker_write() 接受的"单声道输入"字节上限：参数校验里
 * bytes > MAX_SPK_BYTES 即拒绝。它与 MIC 侧 320 样本（640 字节）的采集帧无关，
 * 也不是"多帧余量"，而是下面 spk_stereo 展开缓冲的容量来源。 */
#define MAX_SPK_BYTES 4096
/* 以下三个门限触发参数的单位与边界可确认，具体取值来源未确认：
 * 预录 25 帧＝500 ms，触发需连续 6 帧（120 ms）高于背景 +5.00 dB。 */
#define SLEEP_PREROLL_FRAMES 25
#define SLEEP_TRIGGER_FRAMES 6
#define SLEEP_THRESHOLD_DELTA_X100 500
static i2s_chan_handle_t mic_rx, spk_tx;
static bool spk_enabled, spk_retained;
static volatile bool playing;
/* 所有播放源使用相同的固定音量，运行时不接受调音。 */
static const uint32_t speaker_volume = CONFIG_JULIA_SPEAKER_VOLUME_PERCENT;
static int32_t mic_raw[MIC_SAMPLES];
static int16_t mic_pcm[MIC_SAMPLES];
static int16_t sleep_preroll[SLEEP_PREROLL_FRAMES][MIC_SAMPLES];
static volatile bool mic_sleeping;
static volatile bool mic_wake_triggered;
static volatile int16_t sleep_background_dbfs_x100 = -6000;
static size_t sleep_preroll_write;
static size_t sleep_preroll_count;
static uint32_t sleep_active_frames;
/* 立体声展开缓冲按"元素个数"计：MAX_SPK_BYTES 个 int16 = 8192 字节，正好容纳单次上限
 * 4096 单声道字节展开后的立体声样本（count = bytes / 2 个样本，每样本写左右各一份，
 * 共 2 * count 个 int16）。 */
static int16_t spk_stereo[MAX_SPK_BYTES];

/* Speaker 串行化：start/write/stop/self_test 整体持锁，
 * 防止 WSS 下行、本地 TTS、文件播放交叉配置/交叉写 PCM。 */
static SemaphoreHandle_t s_spk_lock;
static TaskHandle_t s_mic_task;
static atomic_bool s_mic_requested = true;
static atomic_bool s_mic_enabled;

/* 仅记录期望状态并唤醒 MIC task，实际 I2S 启停由该任务执行。
 * 调用者只有 julia_quiet_power 和语音路径；本地唤醒检测器必须与本开关成对切换
 * （见 julia_quiet_power 的 set_capture），否则会出现“不采音却仍在喂 AFE”的组合。 */
void board_audio_mic_set_enabled(bool enabled)
{
    if (atomic_exchange(&s_mic_requested, enabled) != enabled && s_mic_task != NULL)
        xTaskNotifyGive(s_mic_task);
}

bool board_audio_mic_is_enabled(void) { return atomic_load(&s_mic_enabled); }

/* 该缓冲由 MIC task 独占并逐帧复用，sink 必须在 callback 返回前复制。 */
static uint8_t s_pcm1_frame[16 + MIC_SAMPLES * 2];
static audio_pcm_sink_t s_afe_sink;
static void *s_afe_ctx;
static audio_frame_sink_t s_wss_sink;
static void *s_wss_ctx;
static volatile bool s_wss_mic_enabled;

static uint8_t sum8(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    while (n--) s += *p++;
    return (uint8_t)s;
}

static esp_err_t mic_init(void)
{
    /* 重试前清理部分初始化对象，避免失败重试占满 I2S 通道。 */
    if (mic_rx != NULL) {
        (void)i2s_channel_disable(mic_rx);
        ESP_RETURN_ON_ERROR(i2s_del_channel(mic_rx), TAG, "cleanup MIC channel");
        mic_rx = NULL;
    }
    i2s_chan_config_t c = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&c, NULL, &mic_rx), TAG, "new MIC channel");
    i2s_std_config_t s = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = MIC_BCLK, .ws = MIC_WS,
                     .dout = I2S_GPIO_UNUSED, .din = MIC_DIN},
    };
    s.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
    esp_err_t err = i2s_channel_init_std_mode(mic_rx, &s);
    if (err == ESP_OK) err = i2s_channel_enable(mic_rx);
    if (err != ESP_OK) {
        (void)i2s_channel_disable(mic_rx);
        if (i2s_del_channel(mic_rx) == ESP_OK) mic_rx = NULL;
    }
    return err;
}

static esp_err_t speaker_init(uint32_t rate, bool enable)
{
    if (spk_tx != NULL) {
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        if (spk_enabled) ESP_RETURN_ON_ERROR(i2s_channel_disable(spk_tx), TAG, "disable SPK");
        spk_enabled = false;
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(spk_tx, &clk), TAG, "reconfig SPK clock");
        const i2s_std_gpio_config_t gpio = {.mclk = I2S_GPIO_UNUSED, .bclk = SPK_BCLK,
            .ws = SPK_WS, .dout = SPK_DOUT, .din = I2S_GPIO_UNUSED};
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_gpio(spk_tx, &gpio), TAG, "restore SPK pins");
        if (enable) {
            ESP_RETURN_ON_ERROR(i2s_channel_enable(spk_tx), TAG, "enable SPK");
            spk_enabled = true;
        }
        return ESP_OK;
    }
    i2s_chan_config_t c = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    c.auto_clear = true;
    c.dma_desc_num = 4;
    c.dma_frame_num = 160;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&c, &spk_tx, NULL), TAG, "new SPK channel");
    i2s_std_config_t s = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = SPK_BCLK, .ws = SPK_WS,
                     .dout = SPK_DOUT, .din = I2S_GPIO_UNUSED},
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(spk_tx, &s), TAG, "init SPK std mode");
    if (enable) {
        ESP_RETURN_ON_ERROR(i2s_channel_enable(spk_tx), TAG, "enable SPK");
        spk_enabled = true;
    }
    return ESP_OK;
}

static int16_t scale_sample(int16_t sample)
{
    int32_t value = (int32_t)sample * (int32_t)speaker_volume / 100;
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (int16_t)value;
}

static int16_t mic_dbfs_x100(const int16_t *x, size_t count)
{
    return (int16_t)lrint(100 * lc_rms_dbfs(x, count));
}

/* PCM1 头字段和字节序属于设备/服务器协议；修改前必须同步更新 PROTOCOL.md。 */
static void send_pcm1(uint32_t seq, const int16_t *x, size_t count)
{
    uint16_t bytes = (uint16_t)(count * 2);
    int16_t db = mic_dbfs_x100(x, count);
    uint8_t h[16] = {'P', 'C', 'M', '1'};
    h[4] = (uint8_t)seq; h[5] = (uint8_t)(seq >> 8);
    h[6] = (uint8_t)(seq >> 16); h[7] = (uint8_t)(seq >> 24);
    h[8] = (uint8_t)bytes; h[9] = (uint8_t)(bytes >> 8);
    h[10] = (uint8_t)db; h[11] = (uint8_t)(((uint16_t)db) >> 8);
    h[15] = sum8((const uint8_t *)x, bytes);
    memcpy(s_pcm1_frame, h, sizeof(h));
    memcpy(s_pcm1_frame + sizeof(h), x, bytes);
    if (s_wss_sink != NULL) {
        s_wss_sink(s_pcm1_frame, sizeof(h) + bytes, s_wss_ctx);
    }
}

/* 门限触发模式：未确认起音前先把音频留在预录环里，命中门限后按原始采集顺序补发，
 * 避免丢掉触发前的声音；进入该模式时上层需给出背景电平。 */
static void mic_task(void *arg)
{
    uint32_t seq = 0;
    size_t filled = 0;
    while (1) {
        bool requested = atomic_load(&s_mic_requested);
        /* 只在期望状态与实际状态不一致时做一次 I2S 启停；power transition 失败时不更新
         * s_mic_enabled，下一轮重新尝试，避免把通道留在半初始化状态。 */
        if (requested != atomic_load(&s_mic_enabled)) {
            esp_err_t err;
            if (requested) {
                err = mic_init();
            } else {
                err = i2s_channel_disable(mic_rx);
                if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
                if (err == ESP_OK) err = i2s_del_channel(mic_rx);
                if (err == ESP_OK) mic_rx = NULL;
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "MIC power transition failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            atomic_store(&s_mic_enabled, requested);
            filled = 0;
            ESP_LOGI(TAG, "MIC capture %s", requested ? "on" : "off");
        }
        if (!requested) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        size_t got = 0;
        /* I2S 可能短读：把本次读到的样本累加到上轮余量后面，凑满 MIC_SAMPLES 才算一帧，
         * 否则会把不足 20 ms 的音频当成完整帧送出。 */
        esp_err_t read_err = i2s_channel_read(mic_rx, mic_raw + filled,
            sizeof(mic_raw) - filled * sizeof(*mic_raw), &got, 100);
        if (read_err != ESP_OK && read_err != ESP_ERR_TIMEOUT) {
            filled = 0;
            continue;
        }
        filled += got / sizeof(*mic_raw);
        if (filled < MIC_SAMPLES) continue;
        filled = 0;
        if (!atomic_load(&s_mic_requested)) continue;
        size_t count = MIC_SAMPLES;
        if (count > MIC_SAMPLES) count = MIC_SAMPLES;
        for (size_t i = 0; i < count; i++) {
            /* 增益在本层施加一次，实际运算是"先 64 位乘法、再一次性整除"：
             * ((int64_t)mic_raw[i] * CONFIG_JULIA_MIC_GAIN_PERCENT) / (100LL << 14)，
             * 即把"除以 2^14 缩回有效样本"与"乘增益百分比 / 100"合并成一次整数除法；
             * 除数 100LL << 14 = 100 × 16384，除法向零截断，之后才做 PCM16 饱和裁剪。
             * 必须保持现有运算顺序与舍入口径：若改成先右移缩回样本再乘增益，会先丢掉低位
             * 精度、并产生第二次截断，结果与现实现不一致（依赖该电平的阈值会随之失真）；
             * 32 位槽数据有效位深 14 这一换算依据在本仓库内来源未确认。
             * AFE 与 WSS 上行共用同一份结果，任何一侧再加一次增益都会使阈值失真。 */
            int32_t v = (int32_t)(((int64_t)mic_raw[i] *
                                   CONFIG_JULIA_MIC_GAIN_PERCENT) /
                                  (100LL << 14));
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            mic_pcm[i] = (int16_t)v;
        }
        /* AFE 不受 WSS 上传门控；播放期间采样可能包含回声，本层不做消除。 */
        if (s_afe_sink != NULL) {
            s_afe_sink(mic_pcm, count, s_afe_ctx);
        }
        /* WSS 门控由会话/唤醒策略决定；不能把 enabled 等同于 MIC_START。 */
        if (!s_wss_mic_enabled) {
            continue;
        }
        if (!mic_sleeping) {
            send_pcm1(seq++, mic_pcm, count);
            continue;
        }

        memcpy(sleep_preroll[sleep_preroll_write], mic_pcm, count * sizeof(int16_t));
        /* count 不足时补零，保证补发的是完整 320 样本帧。 */
        if (count < MIC_SAMPLES) {
            memset(&sleep_preroll[sleep_preroll_write][count], 0,
                   (MIC_SAMPLES - count) * sizeof(int16_t));
        }
        sleep_preroll_write = (sleep_preroll_write + 1) % SLEEP_PREROLL_FRAMES;
        if (sleep_preroll_count < SLEEP_PREROLL_FRAMES) sleep_preroll_count++;

        if (!mic_wake_triggered) {
            int16_t db = mic_dbfs_x100(mic_pcm, count);
            int16_t threshold = sleep_background_dbfs_x100 + SLEEP_THRESHOLD_DELTA_X100;
            if (db > threshold) {
                sleep_active_frames++;
            } else {
                sleep_active_frames = 0;
            }
            if (sleep_active_frames >= SLEEP_TRIGGER_FRAMES) {
                mic_wake_triggered = true;
                size_t start = (sleep_preroll_write + SLEEP_PREROLL_FRAMES -
                                sleep_preroll_count) % SLEEP_PREROLL_FRAMES;
                for (size_t i = 0; i < sleep_preroll_count; i++) {
                    send_pcm1(seq++, sleep_preroll[(start + i) % SLEEP_PREROLL_FRAMES],
                              MIC_SAMPLES);
                }
            }
        } else {
            send_pcm1(seq++, mic_pcm, count);
        }
    }
}

esp_err_t board_audio_init(void)
{
    /* 幂等：已创建 MIC task 就直接返回，不重复建通道或任务。 */
    if (s_mic_task != NULL) return ESP_OK;
    if (s_spk_lock == NULL) {
        s_spk_lock = xSemaphoreCreateMutex();
        if (s_spk_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = mic_init();
    if (err != ESP_OK) goto failed;
    atomic_store(&s_mic_enabled, true);
    /* 扬声器延迟到首次播放时初始化，避免启动阶段短暂开启 I2S：本函数成功返回时 SPK 通道
     * 仍未建立（spk_tx == NULL），SPK 引脚保持未配置，直到首次 board_audio_speaker_start()
     * 或 board_audio_speaker_retain(true)。 */
    if (xTaskCreatePinnedToCore(mic_task, "board_mic", 4096, NULL, 5, &s_mic_task, 1) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    ESP_LOGI(TAG, "ready mic=I2S0(15/2/39) gain=%d%% "
                  "spk=I2S1(48/38/47) volume=%u%%",
             CONFIG_JULIA_MIC_GAIN_PERCENT, (unsigned)speaker_volume);
    return ESP_OK;

failed:
    /* 失败即不保留半初始化的采集通道：先声明 MIC 不可用，再停播并释放 I2S RX；
     * s_spk_lock 与已注册的 sink 保留，调用方可读取该错误但不能当成就绪。 */
    atomic_store(&s_mic_enabled, false);
    (void)board_audio_speaker_stop();
    if (mic_rx != NULL) {
        (void)i2s_channel_disable(mic_rx);
        (void)i2s_del_channel(mic_rx);
        mic_rx = NULL;
    }
    ESP_LOGW(TAG, "audio init failed: %s", esp_err_to_name(err));
    return err;
}

/* sink 指针只在 mic_task 里读取：注册方必须在调用前完成上下文准备，注册后不得再改 ctx，
 * 也不得在 mic_task 运行期间注销（本层没有注销接口）。 */
esp_err_t board_audio_set_afe_sink(audio_pcm_sink_t sink, void *ctx)
{
    s_afe_sink = sink;
    s_afe_ctx = ctx;
    return ESP_OK;
}

esp_err_t board_audio_set_wss_sink(audio_frame_sink_t sink, void *ctx)
{
    s_wss_sink = sink;
    s_wss_ctx = ctx;
    return ESP_OK;
}

void board_audio_enable_wss_mic(bool enabled)
{
    s_wss_mic_enabled = enabled;
    if (!enabled) {
        /* 关闭上行：清触发/预录状态，防止重开后发送陈旧预录（等价 MICW 复位）。 */
        mic_wake_triggered = false;
        sleep_active_frames = 0;
        sleep_preroll_write = 0;
        sleep_preroll_count = 0;
    }
}

/* 进入门限触发模式。background_dbfs_x100 由上层给出（单位 0.01 dBFS，MICS 命令范围 −100～0 dBFS），
 * 阈值在 mic_task 内按背景 + SLEEP_THRESHOLD_DELTA_X100 计算。 */
void board_audio_mic_sleep(int16_t background_dbfs_x100)
{
    sleep_background_dbfs_x100 = background_dbfs_x100;
    mic_sleeping = true;
    mic_wake_triggered = false;
    sleep_active_frames = 0;
    sleep_preroll_write = 0;
    sleep_preroll_count = 0;
}

void board_audio_mic_wake(void)
{
    mic_sleeping = false;
    mic_wake_triggered = false;
    sleep_active_frames = 0;
}

static esp_err_t board_audio_speaker_stop_locked(void);

esp_err_t board_audio_speaker_start(uint32_t sample_rate)
{
    uint32_t rate = sample_rate ? sample_rate : 24000;
    esp_err_t err = ESP_OK;
    if (s_spk_lock != NULL && xSemaphoreTake(s_spk_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* 串行化：若已有其他源在播放，先停止旧源，新源接管（后开优先）。上层播放任务因此必须是
     * 唯一 I2S 写入者：把旧源停掉后仍继续写同一通道的调用方会让两路声音交错。 */
    if (playing) {
        (void)board_audio_speaker_stop_locked();
    }
    err = speaker_init(rate, true);
    if (err == ESP_OK) {
        playing = true;
    }
    if (s_spk_lock != NULL) xSemaphoreGive(s_spk_lock);
    return err;
}

static void speaker_clocks_low(void)
{
    const gpio_num_t pins[] = {SPK_BCLK, SPK_WS};
    for (unsigned i = 0; i < 2; ++i) {
        (void)gpio_reset_pin(pins[i]);
        (void)gpio_set_pull_mode(pins[i], GPIO_FLOATING);
        (void)gpio_set_level(pins[i], 0);
        (void)gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
    }
}

static esp_err_t board_audio_speaker_stop_locked(void)
{
    playing = false;
    /* 通道还没建起来时只把时钟脚拉低，避免扬声器悬空在未知电平。 */
    if (spk_tx == NULL) { speaker_clocks_low(); return ESP_OK; }
    esp_err_t err = spk_enabled ? i2s_channel_disable(spk_tx) : ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    spk_enabled = false;
    if (!spk_retained) {
        err = i2s_del_channel(spk_tx);
        if (err == ESP_OK) spk_tx = NULL;
    }
    if (err == ESP_OK) speaker_clocks_low();
    return err;
}

esp_err_t board_audio_speaker_retain(bool retain)
{
    if (!s_spk_lock || xSemaphoreTake(s_spk_lock, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    spk_retained = retain;
    esp_err_t err = ESP_OK;
    if (retain && !spk_tx) {
        err = speaker_init(24000, false);
        if (err == ESP_OK) speaker_clocks_low();
    } else if (!retain && !playing) err = board_audio_speaker_stop_locked();
    xSemaphoreGive(s_spk_lock);
    return err;
}

esp_err_t board_audio_speaker_write(const uint8_t *mono_pcm, size_t bytes)
{
    if (mono_pcm == NULL || bytes == 0 || bytes > MAX_SPK_BYTES || (bytes & 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    if (s_spk_lock != NULL && xSemaphoreTake(s_spk_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!playing) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        const int16_t *m = (const int16_t *)mono_pcm;
        size_t count = bytes / 2;
        for (size_t i = 0; i < count; i++) {
            int16_t value = scale_sample(m[i]);
            spk_stereo[2 * i] = value;
            spk_stereo[2 * i + 1] = value;
        }
        size_t w = 0;
        err = i2s_channel_write(spk_tx, spk_stereo, count * 4, &w, 50);
        if (err == ESP_OK && w != count * 4) err = ESP_ERR_TIMEOUT;
    }
    if (s_spk_lock != NULL) xSemaphoreGive(s_spk_lock);
    return err;
}

esp_err_t board_audio_speaker_stop(void)
{
    esp_err_t err = ESP_OK;
    if (s_spk_lock != NULL && xSemaphoreTake(s_spk_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    err = board_audio_speaker_stop_locked();
    if (s_spk_lock != NULL) xSemaphoreGive(s_spk_lock);
    return err;
}

bool board_audio_speaker_is_playing(void)
{
    return playing;
}

esp_err_t board_audio_speaker_self_test(void)
{
    const uint32_t rate = 24000;
    const int frequencies[3] = {440, 660, 880};
    esp_err_t err = ESP_OK;
    if (s_spk_lock != NULL && xSemaphoreTake(s_spk_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    err = speaker_init(rate, true);
    playing = true;
    for (int tone = 0; err == ESP_OK && tone < 3; tone++) {
        for (int block = 0; err == ESP_OK && block < 12; block++) {
            for (int i = 0; i < 512; i++) {
                double phase = 2.0 * M_PI * frequencies[tone] * (block * 512 + i) / rate;
                int16_t value = scale_sample((int16_t)(sin(phase) * 26000.0));
                spk_stereo[2 * i] = value;
                spk_stereo[2 * i + 1] = value;
            }
            size_t written = 0;
            if (i2s_channel_write(spk_tx, spk_stereo, 512 * 4, &written, 50) != ESP_OK || written != 512 * 4)
                err = ESP_FAIL;
        }
        int16_t silence[1024] = {0};
        size_t written = 0;
        if (i2s_channel_write(spk_tx, silence, sizeof(silence), &written, 50) != ESP_OK || written != sizeof(silence))
            err = ESP_FAIL;
    }
    if (err == ESP_OK) err = board_audio_speaker_stop_locked();
    else playing = false;
    if (s_spk_lock != NULL) xSemaphoreGive(s_spk_lock);
    return err;
}
