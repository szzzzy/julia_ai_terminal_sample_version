/**
 * @file julia_fsm_runtime.c
 * @brief 串行提交行为状态、云端可用性和对应的用户呈现。
 *
 * FSM Task 是行为状态和聚合服务状态的唯一写入者；MQTT/WSS 与 esp_timer 回调
 * 只能投递事件。服务状态与 S0～S8 正交，主状态切换不得清除 offline 标签。
 * 连接事件可能因队列拥塞丢失，因此 Task 空闲时还会核对 transport 就绪快照。
 */
#include "julia_fsm_runtime.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "julia_avatar.h"
#include "julia_backlight.h"
#include "julia_battery.h"
#include "lvgl_port.h"
#include "mqtt_comm.h"
#include "sdkconfig.h"
#include "voice_playback.h"
#include "voice_state_sync.h"
#include "wss_transport.h"

#define FSM_EVENT_QUEUE_DEPTH 16
#define FSM_TASK_STACK_SIZE   4096
#define FSM_TASK_PRIORITY     4
#define SERVICE_LINK_MQTT     (1U << 0)
#define SERVICE_LINK_WSS      (1U << 1)
#define SERVICE_LINK_ALL      (SERVICE_LINK_MQTT | SERVICE_LINK_WSS)
/* 内嵌提示约 2.16 s；3 s 窗口为尾音排空留出余量。它不是网络恢复期限。 */
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
    SemaphoreHandle_t completed;
    bool *applied;
    bool check_revision;
    uint32_t expected_revision;
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
/* s_fsm 仅由 FSM Task 修改；下列快照与链路位图供其它任务无阻塞查询。 */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static julia_main_state_t s_committed_main_state = JULIA_MAIN_STATE_S0_BOOT;
static julia_s2_sub_state_t s_committed_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
static julia_s7_sub_state_t s_committed_s7_sub_state = JULIA_S7_SUB_STATE_NONE;
static julia_service_state_t s_committed_service_state = JULIA_SERVICE_CONNECTING;
static uint32_t s_committed_revision;
static int64_t s_committed_enter_us;
static fsm_event_t s_committed_reason;
static uint8_t s_online_links;
static esp_timer_handle_t s_standby_timer;
static esp_timer_handle_t s_silent_timer;
static esp_timer_handle_t s_disconnect_timer;
static esp_timer_handle_t s_service_init_timer;
/* owner 持有截止时间；timer 事件只是及时唤醒，满队列不能丢掉状态退出条件。 */
static int64_t s_standby_deadline_us;
static int64_t s_companion_deadline_us;
static int64_t s_silent_deadline_us;
static int64_t s_disconnect_deadline_us;
static int64_t s_service_deadline_us;
static julia_fsm_state_observer_t s_state_observer;
static void *s_state_observer_ctx;
/* 提醒只由 FSM Task 调度，持续低电量期间不反复播放。 */
static bool s_low_battery_notified;
static uint32_t s_local_prompt_generation;
static int64_t s_ota_prompt_deadline_us;

/* EMBED_FILES 生成的符号覆盖整个应用生命周期，满足本地播放“不复制源 PCM”的契约。 */
extern const uint8_t network_disconnected_wav_start[]
    asm("_binary_network_disconnected_16k_mono_16bit_wav_start");
extern const uint8_t network_disconnected_wav_end[]
    asm("_binary_network_disconnected_16k_mono_16bit_wav_end");
extern const uint8_t low_battery_wav_start[]
    asm("_binary_low_battery_16k_mono_16bit_wav_start");
extern const uint8_t low_battery_wav_end[]
    asm("_binary_low_battery_16k_mono_16bit_wav_end");
extern const uint8_t upgrade_start_wav_start[]
    asm("_binary_upgrade_start_16k_mono_16bit_wav_start");
extern const uint8_t upgrade_start_wav_end[]
    asm("_binary_upgrade_start_16k_mono_16bit_wav_end");
extern const uint8_t upgrade_success_wav_start[]
    asm("_binary_upgrade_success_16k_mono_16bit_wav_start");
extern const uint8_t upgrade_success_wav_end[]
    asm("_binary_upgrade_success_16k_mono_16bit_wav_end");
extern const uint8_t upgrade_failed_wav_start[]
    asm("_binary_upgrade_failed_16k_mono_16bit_wav_start");
extern const uint8_t upgrade_failed_wav_end[]
    asm("_binary_upgrade_failed_16k_mono_16bit_wav_end");
extern const uint8_t restart_after_issue_wav_start[]
    asm("_binary_restart_after_issue_16k_mono_16bit_wav_start");
extern const uint8_t restart_after_issue_wav_end[]
    asm("_binary_restart_after_issue_16k_mono_16bit_wav_end");
static bool play_local_prompt(const uint8_t *wav, size_t wav_bytes, const char *name,
                                bool only_if_idle, uint32_t *generation)
{
    esp_err_t err = voice_playback_start_local_wav(wav, wav_bytes, only_if_idle,
                                                   generation);
    if (err != ESP_OK) {
        if (!only_if_idle || err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "local %s prompt start failed: %s", name,
                     esp_err_to_name(err));
        }
        return false;
    } else {
        ESP_LOGI(TAG, "local %s prompt generation=%lu wav_bytes=%u", name,
                 (unsigned long)*generation, (unsigned)wav_bytes);
    }
    return true;
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

/**
 * 只由 FSM Task 调用。ONLINE 要求 MQTT 关键订阅与 WSS 认证会话同时就绪；
 * OFFLINE 在两路全部恢复前保持锁存，避免连接抖动反复触发 S7.1 和本地语音。
 */
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
        } else if (previous_state != JULIA_SERVICE_CONNECTING) {
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
    if (state == JULIA_SERVICE_ONLINE) s_service_deadline_us = 0;
    return true;
}

/**
 * 连接回调是快速、非阻塞的事件生产者，队列满时允许投递失败。周期快照保证一次
 * 丢失的恢复事件不会让 offline 永久残留，也保证丢失的断开事件最终能够补发。
 */
static bool runtime_process_event(fsm_event_t event);

static void service_state_reconcile(void)
{
    portENTER_CRITICAL(&s_state_lock);
    uint8_t online_links = s_online_links;
    portEXIT_CRITICAL(&s_state_lock);
    bool mqtt_ready = mqtt_comm_is_ready();
    bool wss_ready = wss_transport_is_ready() && voice_state_sync_is_ready();
    /* Clear stale links before adding recovered links, including while CONNECTING
     * or OFFLINE. Otherwise interleaved reconnects can falsely report ONLINE.
     * We already own the FSM here: do not enqueue into a possibly full queue. */
    if ((online_links & SERVICE_LINK_MQTT) != 0 && !mqtt_ready) {
        (void)runtime_process_event(EVT_MQTT_DISCONNECTED);
    }
    if ((online_links & SERVICE_LINK_WSS) != 0 && !wss_ready) {
        (void)runtime_process_event(EVT_WSS_DISCONNECTED);
    }
    if ((online_links & SERVICE_LINK_MQTT) == 0 && mqtt_ready) {
        (void)service_state_apply_event(EVT_MQTT_CONNECTED);
    }
    if ((online_links & SERVICE_LINK_WSS) == 0 && wss_ready) {
        (void)service_state_apply_event(EVT_WSS_CONNECTED);
    }
}

static fsm_presentation_t presentation_for(julia_main_state_t main_state,
                                           julia_s2_sub_state_t s2_sub_state,
                                           julia_s7_sub_state_t s7_sub_state)
{
    if (main_state == JULIA_MAIN_STATE_S7_FAULT &&
        s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED) {
        /* 可恢复断联与需要复位的 S7.2 必须保持不同的用户呈现。 */
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

static void play_ota_prompt(const uint8_t *start, const uint8_t *end, const char *name)
{
    /* Match the S4 farewell/goodnight portrait, without changing S8 identity. */
    julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_LISTENING);
    julia_avatar_set_dozing(false);
    julia_avatar_talking_start();
    if (!play_local_prompt(start, (size_t)(end - start), name, false,
                           &s_local_prompt_generation)) {
        s_local_prompt_generation = 0;
        julia_avatar_talking_stop();
    }
    /* PCM16 mono 16 kHz duration plus two seconds for output drain. */
    s_ota_prompt_deadline_us = esp_timer_get_time() +
        (int64_t)(end - start) * 1000000LL / 32000 + 2000000LL;
}

static void local_prompt_poll(void)
{
    /* 本地提示收尾不依赖电量采样是否可用，也不消费 WSS 的完成通知。 */
    if (s_local_prompt_generation != 0) {
        if (voice_playback_generation_is_active(s_local_prompt_generation)) {
            if (s_ota_prompt_deadline_us == 0 ||
                esp_timer_get_time() < s_ota_prompt_deadline_us) return;
            ESP_LOGW(TAG, "OTA prompt timed out; releasing terminal transition");
            (void)voice_playback_stop_generation(s_local_prompt_generation);
        }
        s_ota_prompt_deadline_us = 0;
        s_local_prompt_generation = 0;
        if (!voice_playback_is_active()) {
            julia_avatar_talking_stop();
            apply_presentation(s_fsm.main_state, s_fsm.s2_sub_state, s_fsm.s7_sub_state);
        }
        return;
    }

    julia_battery_status_t battery;
    if (julia_battery_get_status(&battery) != ESP_OK || !battery.valid) return;
    bool low = battery.present && battery.state == JULIA_BATTERY_STATE_LOW;
    if (!low) s_low_battery_notified = false;

    /* 对话、故障、升级和睡眠期间保留提醒，回到可见的空闲状态再播。 */
    bool idle = s_fsm.main_state == JULIA_MAIN_STATE_S1_COMPANION ||
                s_fsm.main_state == JULIA_MAIN_STATE_S3_STANDBY ||
                s_fsm.main_state == JULIA_MAIN_STATE_S5_SILENT;
    if (!low || s_low_battery_notified || !idle || voice_playback_is_active()) return;

    /* S3 的睡眠立绘隐藏嘴层；播报期间暂时恢复人物，完成后恢复当前状态呈现。 */
    julia_avatar_set_dozing(false);
    julia_avatar_talking_start();
    if (play_local_prompt(low_battery_wav_start,
            (size_t)(low_battery_wav_end - low_battery_wav_start), "low battery",
            true, &s_local_prompt_generation)) {
        s_low_battery_notified = true;
    } else {
        julia_avatar_talking_stop();
        apply_presentation(s_fsm.main_state, s_fsm.s2_sub_state, s_fsm.s7_sub_state);
    }
}

static void runtime_on_enter(julia_fsm_t *fsm, julia_main_state_t main_state,
                             julia_s2_sub_state_t s2_sub_state, fsm_event_t event)
{
    (void)fsm;
    portENTER_CRITICAL(&s_state_lock);
    s_committed_main_state = main_state;
    s_committed_s2_sub_state = s2_sub_state;
    s_committed_s7_sub_state = fsm->s7_sub_state;
    if (++s_committed_revision == 0) ++s_committed_revision;
    s_committed_enter_us = esp_timer_get_time();
    s_committed_reason = event;
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
    if (main_state == JULIA_MAIN_STATE_S8_OTA) {
        play_ota_prompt(upgrade_start_wav_start, upgrade_start_wav_end, "OTA start");
    }
    if (main_state == JULIA_MAIN_STATE_S1_COMPANION) {
        s_companion_deadline_us = s_committed_enter_us +
            (int64_t)CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS * 1000000LL;
    }
    if (main_state == JULIA_MAIN_STATE_S3_STANDBY) {
        s_standby_deadline_us = esp_timer_get_time() +
            (int64_t)CONFIG_JULIA_STANDBY_SLEEP_TIMEOUT_SECONDS * 1000000LL;
    }
    if (main_state == JULIA_MAIN_STATE_S5_SILENT) {
        s_silent_deadline_us = esp_timer_get_time() +
            (int64_t)CONFIG_JULIA_SILENT_STANDBY_TIMEOUT_SECONDS * 1000000LL;
    }
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
        /* talking 必须先于首块 PCM 生效，嘴型才表示实际播放而非网络收包。 */
        julia_avatar_talking_start();
        uint32_t generation = 0;
        if (!play_local_prompt(network_disconnected_wav_start,
                (size_t)(network_disconnected_wav_end - network_disconnected_wav_start),
                "disconnect", false, &generation)) julia_avatar_talking_stop();

        s_disconnect_deadline_us = esp_timer_get_time() + DISCONNECT_NOTICE_US;
        /* 计时器失败时立即执行返回策略，不能让提示态永久占用行为状态机。 */
        esp_err_t err = s_disconnect_timer != NULL
                            ? esp_timer_start_once(s_disconnect_timer,
                                                   DISCONNECT_NOTICE_US)
                            : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "断联提示计时启动失败，owner 将按截止时间返回：%s",
                     esp_err_to_name(err));
        }
    }
}

static void runtime_on_exit(julia_fsm_t *fsm, julia_main_state_t main_state,
                            julia_s2_sub_state_t s2_sub_state, fsm_event_t event)
{
    (void)fsm;
    (void)s2_sub_state;
    (void)event;
    s_ota_prompt_deadline_us = 0;
    if (s_local_prompt_generation != 0) {
        bool stopped = voice_playback_stop_generation(s_local_prompt_generation);
        s_local_prompt_generation = 0;
        if (stopped || !voice_playback_is_active()) julia_avatar_talking_stop();
    }
    if (main_state == JULIA_MAIN_STATE_S1_COMPANION) s_companion_deadline_us = 0;
    if (main_state == JULIA_MAIN_STATE_S3_STANDBY) s_standby_deadline_us = 0;
    if (main_state == JULIA_MAIN_STATE_S5_SILENT) s_silent_deadline_us = 0;
    if (main_state == JULIA_MAIN_STATE_S7_FAULT) s_disconnect_deadline_us = 0;
    if (main_state == JULIA_MAIN_STATE_S3_STANDBY && s_standby_timer != NULL) {
        (void)esp_timer_stop(s_standby_timer);
    }
    if (main_state == JULIA_MAIN_STATE_S5_SILENT && s_silent_timer != NULL) {
        (void)esp_timer_stop(s_silent_timer);
    }
    if (main_state == JULIA_MAIN_STATE_S7_FAULT &&
        fsm->s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED) {
        if (s_disconnect_timer != NULL) (void)esp_timer_stop(s_disconnect_timer);
        julia_avatar_talking_stop();
    }
}

static int64_t *event_deadline(fsm_event_t event)
{
    switch (event) {
    case EVT_USER_LEAVE: return &s_companion_deadline_us;
    case EVT_STANDBY_TIMEOUT: return &s_standby_deadline_us;
    case EVT_SILENT_TIMEOUT: return &s_silent_deadline_us;
    case EVT_DISCONNECT_NOTICE_TIMEOUT: return &s_disconnect_deadline_us;
    case EVT_SERVICE_CONNECT_TIMEOUT: return &s_service_deadline_us;
    default: return NULL;
    }
}

static bool runtime_process_event(fsm_event_t event)
{
    if (event == EVT_REQUIRE_WAKE &&
        (s_fsm.main_state == JULIA_MAIN_STATE_S3_STANDBY ||
         s_fsm.main_state == JULIA_MAIN_STATE_S5_SILENT ||
         s_fsm.main_state == JULIA_MAIN_STATE_S6_SLEEP)) return true;
    if (event == EVT_VOICE_SESSION_RESET &&
        s_fsm.main_state != JULIA_MAIN_STATE_S1_COMPANION &&
        s_fsm.main_state != JULIA_MAIN_STATE_S2_DIALOG &&
        s_fsm.main_state != JULIA_MAIN_STATE_S4_INTERACTION) return true;
    int64_t *deadline = event_deadline(event);
    if (deadline != NULL) {
        if (*deadline == 0 || esp_timer_get_time() < *deadline) return false;
        *deadline = 0;
    }
    julia_service_state_t previous = julia_fsm_runtime_get_service_state();
    bool service_event = service_state_apply_event(event);
    /* Suppress repeated notices, never suppress invalidation of a voice session.
     * MQTT may have been offline throughout an otherwise working voice dialog. */
    if (event == EVT_WSS_DISCONNECTED && previous == JULIA_SERVICE_OFFLINE)
        return runtime_process_event(EVT_VOICE_SESSION_RESET);
    if (service_event && (julia_fsm_runtime_get_service_state() != JULIA_SERVICE_OFFLINE ||
                          previous == JULIA_SERVICE_OFFLINE)) return true;
    bool applied = julia_fsm_handle_event(&s_fsm, event, NULL);
    if (!applied) ESP_LOGD(TAG, "ignored event=%s state=%s/%s", julia_fsm_event_name(event),
                           julia_fsm_main_state_name(s_fsm.main_state),
                           julia_fsm_s2_sub_state_name(s_fsm.s2_sub_state));
    return applied;
}

static void runtime_check_deadlines(void)
{
    const fsm_event_t events[] = {EVT_USER_LEAVE, EVT_STANDBY_TIMEOUT, EVT_SILENT_TIMEOUT,
        EVT_DISCONNECT_NOTICE_TIMEOUT, EVT_SERVICE_CONNECT_TIMEOUT};
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        int64_t deadline = *event_deadline(events[i]);
        if (deadline != 0 && esp_timer_get_time() >= deadline)
            (void)runtime_process_event(events[i]);
    }
}

static bool runtime_process_message_event(const fsm_runtime_message_t *message)
{
    if (message->check_revision && message->expected_revision != s_committed_revision)
        return false;
    return runtime_process_event(message->event);
}

/* Called after local_prompt_poll; never consumes the voice service completion. */
static bool ota_terminal_poll(const fsm_runtime_message_t *terminal, bool *started)
{
    if (s_local_prompt_generation != 0) return false;
    if (*started || s_fsm.main_state != JULIA_MAIN_STATE_S8_OTA) return true;
    bool success = terminal->type == FSM_RUNTIME_MESSAGE_EVENT &&
                   terminal->event == EVT_OTA_SUCCEEDED;
    play_ota_prompt(success ? upgrade_success_wav_start : upgrade_failed_wav_start,
                    success ? upgrade_success_wav_end : upgrade_failed_wav_end,
                    success ? "OTA success" : "OTA failed");
    *started = true;
    return false;
}

/* Only called after S7.2 is committed and an automatic restart is allowed.
 * Fault handling already owns the restart wait; a broken audio task must not
 * hold recovery indefinitely. Do not consume the voice service completion. */
static void play_fault_restart_prompt(void)
{
    const size_t bytes = (size_t)(restart_after_issue_wav_end - restart_after_issue_wav_start);
    julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_LISTENING);
    julia_avatar_set_dozing(false);
    julia_avatar_talking_start();
    uint32_t generation = 0;
    if (play_local_prompt(restart_after_issue_wav_start, bytes, "fault restart",
                          false, &generation)) {
        const int64_t deadline = esp_timer_get_time() +
            (int64_t)bytes * 1000000LL / 32000 + 2000000LL;
        while (voice_playback_generation_is_active(generation)) {
            if (esp_timer_get_time() >= deadline) {
                ESP_LOGW(TAG, "Fault restart prompt timed out; continuing recovery");
                (void)voice_playback_stop_generation(generation);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (!voice_playback_is_active()) julia_avatar_talking_stop();
}

static void fsm_task(void *argument)
{
    (void)argument;
    fsm_runtime_message_t message;
    fsm_runtime_message_t ota_terminal = {0};
    bool ota_terminal_pending = false;
    bool ota_result_started = false;
    for (;;) {
        /* Reconcile even under continuous event traffic, before initial timeout. */
        service_state_reconcile();
        runtime_check_deadlines();
        local_prompt_poll();
        /* Keep servicing the queue during both prompts. Retain the synchronous
         * terminal acknowledgement until audio drains, so OTA cannot reboot early.
         * A fast download/failure must also wait for the start prompt first. */
        bool terminal_ready = false;
        if (ota_terminal_pending && ota_terminal_poll(&ota_terminal, &ota_result_started)) {
            message = ota_terminal;
            ota_terminal_pending = false;
            terminal_ready = true;
        }
        /* 有界等待兼作连接状态巡检周期，不能改回 portMAX_DELAY。 */
        if (!terminal_ready && xQueueReceive(s_event_queue, &message,
                pdMS_TO_TICKS(s_local_prompt_generation != 0 || ota_terminal_pending ? 20 : 1000)) != pdTRUE) {
            local_prompt_poll();
            continue;
        }
        if (!terminal_ready && !ota_terminal_pending &&
            s_fsm.main_state == JULIA_MAIN_STATE_S8_OTA &&
            (message.type == FSM_RUNTIME_MESSAGE_FAULT ||
             message.event == EVT_OTA_SUCCEEDED || message.event == EVT_OTA_TASK_FAILED)) {
            ota_terminal = message;
            ota_terminal_pending = true;
            ota_result_started = false;
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
            play_fault_restart_prompt();
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
        bool applied = runtime_process_message_event(&message);
        if (message.completed != NULL) {
            *message.applied = applied;
            xSemaphoreGive(message.completed);
        }
        local_prompt_poll();
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

    s_service_deadline_us = esp_timer_get_time() +
        (int64_t)CONFIG_JULIA_SERVICE_INIT_TIMEOUT_SECONDS * 1000000LL;
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
        ESP_LOGW(TAG, "初始业务连接计时器不可用，owner 将按截止时间巡检");
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

static esp_err_t runtime_post_sync_checked(fsm_event_t event, bool check_revision,
                                         uint32_t expected_revision)
{
    if (event <= EVT_NONE || event >= EVT_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_event_queue == NULL || s_task == NULL || xTaskGetCurrentTaskHandle() == s_task)
        return ESP_ERR_INVALID_STATE;
    /* The caller waits until the FSM releases this storage. No heap allocation:
     * even a failed OTA task creation must still be able to leave S8. */
    StaticSemaphore_t completed_storage;
    SemaphoreHandle_t completed = xSemaphoreCreateBinaryStatic(&completed_storage);
    if (completed == NULL) return ESP_ERR_NO_MEM;
    bool applied = false;
    fsm_runtime_message_t message = {
        .type = FSM_RUNTIME_MESSAGE_EVENT, .event = event,
        .completed = completed, .applied = &applied,
        .check_revision = check_revision, .expected_revision = expected_revision,
    };
    if (xQueueSend(s_event_queue, &message, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(completed);
        return ESP_ERR_NO_MEM;
    }
    (void)xSemaphoreTake(completed, portMAX_DELAY);
    vSemaphoreDelete(completed);
    return applied ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t julia_fsm_runtime_post_sync(fsm_event_t event)
{
    return runtime_post_sync_checked(event, false, 0);
}

esp_err_t julia_fsm_runtime_require_wake(uint32_t expected_revision)
{
    return runtime_post_sync_checked(EVT_REQUIRE_WAKE, true, expected_revision);
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

void julia_fsm_runtime_get_snapshot(julia_fsm_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    portENTER_CRITICAL(&s_state_lock);
    *snapshot = (julia_fsm_snapshot_t){
        .main_state = s_committed_main_state,
        .s2_sub_state = s_committed_s2_sub_state,
        .s7_sub_state = s_committed_s7_sub_state,
        .reason = s_committed_reason,
        .revision = s_committed_revision,
    };
    int64_t remaining = s_committed_enter_us +
        (int64_t)CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS * 1000000LL - esp_timer_get_time();
    if (snapshot->main_state == JULIA_MAIN_STATE_S1_COMPANION && remaining > 0)
        snapshot->companion_remaining_ms = (uint32_t)((remaining + 999) / 1000);
    portEXIT_CRITICAL(&s_state_lock);
}

julia_service_state_t julia_fsm_runtime_get_service_state(void)
{
    julia_service_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_committed_service_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}
