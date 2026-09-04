/**
 * @file julia_fsm_runtime.c
 * @brief 按顺序处理设备行为事件，并把每个状态转换成对应的屏幕和背光表现。
 *
 * 设备在开始交流、等待回答和播放回答时使用不同画面。唤醒后的准备阶段虽然
 * 与听音画面相同，但业务上仍表示“已经被唤醒、尚未确认用户开始说话”。
 */
#include "julia_fsm_runtime.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "julia_avatar.h"
#include "julia_backlight.h"
#include "lvgl_port.h"
#include "mqtt_comm.h"
#include "sdkconfig.h"
#include "voice_playback.h"
#include "wss_transport.h"

#define FSM_EVENT_QUEUE_DEPTH 16
#define FSM_TASK_STACK_SIZE   4096
#define FSM_TASK_PRIORITY     4
#define SERVICE_LINK_MQTT     (1U << 0)
#define SERVICE_LINK_WSS      (1U << 1)
#define SERVICE_LINK_ALL      (SERVICE_LINK_MQTT | SERVICE_LINK_WSS)
/* 断联提示只解释本轮交流中止原因，不应像严重故障一样等待复位或人工处理。 */
#define DISCONNECT_NOTICE_US  3000000ULL

typedef enum {
    FSM_RUNTIME_MESSAGE_EVENT = 0,
    FSM_RUNTIME_MESSAGE_FAULT,
} fsm_runtime_message_type_t;

typedef struct {
    fsm_runtime_message_type_t type;
    fsm_event_t event;
    julia_fault_reason_t fault_reason;
    esp_err_t error;
} fsm_runtime_message_t;

typedef enum {
    FSM_PRESENT_DEFAULT = 0,
    FSM_PRESENT_S1_COMPANION,
    FSM_PRESENT_S3_STANDBY,
    FSM_PRESENT_S5_SILENT,
    FSM_PRESENT_S6_SLEEP,
    FSM_PRESENT_S7_1_DISCONNECTED,
    FSM_PRESENT_S2_1_LISTENING,
    FSM_PRESENT_S2_2_THINKING,
    FSM_PRESENT_S2_3_SPEAKING,
} fsm_presentation_t;

static const char *TAG = "JULIA_FSM_RT";
static QueueHandle_t s_event_queue;
static TaskHandle_t s_task;
static julia_fsm_t s_fsm;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static julia_main_state_t s_committed_main_state = JULIA_MAIN_STATE_S0_BOOT;
static julia_s2_sub_state_t s_committed_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
static julia_s7_sub_state_t s_committed_s7_sub_state = JULIA_S7_SUB_STATE_NONE;
static julia_service_state_t s_committed_service_state = JULIA_SERVICE_CONNECTING;
static uint8_t s_online_links;
static esp_timer_handle_t s_standby_timer;
static esp_timer_handle_t s_silent_timer;
static esp_timer_handle_t s_disconnect_timer;
static esp_timer_handle_t s_service_init_timer;
static julia_fsm_state_observer_t s_state_observer;
static void *s_state_observer_ctx;

extern const uint8_t network_disconnected_wav_start[]
    asm("_binary_network_disconnected_16k_mono_16bit_wav_start");
extern const uint8_t network_disconnected_wav_end[]
    asm("_binary_network_disconnected_16k_mono_16bit_wav_end");

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void play_disconnect_prompt(void)
{
    const uint8_t *wav = network_disconnected_wav_start;
    size_t wav_bytes = (size_t)(network_disconnected_wav_end - wav);
    if (wav_bytes < 44 || memcmp(wav, "RIFF", 4) != 0 ||
        memcmp(wav + 8, "WAVE", 4) != 0 || memcmp(wav + 36, "data", 4) != 0 ||
        read_le16(wav + 20) != 1 || read_le16(wav + 22) != 1 ||
        read_le32(wav + 24) != 16000 || read_le16(wav + 34) != 16) {
        ESP_LOGW(TAG, "local disconnect prompt has an invalid WAV header");
        return;
    }
    size_t pcm_bytes = read_le32(wav + 40);
    if (pcm_bytes > wav_bytes - 44) pcm_bytes = wav_bytes - 44;
    pcm_bytes &= ~(size_t)1U;
    if (pcm_bytes == 0) {
        ESP_LOGW(TAG, "local disconnect prompt has no PCM payload");
        return;
    }
    uint32_t generation = 0;
    esp_err_t err = voice_playback_start_local(16000, wav + 44, pcm_bytes,
                                                &generation);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "local disconnect prompt start failed: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "local disconnect prompt generation=%lu bytes=%u",
                 (unsigned long)generation, (unsigned)pcm_bytes);
    }
}

static void standby_timer_callback(void *argument)
{
    (void)argument;
    esp_err_t err = julia_fsm_runtime_post(EVT_STANDBY_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "S3 驻留超时事件投递失败：%s", esp_err_to_name(err));
    }
}

static void silent_timer_callback(void *argument)
{
    (void)argument;
    esp_err_t err = julia_fsm_runtime_post(EVT_SILENT_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "S5 驻留超时事件投递失败：%s", esp_err_to_name(err));
    }
}

static void disconnect_timer_callback(void *argument)
{
    (void)argument;
    esp_err_t err = julia_fsm_runtime_post(EVT_DISCONNECT_NOTICE_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "断联提示结束事件投递失败：%s", esp_err_to_name(err));
    }
}

static void service_init_timer_callback(void *argument)
{
    (void)argument;
    esp_err_t err = julia_fsm_runtime_post(EVT_SERVICE_CONNECT_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "初始业务连接超时事件投递失败：%s", esp_err_to_name(err));
    }
}

static bool service_state_apply_event(fsm_event_t event)
{
    uint8_t bit = 0;
    bool connected = false;
    switch (event) {
    case EVT_MQTT_DISCONNECTED: bit = SERVICE_LINK_MQTT; break;
    case EVT_WSS_DISCONNECTED: bit = SERVICE_LINK_WSS; break;
    case EVT_MQTT_CONNECTED: bit = SERVICE_LINK_MQTT; connected = true; break;
    case EVT_WSS_CONNECTED: bit = SERVICE_LINK_WSS; connected = true; break;
    case EVT_SERVICE_CONNECT_TIMEOUT: break;
    default: return false;
    }

    portENTER_CRITICAL(&s_state_lock);
    julia_service_state_t previous_state = s_committed_service_state;
    uint8_t previous_links = s_online_links;
    if (event == EVT_SERVICE_CONNECT_TIMEOUT) {
        if (s_committed_service_state == JULIA_SERVICE_CONNECTING) {
            s_committed_service_state = JULIA_SERVICE_OFFLINE;
        }
    } else {
        if (connected) s_online_links |= bit;
        else s_online_links &= (uint8_t)~bit;
        if ((s_online_links & SERVICE_LINK_ALL) == SERVICE_LINK_ALL) {
            s_committed_service_state = JULIA_SERVICE_ONLINE;
        } else if (!connected || previous_state == JULIA_SERVICE_OFFLINE) {
            s_committed_service_state = JULIA_SERVICE_OFFLINE;
        } else {
            s_committed_service_state = JULIA_SERVICE_CONNECTING;
        }
    }
    bool changed = previous_state != s_committed_service_state ||
                   previous_links != s_online_links;
    julia_service_state_t state = s_committed_service_state;
    uint8_t online_links = s_online_links;
    portEXIT_CRITICAL(&s_state_lock);

    if (changed) {
        julia_avatar_set_offline(state == JULIA_SERVICE_OFFLINE);
        const char *name = state == JULIA_SERVICE_ONLINE ? "ONLINE" :
                           state == JULIA_SERVICE_OFFLINE ? "OFFLINE" : "CONNECTING";
        ESP_LOGI(TAG, "service=%s online_links=0x%02x by %s", name,
                 (unsigned)online_links, julia_fsm_event_name(event));
    }
    if (state == JULIA_SERVICE_ONLINE && s_service_init_timer != NULL) {
        (void)esp_timer_stop(s_service_init_timer);
    }
    return true;
}

static void service_state_reconcile(void)
{
    portENTER_CRITICAL(&s_state_lock);
    uint8_t online_links = s_online_links;
    julia_service_state_t state = s_committed_service_state;
    portEXIT_CRITICAL(&s_state_lock);
    bool mqtt_ready = mqtt_comm_is_ready();
    bool wss_ready = wss_transport_is_ready();
    if ((online_links & SERVICE_LINK_MQTT) == 0 && mqtt_ready) {
        (void)service_state_apply_event(EVT_MQTT_CONNECTED);
    }
    if ((online_links & SERVICE_LINK_WSS) == 0 && wss_ready) {
        (void)service_state_apply_event(EVT_WSS_CONNECTED);
    }
    if (state == JULIA_SERVICE_ONLINE) {
        if ((online_links & SERVICE_LINK_MQTT) != 0 && !mqtt_ready) {
            (void)julia_fsm_runtime_post(EVT_MQTT_DISCONNECTED);
        }
        if ((online_links & SERVICE_LINK_WSS) != 0 && !wss_ready) {
            (void)julia_fsm_runtime_post(EVT_WSS_DISCONNECTED);
        }
    }
}

static fsm_presentation_t presentation_for(julia_main_state_t main_state,
                                           julia_s2_sub_state_t s2_sub_state,
                                           julia_s7_sub_state_t s7_sub_state)
{
    if (main_state == JULIA_MAIN_STATE_S7_FAULT &&
        s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED) {
        /* S7.1 使用独立立绘和字幕，不能落入 S7.2 的严重故障呈现。 */
        return FSM_PRESENT_S7_1_DISCONNECTED;
    }
    if (main_state == JULIA_MAIN_STATE_S2_DIALOG) {
        switch (s2_sub_state) {
        case JULIA_S2_SUB_STATE_S2_1_LISTENING: return FSM_PRESENT_S2_1_LISTENING;
        case JULIA_S2_SUB_STATE_S2_2_THINKING: return FSM_PRESENT_S2_2_THINKING;
        case JULIA_S2_SUB_STATE_S2_3_SPEAKING: return FSM_PRESENT_S2_3_SPEAKING;
        case JULIA_S2_SUB_STATE_NONE:
        case JULIA_S2_SUB_STATE_COUNT:
        default: return FSM_PRESENT_DEFAULT;
        }
    }
    /* S4 保持独立状态身份，只复用 S2.1 对应的现有行为实现。 */
    if (main_state == JULIA_MAIN_STATE_S4_INTERACTION)
        return FSM_PRESENT_S2_1_LISTENING;
    if (main_state == JULIA_MAIN_STATE_S1_COMPANION) return FSM_PRESENT_S1_COMPANION;
    if (main_state == JULIA_MAIN_STATE_S3_STANDBY) return FSM_PRESENT_S3_STANDBY;
    if (main_state == JULIA_MAIN_STATE_S5_SILENT) return FSM_PRESENT_S5_SILENT;
    if (main_state == JULIA_MAIN_STATE_S6_SLEEP) return FSM_PRESENT_S6_SLEEP;
    /* 调试阶段 S0/S7.2/S8 共用 Companion 基础 UI，由状态叠字区分。 */
    return FSM_PRESENT_DEFAULT;
}

static const char *presentation_name(fsm_presentation_t presentation)
{
    switch (presentation) {
    case FSM_PRESENT_S1_COMPANION: return "S1_COMPANION";
    case FSM_PRESENT_S3_STANDBY: return "S3_STANDBY";
    case FSM_PRESENT_S5_SILENT: return "S5_SILENT";
    case FSM_PRESENT_S6_SLEEP: return "S6_SLEEP";
    case FSM_PRESENT_S7_1_DISCONNECTED: return "S7.1_DISCONNECTED";
    case FSM_PRESENT_S2_1_LISTENING: return "S2.1_LISTENING";
    case FSM_PRESENT_S2_2_THINKING: return "S2.2_THINKING";
    case FSM_PRESENT_S2_3_SPEAKING: return "S2.3_SPEAKING";
    case FSM_PRESENT_DEFAULT:
    default: return "DEFAULT";
    }
}

static const char *state_status_text(julia_main_state_t main_state,
                                     julia_s2_sub_state_t s2_sub_state,
                                     julia_s7_sub_state_t s7_sub_state)
{
    if (main_state == JULIA_MAIN_STATE_S7_FAULT &&
        s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED) {
        return "S7.1 DISCONNECTED";
    }
    if (main_state == JULIA_MAIN_STATE_S7_FAULT &&
        s7_sub_state == JULIA_S7_SUB_STATE_S7_2_FAULT) {
        return "S7.2 FAULT";
    }
    if (main_state == JULIA_MAIN_STATE_S2_DIALOG) {
        switch (s2_sub_state) {
        case JULIA_S2_SUB_STATE_S2_1_LISTENING: return "S2.1 LISTEN";
        case JULIA_S2_SUB_STATE_S2_2_THINKING: return "S2.2 THINK";
        case JULIA_S2_SUB_STATE_S2_3_SPEAKING: return "S2.3 SPEAK";
        case JULIA_S2_SUB_STATE_NONE:
        case JULIA_S2_SUB_STATE_COUNT:
        default: return "S2 DIALOG";
        }
    }
    switch (main_state) {
    case JULIA_MAIN_STATE_S0_BOOT: return "S0 BOOT";
    case JULIA_MAIN_STATE_S1_COMPANION: return "S1 COMPANION";
    case JULIA_MAIN_STATE_S3_STANDBY: return "S3 STANDBY";
    case JULIA_MAIN_STATE_S4_INTERACTION: return "S4 INTERACTION";
    case JULIA_MAIN_STATE_S5_SILENT: return "S5 SILENT";
    case JULIA_MAIN_STATE_S6_SLEEP: return "S6 SLEEP";
    case JULIA_MAIN_STATE_S7_FAULT: return "S7 UNKNOWN";
    case JULIA_MAIN_STATE_S8_OTA: return "S8 OTA";
    case JULIA_MAIN_STATE_S2_DIALOG: return "S2 DIALOG";
    case JULIA_MAIN_STATE_COUNT:
    default: return "STATE UNKNOWN";
    }
}

static void apply_presentation(julia_main_state_t main_state,
                               julia_s2_sub_state_t s2_sub_state,
                               julia_s7_sub_state_t s7_sub_state)
{
    fsm_presentation_t presentation = presentation_for(main_state, s2_sub_state,
                                                        s7_sub_state);
    if (presentation != FSM_PRESENT_S6_SLEEP) {
        esp_err_t display_err = lvgl_port_set_display_off(false);
        if (display_err != ESP_OK) {
            ESP_LOGW(TAG, "display wake failed: %s", esp_err_to_name(display_err));
        }
    }
    switch (presentation) {
    case FSM_PRESENT_S1_COMPANION:
        julia_backlight_breathe_stop();
        julia_backlight_set(CONFIG_JULIA_COMPANION_BRIGHTNESS_PERCENT);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(false);
        break;
    case FSM_PRESENT_S3_STANDBY: {
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(true);
        esp_err_t err = julia_backlight_breathe_start(
            CONFIG_JULIA_DISPLAY_BREATHE_MIN_PERCENT,
            CONFIG_JULIA_DISPLAY_BREATHE_MAX_PERCENT,
            CONFIG_JULIA_DISPLAY_BREATHE_PERIOD_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "standby breathing start failed: %s",
                     esp_err_to_name(err));
        }
        break;
    }
    case FSM_PRESENT_S6_SLEEP:
        julia_backlight_breathe_stop();
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(true);
        julia_backlight_set(0);
        {
            esp_err_t display_err = lvgl_port_set_display_off(true);
            if (display_err != ESP_OK) {
                ESP_LOGW(TAG, "display sleep failed: %s", esp_err_to_name(display_err));
            }
        }
        break;
    case FSM_PRESENT_S7_1_DISCONNECTED:
        /* S7.1 只承担短暂提示；长期断联由独立 offline 标签表达。 */
        julia_backlight_breathe_stop();
        julia_backlight_set(CONFIG_JULIA_COMPANION_BRIGHTNESS_PERCENT);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(false);
        break;
    case FSM_PRESENT_S5_SILENT:
        /* S5 继续复用 Companion 基础立绘，但用固定低亮度明确区分静默状态。 */
        julia_backlight_breathe_stop();
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(false);
        julia_backlight_set(CONFIG_JULIA_SILENT_BRIGHTNESS_PERCENT);
        break;
    case FSM_PRESENT_S2_1_LISTENING:
        /* 复用现有“听”呈现：闭眼立绘、停止嘴型播放并保持屏幕唤醒。 */
        julia_backlight_breathe_stop();
        julia_backlight_set(100);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_LISTENING);
        julia_avatar_set_dozing(false);
        break;
    case FSM_PRESENT_S2_2_THINKING:
        /* 复用现有“想”呈现；推理与传输仍由原语音服务负责。 */
        julia_backlight_breathe_stop();
        julia_backlight_set(100);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_THINKING);
        julia_avatar_set_dozing(false);
        break;
    case FSM_PRESENT_S2_3_SPEAKING:
        /* 复用现有“说”呈现；音频播放与 PCM 嘴型仍由原播放链路负责。 */
        julia_backlight_breathe_stop();
        julia_backlight_set(100);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_SPEAKING);
        julia_avatar_set_dozing(false);
        break;
    case FSM_PRESENT_DEFAULT:
    default:
        julia_backlight_breathe_stop();
        julia_backlight_set(100);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(false);
        break;
    }

    ESP_LOGI(TAG, "state=%s/%s/%s presentation=%s",
             julia_fsm_main_state_name(main_state),
             julia_fsm_s2_sub_state_name(s2_sub_state),
             julia_fsm_s7_sub_state_name(s7_sub_state),
             presentation_name(presentation));
}

static void runtime_on_enter(julia_fsm_t *fsm, julia_main_state_t main_state,
                             julia_s2_sub_state_t s2_sub_state, fsm_event_t event)
{
    (void)fsm;
    portENTER_CRITICAL(&s_state_lock);
    s_committed_main_state = main_state;
    s_committed_s2_sub_state = s2_sub_state;
    s_committed_s7_sub_state = fsm->s7_sub_state;
    portEXIT_CRITICAL(&s_state_lock);
    /* 先让新状态正式生效，再通知语音服务回报服务器，避免服务器过早发送回答。 */
    if (s_state_observer != NULL) {
        s_state_observer(main_state, s2_sub_state, event, s_state_observer_ctx);
    }
    ESP_LOGI(TAG, "enter %s/%s/%s by %s", julia_fsm_main_state_name(main_state),
             julia_fsm_s2_sub_state_name(s2_sub_state),
             julia_fsm_s7_sub_state_name(fsm->s7_sub_state),
             julia_fsm_event_name(event));
    julia_avatar_set_status_text(
        state_status_text(main_state, s2_sub_state, fsm->s7_sub_state));
    apply_presentation(main_state, s2_sub_state, fsm->s7_sub_state);
    if (main_state == JULIA_MAIN_STATE_S3_STANDBY && s_standby_timer != NULL) {
        esp_err_t err = esp_timer_start_once(
            s_standby_timer,
            (uint64_t)CONFIG_JULIA_STANDBY_SLEEP_TIMEOUT_SECONDS * 1000000ULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "S3 驻留计时启动失败：%s", esp_err_to_name(err));
        }
    }
    if (main_state == JULIA_MAIN_STATE_S5_SILENT && s_silent_timer != NULL) {
        esp_err_t err = esp_timer_start_once(
            s_silent_timer,
            (uint64_t)CONFIG_JULIA_SILENT_STANDBY_TIMEOUT_SECONDS * 1000000ULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "S5 驻留计时启动失败：%s", esp_err_to_name(err));
        }
    }
    if (main_state == JULIA_MAIN_STATE_S7_FAULT &&
        fsm->s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED) {
        play_disconnect_prompt();
        /* 计时器失败时立即执行返回策略，不能让提示态永久占用行为状态机。 */
        esp_err_t err = s_disconnect_timer != NULL
                            ? esp_timer_start_once(s_disconnect_timer,
                                                   DISCONNECT_NOTICE_US)
                            : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "断联提示计时启动失败，立即返回稳定状态：%s",
                     esp_err_to_name(err));
            (void)julia_fsm_runtime_post(EVT_DISCONNECT_NOTICE_TIMEOUT);
        }
    }
}

static void runtime_on_exit(julia_fsm_t *fsm, julia_main_state_t main_state,
                            julia_s2_sub_state_t s2_sub_state, fsm_event_t event)
{
    (void)fsm;
    (void)s2_sub_state;
    (void)event;
    if (main_state == JULIA_MAIN_STATE_S3_STANDBY && s_standby_timer != NULL) {
        (void)esp_timer_stop(s_standby_timer);
    }
    if (main_state == JULIA_MAIN_STATE_S5_SILENT && s_silent_timer != NULL) {
        (void)esp_timer_stop(s_silent_timer);
    }
    if (main_state == JULIA_MAIN_STATE_S7_FAULT &&
        fsm->s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED &&
        s_disconnect_timer != NULL) {
        (void)esp_timer_stop(s_disconnect_timer);
    }
}

static void fsm_task(void *argument)
{
    (void)argument;
    fsm_runtime_message_t message;
    for (;;) {
        if (xQueueReceive(s_event_queue, &message, pdMS_TO_TICKS(1000)) != pdTRUE) {
            service_state_reconcile();
            continue;
        }
        if (message.type == FSM_RUNTIME_MESSAGE_FAULT) {
            julia_main_state_t previous_main = s_fsm.main_state;
            julia_s2_sub_state_t previous_sub = s_fsm.s2_sub_state;
            esp_err_t record_err = julia_fault_record(message.fault_reason, message.error,
                                                      previous_main, previous_sub);
            ESP_LOGE(TAG, "严重故障：reason=%s err=%s，进入 S7.2",
                     julia_fault_reason_name(message.fault_reason),
                     esp_err_to_name(message.error));
            if (s_fsm.main_state != JULIA_MAIN_STATE_S7_FAULT ||
                s_fsm.s7_sub_state != JULIA_S7_SUB_STATE_S7_2_FAULT) {
                (void)julia_fsm_transition_to(&s_fsm, JULIA_MAIN_STATE_S7_FAULT,
                                              JULIA_S2_SUB_STATE_NONE, EVT_NONE);
            }
            if (record_err == ESP_OK && !julia_fault_reset_allowed()) {
                ESP_LOGE(TAG, "同类故障连续超过自动复位上限，保持 S7.2 等待售后处理");
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_JULIA_FAULT_RESET_DELAY_MS));
            esp_restart();
            continue;
        }
        if (!julia_fsm_state_is_valid_full(s_fsm.main_state, s_fsm.s2_sub_state,
                                           s_fsm.s7_sub_state)) {
            (void)julia_fault_record(JULIA_FAULT_FSM_STATE_CORRUPT,
                                     ESP_ERR_INVALID_STATE,
                                     s_committed_main_state,
                                     s_committed_s2_sub_state);
            ESP_LOGE(TAG, "FSM 状态非法，立即复位");
            esp_restart();
            continue;
        }
        julia_service_state_t previous_service_state =
            julia_fsm_runtime_get_service_state();
        bool service_event = service_state_apply_event(message.event);
        if (service_event) {
            julia_service_state_t current_service_state =
                julia_fsm_runtime_get_service_state();
            if (current_service_state != JULIA_SERVICE_OFFLINE ||
                previous_service_state == JULIA_SERVICE_OFFLINE) {
                /* 恢复事件只维护标签；同一离线周期内的后续断联也不重复进入 S7.1。 */
                continue;
            }
        }
        if (!julia_fsm_handle_event(&s_fsm, message.event, NULL)) {
            if (message.event == EVT_WAKEUP ||
                message.event == EVT_INTENT_GOODNIGHT ||
                message.event == EVT_INTENT_DISMISS) {
                ESP_LOGW(TAG, "关键交互事件被忽略：event=%s state=%s/%s",
                         julia_fsm_event_name(message.event),
                         julia_fsm_main_state_name(s_fsm.main_state),
                         julia_fsm_s2_sub_state_name(s_fsm.s2_sub_state));
            } else {
                ESP_LOGD(TAG, "ignored event=%s state=%s/%s",
                         julia_fsm_event_name(message.event),
                         julia_fsm_main_state_name(s_fsm.main_state),
                         julia_fsm_s2_sub_state_name(s_fsm.s2_sub_state));
            }
        }
    }
    vTaskDelete(NULL);
}

esp_err_t julia_fsm_runtime_init(bool boot_dependencies_ready)
{
    if (s_task != NULL) return ESP_OK;
    s_event_queue = xQueueCreate(FSM_EVENT_QUEUE_DEPTH, sizeof(fsm_runtime_message_t));
    if (s_event_queue == NULL) return ESP_ERR_NO_MEM;

    julia_fsm_init(&s_fsm);
    s_fsm.on_enter = runtime_on_enter;
    s_fsm.on_exit = runtime_on_exit;
    if (s_standby_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = standby_timer_callback,
            .name = "s3_sleep",
        };
        esp_err_t timer_err = esp_timer_create(&timer_args, &s_standby_timer);
        if (timer_err != ESP_OK) {
            ESP_LOGW(TAG, "S3 驻留计时器创建失败：%s", esp_err_to_name(timer_err));
        }
    }
    if (s_silent_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = silent_timer_callback,
            .name = "s5_standby",
        };
        esp_err_t timer_err = esp_timer_create(&timer_args, &s_silent_timer);
        if (timer_err != ESP_OK) {
            ESP_LOGW(TAG, "S5 驻留计时器创建失败：%s", esp_err_to_name(timer_err));
        }
    }
    if (s_disconnect_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = disconnect_timer_callback,
            .name = "s7_1_standby",
        };
        esp_err_t timer_err = esp_timer_create(&timer_args, &s_disconnect_timer);
        if (timer_err != ESP_OK) {
            ESP_LOGW(TAG, "断联提示计时器创建失败：%s", esp_err_to_name(timer_err));
        }
    }
    if (s_service_init_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = service_init_timer_callback,
            .name = "service_init",
        };
        esp_err_t timer_err = esp_timer_create(&timer_args, &s_service_init_timer);
        if (timer_err != ESP_OK) {
            ESP_LOGW(TAG, "初始业务连接计时器创建失败：%s", esp_err_to_name(timer_err));
        }
    }
    portENTER_CRITICAL(&s_state_lock);
    s_committed_main_state = s_fsm.main_state;
    s_committed_s2_sub_state = s_fsm.s2_sub_state;
    s_committed_s7_sub_state = s_fsm.s7_sub_state;
    s_online_links = 0;
    s_committed_service_state = JULIA_SERVICE_CONNECTING;
    portEXIT_CRITICAL(&s_state_lock);
    julia_avatar_set_offline(false);
    if (boot_dependencies_ready) {
        /* 初始化完成后进入待唤醒的 S3；S1 只保留会话后的免唤醒陪伴语义。 */
        if (!julia_fsm_transition_to(&s_fsm, JULIA_MAIN_STATE_S3_STANDBY,
                                     JULIA_S2_SUB_STATE_NONE, EVT_NONE)) {
            vQueueDelete(s_event_queue);
            s_event_queue = NULL;
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        apply_presentation(s_fsm.main_state, s_fsm.s2_sub_state,
                           s_fsm.s7_sub_state);
    }

    if (xTaskCreate(fsm_task, "julia_fsm", FSM_TASK_STACK_SIZE, NULL,
                    FSM_TASK_PRIORITY, &s_task) != pdPASS) {
        if (s_standby_timer != NULL) {
            (void)esp_timer_delete(s_standby_timer);
            s_standby_timer = NULL;
        }
        if (s_silent_timer != NULL) {
            (void)esp_timer_delete(s_silent_timer);
            s_silent_timer = NULL;
        }
        if (s_disconnect_timer != NULL) {
            (void)esp_timer_delete(s_disconnect_timer);
            s_disconnect_timer = NULL;
        }
        if (s_service_init_timer != NULL) {
            (void)esp_timer_delete(s_service_init_timer);
            s_service_init_timer = NULL;
        }
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (s_service_init_timer == NULL ||
        esp_timer_start_once(
            s_service_init_timer,
            (uint64_t)CONFIG_JULIA_SERVICE_INIT_TIMEOUT_SECONDS * 1000000ULL) != ESP_OK) {
        ESP_LOGW(TAG, "初始业务连接计时器不可用，立即按离线处理");
        (void)julia_fsm_runtime_post(EVT_SERVICE_CONNECT_TIMEOUT);
    }
    ESP_LOGI(TAG, "ready initial=%s/%s/%s queue=%u s1_bl=%d%% "
                  "s3_breathe=%d-%d%% s3_sleep=%ds s5_standby=%ds "
                  "s6_bl=0%% fault_reset=%dms quick_fault=%ds/%d",
             julia_fsm_main_state_name(s_fsm.main_state),
             julia_fsm_s2_sub_state_name(s_fsm.s2_sub_state),
             julia_fsm_s7_sub_state_name(s_fsm.s7_sub_state),
             (unsigned)FSM_EVENT_QUEUE_DEPTH,
             CONFIG_JULIA_COMPANION_BRIGHTNESS_PERCENT,
             CONFIG_JULIA_DISPLAY_BREATHE_MIN_PERCENT,
             CONFIG_JULIA_DISPLAY_BREATHE_MAX_PERCENT,
             CONFIG_JULIA_STANDBY_SLEEP_TIMEOUT_SECONDS,
             CONFIG_JULIA_SILENT_STANDBY_TIMEOUT_SECONDS,
             CONFIG_JULIA_FAULT_RESET_DELAY_MS,
             CONFIG_JULIA_FAULT_QUICK_UPTIME_SECONDS,
             CONFIG_JULIA_FAULT_AUTO_RESET_LIMIT);
    return ESP_OK;
}

void julia_fsm_runtime_set_state_observer(julia_fsm_state_observer_t observer,
                                          void *ctx)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state_observer = observer;
    s_state_observer_ctx = ctx;
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t julia_fsm_runtime_post(fsm_event_t event)
{
    if (event <= EVT_NONE || event >= EVT_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_event_queue == NULL) return ESP_ERR_INVALID_STATE;
    fsm_runtime_message_t message = {
        .type = FSM_RUNTIME_MESSAGE_EVENT,
        .event = event,
    };
    return xQueueSend(s_event_queue, &message, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t julia_fsm_runtime_raise_fault(julia_fault_reason_t reason, esp_err_t error)
{
    if (reason <= JULIA_FAULT_NONE || reason > JULIA_FAULT_CORE_TASK_STALLED)
        return ESP_ERR_INVALID_ARG;
    if (s_event_queue == NULL) return ESP_ERR_INVALID_STATE;
    fsm_runtime_message_t message = {
        .type = FSM_RUNTIME_MESSAGE_FAULT,
        .fault_reason = reason,
        .error = error,
    };
    return xQueueSendToFront(s_event_queue, &message, 0) == pdTRUE
               ? ESP_OK : ESP_ERR_NO_MEM;
}

julia_main_state_t julia_fsm_runtime_get_state(void)
{
    julia_main_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_committed_main_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

julia_s2_sub_state_t julia_fsm_runtime_get_s2_sub_state(void)
{
    julia_s2_sub_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_committed_s2_sub_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

julia_s7_sub_state_t julia_fsm_runtime_get_s7_sub_state(void)
{
    julia_s7_sub_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_committed_s7_sub_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

julia_service_state_t julia_fsm_runtime_get_service_state(void)
{
    julia_service_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_committed_service_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}
