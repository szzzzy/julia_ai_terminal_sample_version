/**
 * @file julia_fsm_runtime.c
 * @brief Julia FSM 的运行时持有者与呈现绑定。
 *
 * 听、想、说阶段复用现有语音事件。S4 暂时复用 S2.1“听”的呈现效果，
 * 但两者仍保持各自独立的状态身份；其他事件映射后续补充。
 */
#include "julia_fsm_runtime.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "julia_avatar.h"
#include "julia_backlight.h"
#include "sdkconfig.h"

#define FSM_EVENT_QUEUE_DEPTH 16
#define FSM_TASK_STACK_SIZE   4096
#define FSM_TASK_PRIORITY     4

typedef enum {
    FSM_PRESENT_DEFAULT = 0,
    FSM_PRESENT_QUIET,
    FSM_PRESENT_SLEEP,
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
    if (main_state == JULIA_MAIN_STATE_S5_SILENT) return FSM_PRESENT_QUIET;
    if (main_state == JULIA_MAIN_STATE_S6_SLEEP) return FSM_PRESENT_SLEEP;
    return FSM_PRESENT_DEFAULT;
}

static const char *presentation_name(fsm_presentation_t presentation)
{
    switch (presentation) {
    case FSM_PRESENT_QUIET: return "QUIET";
    case FSM_PRESENT_SLEEP: return "SLEEP";
    case FSM_PRESENT_S2_1_LISTENING: return "S2.1_LISTENING";
    case FSM_PRESENT_S2_2_THINKING: return "S2.2_THINKING";
    case FSM_PRESENT_S2_3_SPEAKING: return "S2.3_SPEAKING";
    case FSM_PRESENT_DEFAULT:
    default: return "DEFAULT";
    }
}

static void apply_presentation(julia_main_state_t main_state,
                               julia_s2_sub_state_t s2_sub_state)
{
    fsm_presentation_t presentation = presentation_for(main_state, s2_sub_state);
    switch (presentation) {
    case FSM_PRESENT_QUIET:
        julia_backlight_breathe_stop();
        julia_backlight_set(100);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(true);
        break;
    case FSM_PRESENT_SLEEP: {
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(true);
        esp_err_t err = julia_backlight_breathe_start(
            CONFIG_JULIA_DISPLAY_BREATHE_MIN_PERCENT,
            CONFIG_JULIA_DISPLAY_BREATHE_MAX_PERCENT,
            CONFIG_JULIA_DISPLAY_BREATHE_PERIOD_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sleep breathing start failed: %s", esp_err_to_name(err));
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
    if (main_state == JULIA_MAIN_STATE_S7_FAULT) {
        /* S7 不承载恢复流程，记录状态后立即复位；重启入口自然回到 S0。 */
        ESP_LOGE(TAG, "进入 S7 故障态，立即复位系统");
        esp_restart();
        return;
    }
    apply_presentation(main_state, s2_sub_state);
}

static void fsm_task(void *argument)
{
    (void)argument;
    fsm_event_t event;
    while (xQueueReceive(s_event_queue, &event, portMAX_DELAY) == pdTRUE) {
        if (!julia_fsm_handle_event(&s_fsm, event, NULL)) {
            ESP_LOGD(TAG, "ignored event=%s state=%s/%s", julia_fsm_event_name(event),
                     julia_fsm_main_state_name(s_fsm.main_state),
                     julia_fsm_s2_sub_state_name(s_fsm.s2_sub_state));
        }
    }
    vTaskDelete(NULL);
}

esp_err_t julia_fsm_runtime_init(void)
{
    if (s_task != NULL) return ESP_OK;
    s_event_queue = xQueueCreate(FSM_EVENT_QUEUE_DEPTH, sizeof(fsm_event_t));
    if (s_event_queue == NULL) return ESP_ERR_NO_MEM;

    julia_fsm_init(&s_fsm);
    s_fsm.on_enter = runtime_on_enter;
    portENTER_CRITICAL(&s_state_lock);
    s_committed_main_state = s_fsm.main_state;
    s_committed_s2_sub_state = s_fsm.s2_sub_state;
    portEXIT_CRITICAL(&s_state_lock);
    apply_presentation(s_fsm.main_state, s_fsm.s2_sub_state);

    if (xTaskCreate(fsm_task, "julia_fsm", FSM_TASK_STACK_SIZE, NULL,
                    FSM_TASK_PRIORITY, &s_task) != pdPASS) {
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
    return xQueueSend(s_event_queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
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
