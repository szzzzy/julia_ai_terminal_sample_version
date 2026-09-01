/**
 * @file julia_fsm_runtime.c
 * @brief Julia 行为状态机的单实例运行时与现有呈现适配。
 *
 * 本模块串行处理事件并保存当前状态。S2.1、S2.2、S2.3 分别调用项目
 * 已有的听、想、说呈现接口；S4 只复用 S2.1 的呈现，不与 S2.1 合并状态。
 */
#include "julia_fsm_runtime.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "julia_avatar.h"
#include "julia_backlight.h"
#include "sdkconfig.h"

#define FSM_EVENT_QUEUE_DEPTH 16
#define FSM_TASK_STACK_SIZE   4096
#define FSM_TASK_PRIORITY     4
#define FSM_FAULT_RESET_DELAY_MS 3000

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
    FSM_PRESENT_S3_STANDBY,
    FSM_PRESENT_S6_SLEEP,
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
static esp_timer_handle_t s_standby_timer;
static esp_timer_handle_t s_silent_timer;

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

static fsm_presentation_t presentation_for(julia_main_state_t main_state,
                                           julia_s2_sub_state_t s2_sub_state)
{
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
    if (main_state == JULIA_MAIN_STATE_S3_STANDBY) return FSM_PRESENT_S3_STANDBY;
    if (main_state == JULIA_MAIN_STATE_S6_SLEEP) return FSM_PRESENT_S6_SLEEP;
    /* 调试阶段 S0/S1/S5/S7/S8 共用 Companion 基础 UI，由状态叠字区分。 */
    return FSM_PRESENT_DEFAULT;
}

static const char *presentation_name(fsm_presentation_t presentation)
{
    switch (presentation) {
    case FSM_PRESENT_S3_STANDBY: return "S3_STANDBY";
    case FSM_PRESENT_S6_SLEEP: return "S6_SLEEP";
    case FSM_PRESENT_S2_1_LISTENING: return "S2.1_LISTENING";
    case FSM_PRESENT_S2_2_THINKING: return "S2.2_THINKING";
    case FSM_PRESENT_S2_3_SPEAKING: return "S2.3_SPEAKING";
    case FSM_PRESENT_DEFAULT:
    default: return "DEFAULT";
    }
}

static const char *state_status_text(julia_main_state_t main_state,
                                     julia_s2_sub_state_t s2_sub_state)
{
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
    case JULIA_MAIN_STATE_S7_FAULT: return "S7 FAULT";
    case JULIA_MAIN_STATE_S8_OTA: return "S8 OTA";
    case JULIA_MAIN_STATE_S2_DIALOG: return "S2 DIALOG";
    case JULIA_MAIN_STATE_COUNT:
    default: return "STATE UNKNOWN";
    }
}

static void apply_presentation(julia_main_state_t main_state,
                               julia_s2_sub_state_t s2_sub_state)
{
    fsm_presentation_t presentation = presentation_for(main_state, s2_sub_state);
    switch (presentation) {
    case FSM_PRESENT_S3_STANDBY:
    case FSM_PRESENT_S6_SLEEP: {
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(true);
        esp_err_t err = julia_backlight_breathe_start(
            CONFIG_JULIA_DISPLAY_BREATHE_MIN_PERCENT,
            CONFIG_JULIA_DISPLAY_BREATHE_MAX_PERCENT,
            CONFIG_JULIA_DISPLAY_BREATHE_PERIOD_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "standby/sleep breathing start failed: %s",
                     esp_err_to_name(err));
        }
        break;
    }
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

    ESP_LOGI(TAG, "state=%s/%s presentation=%s",
             julia_fsm_main_state_name(main_state),
             julia_fsm_s2_sub_state_name(s2_sub_state),
             presentation_name(presentation));
}

static void runtime_on_enter(julia_fsm_t *fsm, julia_main_state_t main_state,
                             julia_s2_sub_state_t s2_sub_state, fsm_event_t event)
{
    (void)fsm;
    portENTER_CRITICAL(&s_state_lock);
    s_committed_main_state = main_state;
    s_committed_s2_sub_state = s2_sub_state;
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "enter %s/%s by %s", julia_fsm_main_state_name(main_state),
             julia_fsm_s2_sub_state_name(s2_sub_state), julia_fsm_event_name(event));
    julia_avatar_set_status_text(state_status_text(main_state, s2_sub_state));
    apply_presentation(main_state, s2_sub_state);
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
}

static void fsm_task(void *argument)
{
    (void)argument;
    fsm_runtime_message_t message;
    while (xQueueReceive(s_event_queue, &message, portMAX_DELAY) == pdTRUE) {
        if (message.type == FSM_RUNTIME_MESSAGE_FAULT) {
            julia_main_state_t previous_main = s_fsm.main_state;
            julia_s2_sub_state_t previous_sub = s_fsm.s2_sub_state;
            esp_err_t record_err = julia_fault_record(message.fault_reason, message.error,
                                                      previous_main, previous_sub);
            ESP_LOGE(TAG, "严重故障：reason=%s err=%s，进入 S7",
                     julia_fault_reason_name(message.fault_reason),
                     esp_err_to_name(message.error));
            if (s_fsm.main_state != JULIA_MAIN_STATE_S7_FAULT) {
                (void)julia_fsm_transition_to(&s_fsm, JULIA_MAIN_STATE_S7_FAULT,
                                              JULIA_S2_SUB_STATE_NONE, EVT_NONE);
            }
            if (record_err == ESP_OK && !julia_fault_reset_allowed()) {
                ESP_LOGE(TAG, "同类故障连续超过自动复位上限，保持 S7 等待售后处理");
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(FSM_FAULT_RESET_DELAY_MS));
            esp_restart();
            continue;
        }
        if (!julia_fsm_state_is_valid(s_fsm.main_state, s_fsm.s2_sub_state)) {
            (void)julia_fault_record(JULIA_FAULT_FSM_STATE_CORRUPT,
                                     ESP_ERR_INVALID_STATE,
                                     s_committed_main_state,
                                     s_committed_s2_sub_state);
            ESP_LOGE(TAG, "FSM 状态非法，立即复位");
            esp_restart();
            continue;
        }
        if (!julia_fsm_handle_event(&s_fsm, message.event, NULL)) {
            ESP_LOGD(TAG, "ignored event=%s state=%s/%s",
                     julia_fsm_event_name(message.event),
                     julia_fsm_main_state_name(s_fsm.main_state),
                     julia_fsm_s2_sub_state_name(s_fsm.s2_sub_state));
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
    portENTER_CRITICAL(&s_state_lock);
    s_committed_main_state = s_fsm.main_state;
    s_committed_s2_sub_state = s_fsm.s2_sub_state;
    portEXIT_CRITICAL(&s_state_lock);
    if (boot_dependencies_ready) {
        /* 直接复用现有初始化结果，不创建仅供状态机使用的开机完成事件。 */
        if (!julia_fsm_transition_to(&s_fsm, JULIA_MAIN_STATE_S1_COMPANION,
                                     JULIA_S2_SUB_STATE_NONE, EVT_NONE)) {
            vQueueDelete(s_event_queue);
            s_event_queue = NULL;
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        apply_presentation(s_fsm.main_state, s_fsm.s2_sub_state);
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
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready initial=%s/%s queue=%u",
             julia_fsm_main_state_name(s_fsm.main_state),
             julia_fsm_s2_sub_state_name(s_fsm.s2_sub_state),
             (unsigned)FSM_EVENT_QUEUE_DEPTH);
    return ESP_OK;
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
