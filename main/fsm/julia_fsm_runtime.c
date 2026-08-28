/**
 * @file julia_fsm_runtime.c
 * @brief Runtime owner and presentation binding for the existing Julia FSM.
 *
 * The full 6-main/20-sub-state decision table remains in julia_fsm.c.  Phase
 * one only has concrete presentation for the default portrait, two idle/sleep
 * behaviours (QUIET and SLEEP), and LISTEN/THINK/SPEAK.  Every other state is
 * deliberately retained and falls back to the default portrait until a later
 * presentation is supplied.
 */
#include "julia_fsm_runtime.h"

#include <stdbool.h>

#include "esp_log.h"
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
    FSM_PRESENT_FAR_STANDBY,
    FSM_PRESENT_SLEEP,
    FSM_PRESENT_LISTEN,
    FSM_PRESENT_THINK,
    FSM_PRESENT_SPEAK,
} fsm_presentation_t;

static const char *TAG = "JULIA_FSM_RT";
static QueueHandle_t s_event_queue;
static TaskHandle_t s_task;
static julia_fsm_t s_fsm;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static julia_sub_state_t s_committed_state = JULIA_SUB_STATE_S1_1_NEAR_STANDBY;

/* Unspecified entries are zero-initialized to FSM_PRESENT_DEFAULT. */
static const fsm_presentation_t s_presentations[JULIA_SUB_STATE_COUNT] = {
    [JULIA_SUB_STATE_S0_1_NIGHT_SLEEP] = FSM_PRESENT_SLEEP,
    [JULIA_SUB_STATE_S0_2_DAY_AWAY] = FSM_PRESENT_QUIET,
    [JULIA_SUB_STATE_S0_3_MANUAL_SLEEP] = FSM_PRESENT_SLEEP,
    [JULIA_SUB_STATE_S1_2_FAR_STANDBY] = FSM_PRESENT_FAR_STANDBY,
    [JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION] = FSM_PRESENT_QUIET,
    [JULIA_SUB_STATE_S3_3_USER_CALL] = FSM_PRESENT_LISTEN,
    [JULIA_SUB_STATE_S4_1_LIGHT_DIALOG] = FSM_PRESENT_THINK,
    [JULIA_SUB_STATE_S4_2_DEEP_TALK] = FSM_PRESENT_THINK,
    [JULIA_SUB_STATE_S4_3_MULTI_TURN] = FSM_PRESENT_SPEAK,
    [JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE] = FSM_PRESENT_THINK,
};

static const char *presentation_name(fsm_presentation_t presentation)
{
    switch (presentation) {
    case FSM_PRESENT_QUIET: return "QUIET";
    case FSM_PRESENT_FAR_STANDBY: return "FAR_STANDBY";
    case FSM_PRESENT_SLEEP: return "SLEEP";
    case FSM_PRESENT_LISTEN: return "LISTEN";
    case FSM_PRESENT_THINK: return "THINK";
    case FSM_PRESENT_SPEAK: return "SPEAK";
    case FSM_PRESENT_DEFAULT:
    default: return "DEFAULT";
    }
}

static void apply_presentation(julia_sub_state_t state)
{
    fsm_presentation_t presentation = state < JULIA_SUB_STATE_COUNT
                                          ? s_presentations[state]
                                          : FSM_PRESENT_DEFAULT;

    switch (presentation) {
    case FSM_PRESENT_QUIET:
        julia_backlight_breathe_stop();
        julia_backlight_set(100);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(true);
        break;
    case FSM_PRESENT_FAR_STANDBY: {
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(true);
        esp_err_t err = julia_backlight_breathe_start(
            CONFIG_JULIA_DISPLAY_BREATHE_MIN_PERCENT,
            CONFIG_JULIA_DISPLAY_BREATHE_MAX_PERCENT,
            CONFIG_JULIA_DISPLAY_BREATHE_PERIOD_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "far-standby breathing start failed: %s", esp_err_to_name(err));
        }
        break;
    }
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
    case FSM_PRESENT_LISTEN:
        julia_backlight_breathe_stop();
        julia_backlight_set(100);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_LISTENING);
        julia_avatar_set_dozing(false);
        break;
    case FSM_PRESENT_THINK:
        julia_backlight_breathe_stop();
        julia_backlight_set(100);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_THINKING);
        julia_avatar_set_dozing(false);
        break;
    case FSM_PRESENT_SPEAK:
        /* Speaker ownership remains in voice_service; this only selects its frame. */
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

    ESP_LOGI(TAG, "state=%s presentation=%s",
             julia_fsm_sub_state_name(state), presentation_name(presentation));
}

static void runtime_on_enter(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t event)
{
    (void)fsm;
    portENTER_CRITICAL(&s_state_lock);
    s_committed_state = state;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG, "enter %s by %s", julia_fsm_sub_state_name(state),
             julia_fsm_event_name(event));
    apply_presentation(state);
}

static void fsm_task(void *argument)
{
    (void)argument;
    fsm_event_t event;
    while (xQueueReceive(s_event_queue, &event, portMAX_DELAY) == pdTRUE) {
        if (!julia_fsm_handle_event(&s_fsm, event, NULL)) {
            ESP_LOGD(TAG, "ignored event=%s state=%s", julia_fsm_event_name(event),
                     julia_fsm_sub_state_name(s_fsm.sub_state));
        }
    }
    vTaskDelete(NULL);
}

esp_err_t julia_fsm_runtime_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_event_queue = xQueueCreate(FSM_EVENT_QUEUE_DEPTH, sizeof(fsm_event_t));
    if (s_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    julia_fsm_init(&s_fsm);
    s_fsm.on_enter = runtime_on_enter;
    portENTER_CRITICAL(&s_state_lock);
    s_committed_state = s_fsm.sub_state;
    portEXIT_CRITICAL(&s_state_lock);
    apply_presentation(s_fsm.sub_state);

    if (xTaskCreate(fsm_task, "julia_fsm", FSM_TASK_STACK_SIZE, NULL,
                    FSM_TASK_PRIORITY, &s_task) != pdPASS) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready initial=%s queue=%u", julia_fsm_sub_state_name(s_fsm.sub_state),
             (unsigned)FSM_EVENT_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t julia_fsm_runtime_post(fsm_event_t event)
{
    if (event <= EVT_NONE || event > EVT_WAKEUP) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_event_queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

julia_sub_state_t julia_fsm_runtime_get_state(void)
{
    julia_sub_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_committed_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}
