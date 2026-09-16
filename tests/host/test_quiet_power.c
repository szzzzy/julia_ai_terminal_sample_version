/* Exercise the actual coordinator loop with delayed IMU readiness, playback,
 * unavailable Wi-Fi, repeated sleep and sensor failure. No real peripherals. */
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskCreate(void (*fn)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
#include "../../main/app/julia_quiet_power.c"

static jmp_buf done;
static unsigned step;
static int64_t clock_us;
static julia_main_state_t test_state = JULIA_MAIN_STATE_S3_STANDBY;
static bool mic_on = true, radio_paused, imu_ready, playing, online = true;
static unsigned imu_failures;
static bool wake_paused;

julia_main_state_t julia_fsm_runtime_get_state(void) { return test_state; }
julia_service_state_t julia_fsm_runtime_get_service_state(void)
{ return online ? JULIA_SERVICE_ONLINE : JULIA_SERVICE_CONNECTING; }
bool julia_motion_ready(void) { return imu_ready; }
bool voice_playback_is_active(void) { return playing; }
bool board_audio_mic_is_enabled(void) { return mic_on; }
void board_audio_mic_set_enabled(bool value) { mic_on = value; }
void network_lifecycle_set_paused(bool value) { radio_paused = value; }
void wake_detector_set_paused(bool value) { wake_paused = value; }
int64_t esp_timer_get_time(void) { return clock_us; }
esp_err_t julia_fsm_runtime_post(fsm_event_t event)
{
    assert(event == EVT_IMU_UNAVAILABLE);
    ++imu_failures;
    test_state = JULIA_MAIN_STATE_S3_STANDBY;
    return ESP_OK;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned size,
                      void *arg, unsigned priority, TaskHandle_t *handle)
{
    (void)fn; (void)name; (void)size; (void)arg; (void)priority;
    *handle = (void *)1;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks)
{
    clock_us += (int64_t)ticks * 1000;
    ++step;
    switch (step) {
    case 1:
        assert(mic_on && !radio_paused);
        test_state = JULIA_MAIN_STATE_S5_SILENT;
        break;
    case 2:
        assert(!mic_on && !radio_paused); /* No offline mode without working IMU. */
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
        assert(wake_paused);
#endif
        imu_ready = true;
        playing = true;
        break;
    case 3:
        assert(!radio_paused); /* Finish the local farewell before closing services. */
        playing = false;
        break;
    case 4:
        assert(radio_paused && !mic_on);
        test_state = JULIA_MAIN_STATE_S6_SLEEP;
        break;
    case 5:
        assert(radio_paused && !mic_on);
        test_state = JULIA_MAIN_STATE_S3_STANDBY;
        online = false;
        break;
    case 6:
        assert(!radio_paused && !mic_on); /* Radio resume must precede voice. */
        break;
    case 8:
        assert(!mic_on);
        online = true;
        break;
    case 9:
        assert(mic_on);
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
        assert(!wake_paused);
#endif
        test_state = JULIA_MAIN_STATE_S5_SILENT;
        break;
    case 10:
        assert(radio_paused && !mic_on);
        imu_ready = false;
        break;
    case 34:
        assert(imu_failures == 1 && test_state == JULIA_MAIN_STATE_S3_STANDBY);
        assert(!radio_paused && mic_on);
        test_state = JULIA_MAIN_STATE_S8_OTA;
        break;
    case 35:
        assert(!radio_paused && !mic_on); /* OTA keeps network, pauses voice. */
        test_state = JULIA_MAIN_STATE_S3_STANDBY;
        break;
    case 36:
        assert(mic_on && !radio_paused);
        longjmp(done, 1);
    }
}
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
unsigned ulTaskNotifyTake(int clear, TickType_t ticks)
{
    assert(ticks == portMAX_DELAY && !radio_paused);
    switch (++step) {
    case 1: assert(mic_on); test_state=JULIA_MAIN_STATE_S5_SILENT; break;
    case 2: assert(mic_on); test_state=JULIA_MAIN_STATE_S6_SLEEP; break;
    case 3: assert(mic_on); test_state=JULIA_MAIN_STATE_S8_OTA; break;
    case 4: assert(!mic_on); test_state=JULIA_MAIN_STATE_S3_STANDBY; break;
    case 5: assert(mic_on); longjmp(done,1);
    }
    return 1;
}
#endif
int main(void)
{
    assert(julia_quiet_power_init() == ESP_OK);
    assert(julia_quiet_power_init() == ESP_OK);
    if (setjmp(done) == 0) quiet_power_task(NULL);
    return 0;
}
