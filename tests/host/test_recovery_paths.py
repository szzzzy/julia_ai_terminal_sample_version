"""Run firmware functions under deterministic scheduling and I/O failures.

Only peripheral/RTOS boundaries are replaced. Function bodies are extracted from
the current source so these regressions exercise the shipped implementation.
"""
import argparse
from pathlib import Path
import re
import subprocess


def function(source, name):
    match = re.search(r"^(?:static )?[A-Za-z_]\w*(?:[ \t]+\w+)*[ \t*]+" + name +
                      r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise ValueError("Function not found: " + name)
    start = match.start()
    i = match.end() - 1
    depth = 0
    mode = "code"
    while i < len(source):
        c, following = source[i], source[i + 1:i + 2]
        if mode == "line":
            if c == "\n":
                mode = "code"
        elif mode == "block":
            if c == "*" and following == "/":
                mode = "code"
                i += 1
        elif mode in ("string", "char"):
            if c == "\\":
                i += 1
            elif c == ('"' if mode == "string" else "'"):
                mode = "code"
        elif c == "/" and following in ("/", "*"):
            mode = "line" if following == "/" else "block"
            i += 1
        elif c in ('"', "'"):
            mode = "string" if c == '"' else "char"
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
        i += 1
    raise ValueError("Unclosed function: " + name)


COMMON = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "esp_err.h"
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_VERSION 0x10a
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((ms)/10U)
#define portTICK_PERIOD_MS 10U
#define ESP_RETURN_ON_ERROR(expr, ...) do {esp_err_t e=(expr);if(e!=ESP_OK)return e;} while(0)
typedef void *SemaphoreHandle_t;
typedef uint32_t TickType_t;
'''


def run_case(args, name, prefix, units, main, extra=()):
    root = args.root
    body = []
    for path, names in units:
        source = (root / path).read_text(encoding="utf-8")
        body += [function(source, n) for n in names]
    code = args.out / (name + ".c")
    exe = args.out / (name + (".exe" if Path(args.cc).suffix == ".exe" else ""))
    code.write_text(COMMON + prefix + "\n" + "\n".join(body) + "\n" + main, encoding="utf-8")
    command = [args.cc, "-I" + str(root / "tests/host/stubs"),
               "-I" + str(root / "main/fsm"), "-I" + str(root / "main/hardware"), str(code)]
    command += [str(root / p) for p in extra] + ["-o", str(exe)]
    subprocess.run(command, check=True)
    subprocess.run([str(exe)], check=True)


def backlight(args):
    source = (args.root / "main/ui/julia_backlight.c").read_text(encoding="utf-8")
    tables = source[source.index("static const uint16_t s_sine_q10"):source.index("static volatile uint8_t s_percent")]
    prefix = r'''
#define BL_MODE 0
#define BL_CHANNEL 0
#define BL_MAX_DUTY 1023U
#define BREATHE_LUT_SEGMENTS 120U
#define BREATHE_MIN_SEGMENT_MS 5U
#define BREATHE_ZERO_HOLD_PERCENT 15U
#define JULIA_DISPLAY_LOG 0
#define LEDC_FADE_NO_WAIT 0
static uint8_t s_percent,s_min_percent,s_max_percent;
static bool s_breathing,s_gamma_enabled=true;
static uint16_t s_curve_index,s_segments;
static uint32_t s_generation,s_period_ms,s_segment_ms;
static TickType_t s_segment_started_tick;
static void *s_control_lock=(void*)1,*s_fade_done;
static jmp_buf done;
static unsigned notifications,duty,target,task=1,owner,depth;
static bool inject_stop,pending_stop;
void julia_backlight_set(uint8_t percent);
void julia_backlight_breathe_stop(void);
static int xSemaphoreTakeRecursive(void *s,uint32_t wait) {
    assert(s); if(depth && owner!=task){pending_stop=true;return 0;}
    owner=task;depth++;return 1;
}
static void xSemaphoreGiveRecursive(void *s) {
    assert(depth && owner==task);--depth;
    if(!depth && pending_stop){pending_stop=false;unsigned saved=task;task=2;julia_backlight_set(0);task=saved;}
}
static int ledc_fade_stop(int m,int c){assert(depth && owner==task);return 0;}
static int ledc_set_duty(int m,int c,unsigned d){assert(depth);duty=d;return 0;}
static int ledc_update_duty(int m,int c){assert(depth);return 0;}
static unsigned ledc_get_duty(int m,int c){return duty;}
static int ledc_set_fade_with_time(int m,int c,unsigned d,unsigned t) {
    assert(depth && owner==task);
    if(inject_stop){inject_stop=false;task=2;julia_backlight_set(0);task=1;}
    target=d;return 0;
}
static int ledc_fade_start(int m,int c,int mode){assert(depth);duty=target;return 0;}
static void ulTaskNotifyTake(int clear,uint32_t ticks){if(notifications++)longjmp(done,1);}
static void xSemaphoreGive(void *s){}
static void vTaskDelayUntil(TickType_t *last,TickType_t increment){assert(!depth && increment>0);*last+=increment;}
static TickType_t xTaskGetTickCount(void){return 0;}
'''
    run_case(args, "backlight_recovery", prefix + tables, [("main/ui/julia_backlight.c", [
        "control_lock", "control_unlock", "duty_for", "curve_duty", "start_segment", "breathe_task",
        "julia_backlight_breathe_stop", "julia_backlight_set", "julia_backlight_breathe_start_ex"])], r'''
int main(void) {
    assert(julia_backlight_breathe_start_ex(5,30,4000,120)==ESP_OK);
    s_curve_index=59;inject_stop=true;notifications=0;
    if(!setjmp(done))breathe_task(NULL);
    assert(!s_breathing && duty==0 && depth==0);
    unsigned periods[]={500,600,1199,4000};
    for(unsigned i=0;i<4;i++){
        assert(julia_backlight_breathe_start_ex(5,30,periods[i],120)==ESP_OK);
        notifications=0;if(!setjmp(done))breathe_task(NULL);
        assert(s_segment_ms>=10 && s_segments<=120 && depth==0);
    }
    puts("PASS: concurrent sleep wins over old fade; legal short periods use nonzero ticks");return 0;
}
''')


def rtc(args):
    prefix = r'''
#include "pcf85063_shared.h"
#define PCF85063_SECONDS_REG 4
#define PCF85063_YEAR_OFFSET 1970
static uint8_t registers[8];
static unsigned writes;
static bool fail_read,fail_write;
static esp_err_t read_regs(uint8_t r,uint8_t *d,size_t n){if(fail_read)return ESP_FAIL;memcpy(d,registers+r-3,n);return ESP_OK;}
static esp_err_t write_regs(uint8_t r,const uint8_t *d,size_t n){if(fail_write)return ESP_FAIL;writes++;memcpy(registers+r-3,d,n);return ESP_OK;}
'''
    run_case(args, "rtc_recovery", prefix, [("main/hardware/pcf85063_shared.c", [
        "board_rtc_read_time", "board_rtc_set_time"])], r'''
int main(void){
    board_rtc_datetime_t time={.year=2026,.month=9,.day=5,.dotw=6,.hour=12};
    registers[0]=0x37;board_rtc_encode(&time,registers+1);
    board_rtc_datetime_t got={0};
    assert(board_rtc_read_time(&got)==ESP_OK && got.year==2026 && writes==0);
    time.year=2028;assert(board_rtc_set_time(&time)==ESP_OK && registers[7]==0x58 && registers[0]==0x37);
    registers[1]|=0x80;got.year=123;
    assert(board_rtc_read_time(&got)==ESP_ERR_INVALID_STATE && got.year==123);
    fail_write=true;assert(board_rtc_set_time(&time)==ESP_FAIL);
    fail_read=true;assert(board_rtc_read_time(&got)==ESP_FAIL && got.year==123);
    fail_write=false;time.year=2026;time.month=2;time.day=29;
    assert(board_rtc_set_time(&time)==ESP_ERR_INVALID_ARG && writes==1);
    puts("PASS: RTC retains legacy encoding, rejects stopped/invalid clocks and propagates I/O failures");return 0;
}
''', ["main/hardware/rtc_calendar.c"])


def display(args):
    prefix = r'''
static void *s_panel=(void*)1,*s_panel_mutex=(void*)1;
static bool s_display_off=true,s_display_target_off=true,s_display_state_known=true;
static int64_t s_wake_started_us;
static unsigned attempts;static bool fail=true;
static int xSemaphoreTake(void *s,unsigned ticks){return 1;}
static void xSemaphoreGive(void *s){}
static esp_err_t esp_lcd_panel_disp_on_off(void *p,bool on){attempts++;return fail?ESP_FAIL:ESP_OK;}
static int64_t esp_timer_get_time(void){return 100;}
'''
    run_case(args, "display_recovery", prefix, [("main/lvgl_port/lvgl_port.c", [
        "apply_display_target", "lvgl_port_set_display_off"])], r'''
int main(void){
    assert(lvgl_port_set_display_off(false)==ESP_FAIL && s_display_off && !s_display_state_known);
    fail=false;assert(apply_display_target()==ESP_OK && !s_display_off && s_display_state_known);
    assert(attempts==2);assert(lvgl_port_set_display_off(false)==ESP_OK && attempts==2);
    assert(lvgl_port_set_display_off(true)==ESP_OK && s_display_off);
    puts("PASS: LCD hardware failures remain pending and are retried");return 0;
}
''')


def mqtt(args):
    prefix = r'''
#define MQTT_STATUS_TRACK_SIZE 12
#define NATIVE_OTA_EVENT_ID_SIZE 33
#define CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS 30000
#define MQTT_ERROR_TYPE_SUBSCRIBE_FAILED 3
typedef struct{int error_type;} mqtt_error_t;
typedef struct{char *data;int data_len;mqtt_error_t *error_handle;} mqtt_event_t;
typedef mqtt_event_t *esp_mqtt_event_handle_t;
typedef struct{int state;char event_id[33];char json[16];size_t json_len;} native_ota_report_message_t;
typedef struct{bool used;int msg_id;int64_t sent_us;char event_id[33];} mqtt_status_track_t;
static mqtt_status_track_t s_status_tracks[12];
static void *s_status_queue=(void*)1,*s_status_ack_queue=(void*)2,*s_status_lock=(void*)3,*s_client=(void*)4;
static char s_status_topic[]="status";
static native_ota_report_message_t message={0,"event-1","{}",2};
static bool queued=true,ack_pending=true,s_status_need_flush;static int64_t now_us=1;
static unsigned report_acks;
static bool mqtt_status_is_ready(void){return true;}
static int64_t esp_timer_get_time(void){return now_us;}
static int xQueuePeek(void *q,void *p,unsigned t){if(queued){memcpy(p,&message,sizeof(message));return 1;}return 0;}
static int xQueueReceive(void *q,void *p,unsigned t){if(q==s_status_queue&&queued){queued=false;memcpy(p,&message,sizeof(message));return 1;}if(q==s_status_ack_queue&&ack_pending){ack_pending=false;*(int*)p=42;return 1;}return 0;}
static int esp_mqtt_client_enqueue(void *c,const char *t,const char *j,int n,int qos,int retain,bool store){return 42;}
static int xSemaphoreTake(void *s,uint32_t ticks){return ticks==0?0:1;}
static void xSemaphoreGive(void *s){}
static void native_ota_report_ack_event(const char *s){report_acks++;}
'''
    run_case(args, "mqtt_recovery", prefix, [("main/network/mqtt_comm.c", [
        "mqtt_status_find_event_locked", "mqtt_status_find_free_locked", "mqtt_status_drain_one_critical",
        "mqtt_status_process_published", "mqtt_status_expire_tracks", "mqtt_suback_succeeded"])], r'''
int main(void){
    s_status_tracks[0].used=true;strcpy(s_status_tracks[0].event_id,"event-1");
    assert(mqtt_status_drain_one_critical() && s_status_tracks[0].msg_id==42);
    assert(mqtt_status_process_published() && report_acks==1 && !s_status_tracks[0].used);
    s_status_tracks[0].used=true;s_status_tracks[0].msg_id=43;s_status_tracks[0].sent_us=1;
    now_us=35000002;mqtt_status_expire_tracks();assert(!s_status_tracks[0].used && s_status_need_flush);
    char code=0;mqtt_event_t event={&code,1,NULL};assert(mqtt_suback_succeeded(&event));
    code=2;assert(mqtt_suback_succeeded(&event));code=(char)0x80;assert(!mqtt_suback_succeeded(&event));
    code=0;mqtt_error_t error={3};event.error_handle=&error;assert(!mqtt_suback_succeeded(&event));
    puts("PASS: PUBACK binding survives lock contention, expired tracks retry, rejected SUBACK stays offline");return 0;
}
''')


def fsm(args):
    prefix = r'''
#include "julia_fsm_runtime.h"
#define CONFIG_JULIA_STANDBY_SLEEP_TIMEOUT_SECONDS 300
#define CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS 300
#define CONFIG_JULIA_SILENT_STANDBY_TIMEOUT_SECONDS 1800
#define SERVICE_LINK_MQTT 1
#define SERVICE_LINK_WSS 2
#define SERVICE_LINK_ALL 3
#define DISCONNECT_NOTICE_US 3000000ULL
typedef struct {int type;fsm_event_t event;julia_fault_reason_t fault_reason;esp_err_t error;
    SemaphoreHandle_t completed;bool *applied;bool check_revision;uint32_t expected_revision;} fsm_runtime_message_t;
#define FSM_RUNTIME_MESSAGE_EVENT 0
static julia_fsm_t s_fsm;
static julia_main_state_t s_committed_main_state;
static julia_s2_sub_state_t s_committed_s2_sub_state;
static julia_s7_sub_state_t s_committed_s7_sub_state;
static uint32_t s_committed_revision;
static int64_t s_committed_enter_us;
static fsm_event_t s_committed_reason;
static julia_service_state_t s_committed_service_state=JULIA_SERVICE_ONLINE;
static uint8_t s_online_links=3;
static void *s_event_queue=(void*)1,*s_task=(void*)2,*current_task=(void*)1;
static void *s_standby_timer=(void*)1,*s_silent_timer=(void*)1,*s_disconnect_timer=(void*)1,*s_service_init_timer=(void*)1;
static int64_t s_standby_deadline_us,s_silent_deadline_us,s_disconnect_deadline_us,s_service_deadline_us,now;
static int64_t s_companion_deadline_us;
static julia_fsm_state_observer_t s_state_observer;
static void *s_state_observer_ctx;
static fsm_runtime_message_t queued;
static bool pending;
static bool wake_before_dispatch;
static bool mqtt_ready=true,wss_ready=true;
static bool cloud_ready=true;
static bool voice_state_sync_is_ready(void){return cloud_ready;}
static bool mqtt_comm_is_ready(void){return mqtt_ready;}
static bool wss_transport_is_ready(void){return wss_ready;}
static bool runtime_process_event(fsm_event_t event);
static bool runtime_process_message_event(const fsm_runtime_message_t *message);
static int64_t esp_timer_get_time(void){return now;}
static int esp_timer_start_once(void *t,uint64_t us){return ESP_OK;}
static int esp_timer_stop(void *t){return ESP_OK;}
static void *xTaskGetCurrentTaskHandle(void){return current_task;}
typedef int StaticSemaphore_t;
static void *xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s){*s=0;return s;}
static void vSemaphoreDelete(void *s){assert(*(int*)s);}
static int xQueueSend(void *q,const void *m,unsigned t){assert(!pending);queued=*(const fsm_runtime_message_t*)m;pending=true;return 1;}
static void xSemaphoreGive(void *s){*(int*)s=1;}
static int xSemaphoreTake(void *s,unsigned t){
    assert(pending);pending=false;current_task=s_task;
    if(wake_before_dispatch){wake_before_dispatch=false;assert(runtime_process_event(EVT_WAKEUP));}
    *queued.applied=runtime_process_message_event(&queued);xSemaphoreGive(queued.completed);
    current_task=(void*)1;return *(int*)s;
}
static void apply_presentation(julia_main_state_t m,julia_s2_sub_state_t s,julia_s7_sub_state_t f){}
static const char *state_status_text(julia_main_state_t m,julia_s2_sub_state_t s,julia_s7_sub_state_t f){return "state";}
static void julia_avatar_set_status_text(const char *s){}
static void julia_avatar_set_offline(bool o){}
static void julia_avatar_talking_start(void){}
static void julia_avatar_talking_stop(void){}
static const uint8_t network_disconnected_wav_start[1],network_disconnected_wav_end[1];
static uint32_t s_local_prompt_generation;
static int64_t s_ota_prompt_deadline_us;
static const uint8_t upgrade_start_wav_start[1],upgrade_start_wav_end[1];
static const uint8_t upgrade_success_wav_start[1],upgrade_success_wav_end[1];
static const uint8_t upgrade_failed_wav_start[1],upgrade_failed_wav_end[1];
static const uint8_t *ota_played;
static bool ota_play_fail;
static void play_ota_prompt(const uint8_t *start,const uint8_t *end,const char *name){
    ota_played=start;s_local_prompt_generation=ota_play_fail?0:42;
}
static bool voice_playback_stop_generation(uint32_t generation){return false;}
static bool voice_playback_is_active(void){return false;}
static bool play_local_prompt(const uint8_t *wav,size_t bytes,const char *name,bool idle,uint32_t *generation){return false;}
esp_err_t julia_fsm_runtime_post(fsm_event_t event){return ESP_ERR_NO_MEM;}
'''
    names = ["julia_fsm_runtime_get_service_state", "julia_fsm_runtime_get_state",
             "julia_fsm_runtime_get_s2_sub_state", "service_state_apply_event", "service_state_reconcile", "runtime_on_enter",
             "runtime_on_exit", "event_deadline", "runtime_process_event", "runtime_check_deadlines",
             "disconnect_timer_callback", "runtime_process_message_event", "runtime_post_sync_checked",
             "julia_fsm_runtime_post_sync", "julia_fsm_runtime_require_wake", "julia_fsm_runtime_get_snapshot",
             "ota_terminal_poll"]
    run_case(args, "fsm_recovery", prefix, [("main/fsm/julia_fsm_runtime.c", names)], r'''
int main(void){
    s_fsm.main_state=JULIA_MAIN_STATE_S8_OTA;
    fsm_runtime_message_t terminal={.type=FSM_RUNTIME_MESSAGE_EVENT,.event=EVT_OTA_SUCCEEDED};
    bool started=false;
    s_local_prompt_generation=41; /* Start notice is still playing. */
    assert(!ota_terminal_poll(&terminal,&started) && !started && !ota_played);
    s_local_prompt_generation=0;
    assert(!ota_terminal_poll(&terminal,&started) && started && ota_played==upgrade_success_wav_start);
    assert(!ota_terminal_poll(&terminal,&started)); /* Never release reboot during PCM. */
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S8_OTA);
    s_local_prompt_generation=0;
    assert(ota_terminal_poll(&terminal,&started));
    started=false;terminal.event=EVT_OTA_TASK_FAILED;
    assert(!ota_terminal_poll(&terminal,&started) && ota_played==upgrade_failed_wav_start);
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S8_OTA);
    s_local_prompt_generation=0;
    assert(ota_terminal_poll(&terminal,&started));
    started=false;ota_play_fail=true;
    assert(!ota_terminal_poll(&terminal,&started));
    assert(ota_terminal_poll(&terminal,&started)); /* Audio failure cannot trap S8. */
    started=false;s_fsm.main_state=JULIA_MAIN_STATE_S3_STANDBY;
    assert(ota_terminal_poll(&terminal,&started) && !started);
    julia_fsm_init(&s_fsm);s_fsm.on_enter=runtime_on_enter;s_fsm.on_exit=runtime_on_exit;
    assert(julia_fsm_transition_to(&s_fsm,JULIA_MAIN_STATE_S3_STANDBY,JULIA_S2_SUB_STATE_NONE,EVT_NONE));
    assert(julia_fsm_runtime_post_sync(EVT_WAKEUP)==ESP_OK);
    assert(julia_fsm_runtime_post_sync(EVT_START_DIALOG)==ESP_OK);
    assert(s_committed_s2_sub_state==JULIA_S2_SUB_STATE_S2_2_THINKING);
    assert(julia_fsm_runtime_post_sync(EVT_OTA_AVAILABLE)==ESP_ERR_INVALID_STATE);
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S2_DIALOG);
    assert(julia_fsm_runtime_post_sync(EVT_MULTI_TURN_DETECTED)==ESP_OK);
    assert(julia_fsm_runtime_post_sync(EVT_SILENCE_TIMEOUT)==ESP_OK);
    assert(julia_fsm_runtime_post_sync(EVT_NIGHT_TIME)==ESP_OK && s_fsm.main_state==JULIA_MAIN_STATE_S6_SLEEP);
    assert(julia_fsm_runtime_post_sync(EVT_WAKEUP)==ESP_OK);
    assert(julia_fsm_runtime_post_sync(EVT_INTENT_DISMISS)==ESP_OK);
    assert(julia_fsm_runtime_post_sync(EVT_NIGHT_TIME)==ESP_OK && s_fsm.main_state==JULIA_MAIN_STATE_S6_SLEEP);
    assert(julia_fsm_runtime_post_sync(EVT_WAKEUP)==ESP_OK);
    assert(julia_fsm_runtime_post_sync(EVT_INTENT_DISMISS)==ESP_OK);
    now=s_silent_deadline_us;runtime_check_deadlines();assert(s_fsm.main_state==JULIA_MAIN_STATE_S3_STANDBY);
    assert(julia_fsm_runtime_post_sync(EVT_OTA_AVAILABLE)==ESP_OK && s_fsm.main_state==JULIA_MAIN_STATE_S8_OTA);
    assert(julia_fsm_runtime_post_sync(EVT_OTA_TASK_FAILED)==ESP_OK && s_fsm.main_state==JULIA_MAIN_STATE_S3_STANDBY);
    assert(julia_fsm_runtime_post_sync(EVT_WSS_DISCONNECTED)==ESP_OK);
    assert(s_fsm.s7_sub_state==JULIA_S7_SUB_STATE_S7_1_DISCONNECTED);
    now=s_disconnect_deadline_us;disconnect_timer_callback(NULL); /* Queue is full. */
    runtime_check_deadlines();assert(s_fsm.main_state==JULIA_MAIN_STATE_S3_STANDBY);
    assert(!runtime_process_event(EVT_STANDBY_TIMEOUT)); /* Stale timeout cannot exit new S3. */
    /* Startup failures respect the initial connection deadline. */
    s_committed_service_state=JULIA_SERVICE_CONNECTING;s_online_links=0;
    s_service_deadline_us=now+30000000;
    runtime_process_event(EVT_MQTT_DISCONNECTED);
    assert(s_committed_service_state==JULIA_SERVICE_CONNECTING);
    runtime_process_event(EVT_MQTT_CONNECTED);
    mqtt_ready=false;wss_ready=true;service_state_reconcile();
    assert(s_online_links==SERVICE_LINK_WSS);
    assert(s_committed_service_state==JULIA_SERVICE_CONNECTING);
    now=s_service_deadline_us;runtime_check_deadlines();
    assert(s_committed_service_state==JULIA_SERVICE_OFFLINE);
    /* Opposite links recover/fail together: never combine stale ready bits. */
    mqtt_ready=true;wss_ready=false;service_state_reconcile();
    assert(s_online_links==SERVICE_LINK_MQTT);
    assert(s_committed_service_state==JULIA_SERVICE_OFFLINE);
    wss_ready=true;service_state_reconcile();
    assert(s_committed_service_state==JULIA_SERVICE_ONLINE);
    /* MQTT already offline must not suppress invalidation of a later WSS session. */
    now=s_disconnect_deadline_us;runtime_check_deadlines();
    runtime_process_event(EVT_WAKEUP);runtime_process_event(EVT_START_DIALOG);
    runtime_process_event(EVT_MULTI_TURN_DETECTED);runtime_process_event(EVT_SILENCE_TIMEOUT);
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S1_COMPANION);
    julia_fsm_snapshot_t cloud_snapshot;julia_fsm_runtime_get_snapshot(&cloud_snapshot);
    assert(cloud_snapshot.companion_remaining_ms==300000);
    assert(julia_fsm_runtime_require_wake(cloud_snapshot.revision-1)==ESP_ERR_INVALID_STATE);
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S1_COMPANION);
    wake_before_dispatch=true;
    assert(julia_fsm_runtime_require_wake(cloud_snapshot.revision)==ESP_ERR_INVALID_STATE);
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S4_INTERACTION);
    runtime_process_event(EVT_START_DIALOG);runtime_process_event(EVT_MULTI_TURN_DETECTED);
    runtime_process_event(EVT_SILENCE_TIMEOUT);
    julia_fsm_runtime_get_snapshot(&cloud_snapshot);
    assert(julia_fsm_runtime_require_wake(cloud_snapshot.revision)==ESP_OK);
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S3_STANDBY);
    runtime_process_event(EVT_WAKEUP);
    assert(julia_fsm_runtime_require_wake(s_committed_revision)==ESP_ERR_INVALID_STATE);
    runtime_process_event(EVT_START_DIALOG);runtime_process_event(EVT_MULTI_TURN_DETECTED);
    runtime_process_event(EVT_SILENCE_TIMEOUT);
    s_committed_service_state=JULIA_SERVICE_OFFLINE;s_online_links=SERVICE_LINK_WSS;
    runtime_process_event(EVT_WSS_DISCONNECTED);
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S3_STANDBY);
    /* New socket independently resets stale companion/dialog state. */
    runtime_process_event(EVT_WAKEUP);runtime_process_event(EVT_START_DIALOG);
    runtime_process_event(EVT_VOICE_SESSION_RESET);
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S3_STANDBY);
    runtime_process_event(EVT_WAKEUP);runtime_process_event(EVT_START_DIALOG);
    runtime_process_event(EVT_MULTI_TURN_DETECTED);runtime_process_event(EVT_SILENCE_TIMEOUT);
    int64_t companion_deadline=s_companion_deadline_us;
    assert(companion_deadline==now+300000000);
    assert(!runtime_process_event(EVT_USER_LEAVE)); /* Early/stale producer event. */
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S1_COMPANION);
    now=companion_deadline;runtime_check_deadlines(); /* No queued timeout available. */
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S3_STANDBY);
    /* Sleep and OTA must not be woken/aborted by session establishment. */
    runtime_process_event(EVT_NIGHT_TIME);runtime_process_event(EVT_VOICE_SESSION_RESET);
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S6_SLEEP);
    runtime_process_event(EVT_MOTION_WAKE);runtime_process_event(EVT_OTA_AVAILABLE);
    runtime_process_event(EVT_VOICE_SESSION_RESET);
    assert(s_fsm.main_state==JULIA_MAIN_STATE_S8_OTA);
    runtime_process_event(EVT_OTA_TASK_FAILED);
    /* Missing disconnect callback is applied even when posting always fails. */
    mqtt_ready=false;service_state_reconcile();
    assert(s_committed_service_state==JULIA_SERVICE_OFFLINE);
    assert(s_online_links==SERVICE_LINK_WSS);
    /* Missing recovery callbacks must clear a persistent offline state. */
    mqtt_ready=true;service_state_reconcile();
    assert(s_committed_service_state==JULIA_SERVICE_ONLINE);
    puts("PASS: FSM acknowledgements, OTA admission/exit, approved night edges and dropped timer recovery");return 0;
}
''', ["main/fsm/julia_fsm.c"])


def wifi_profiles(args):
    run_case(args, "wifi_profiles", r'''
#define CONFIG_NETWORK_WIFI_BACKUP_SSID "backup-test"
#define CONFIG_NETWORK_WIFI_BACKUP_PASSWORD "test-password"
#define CONFIG_NETWORK_WIFI_THIRD_SSID "third-test"
#define CONFIG_NETWORK_WIFI_THIRD_PASSWORD "third-password"
#define WIFI_IF_STA 0
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
typedef const char *esp_event_base_t;
typedef struct {int reason;} wifi_event_sta_disconnected_t;
typedef struct {struct {uint8_t ssid[32],password[64];bool bssid_set;unsigned channel,pmf;} sta;} wifi_config_t;
static wifi_config_t s_primary_wifi_config,applied;
static unsigned s_wifi_profile;
static bool s_rotate_wifi_profile,s_backup_wifi_available=true,s_ip_ready,s_connect_attempt_pending;
static bool s_third_wifi_available;
static uint32_t s_retry_attempt;
static int64_t s_next_retry_us,now;
static void *s_network_task=(void*)1;
static int esp_wifi_set_config(int interface,const wifi_config_t *config){applied=*config;return ESP_OK;}
static int64_t esp_timer_get_time(void){return now;}
static void xTaskNotifyGive(void *task){}
static uint32_t network_schedule_retry_locked(void){s_retry_attempt++;s_next_retry_us=now+1000000;return 1000;}
''', [("main/network/network_lifecycle.c", ["network_apply_wifi_profile", "network_wifi_event_handler"])], r'''
int main(void){
    strcpy((char*)s_primary_wifi_config.sta.ssid,"primary-test");
    strcpy((char*)s_primary_wifi_config.sta.password,"primary-password");
    s_primary_wifi_config.sta.pmf=123;s_primary_wifi_config.sta.bssid_set=true;s_primary_wifi_config.sta.channel=6;
    network_wifi_event_handler(NULL,NULL,WIFI_EVENT_STA_START,NULL);
    assert(network_apply_wifi_profile()==ESP_OK);
    assert(strcmp((char*)applied.sta.ssid,"primary-test")==0);
    network_wifi_event_handler(NULL,NULL,WIFI_EVENT_STA_DISCONNECTED,NULL);
    assert(network_apply_wifi_profile()==ESP_OK);
    assert(strcmp((char*)applied.sta.ssid,"backup-test")==0);
    assert(strcmp((char*)applied.sta.password,"test-password")==0);
    assert(applied.sta.pmf==123 && !applied.sta.bssid_set && applied.sta.channel==0);
    s_ip_ready=true; /* Connected backup remains preferred after the first drop. */
    network_wifi_event_handler(NULL,NULL,WIFI_EVENT_STA_DISCONNECTED,NULL);
    network_apply_wifi_profile();assert(s_wifi_profile==1);
    network_wifi_event_handler(NULL,NULL,WIFI_EVENT_STA_DISCONNECTED,NULL);
    network_apply_wifi_profile();assert(s_wifi_profile==0);
    assert(strcmp((char*)applied.sta.password,"primary-password")==0);
    s_rotate_wifi_profile=true; /* Watchdog plus delayed callback rotate only once. */
    network_wifi_event_handler(NULL,NULL,WIFI_EVENT_STA_DISCONNECTED,NULL);
    network_apply_wifi_profile();assert(s_wifi_profile==1);
    network_apply_wifi_profile();assert(s_wifi_profile==1);
    s_wifi_profile=0;s_backup_wifi_available=false;s_rotate_wifi_profile=true;
    network_apply_wifi_profile();assert(s_wifi_profile==0);
    s_backup_wifi_available=true;s_third_wifi_available=true;s_rotate_wifi_profile=true;
    network_apply_wifi_profile();assert(s_wifi_profile==1);
    s_rotate_wifi_profile=true;network_apply_wifi_profile();assert(s_wifi_profile==2);
    assert(strcmp((char*)applied.sta.ssid,"third-test")==0);
    assert(strcmp((char*)applied.sta.password,"third-password")==0);
    s_ip_ready=true;network_wifi_event_handler(NULL,NULL,WIFI_EVENT_STA_DISCONNECTED,NULL);
    network_apply_wifi_profile();assert(s_wifi_profile==2);
    network_wifi_event_handler(NULL,NULL,WIFI_EVENT_STA_DISCONNECTED,NULL);
    network_apply_wifi_profile();assert(s_wifi_profile==0);
    s_backup_wifi_available=false;s_rotate_wifi_profile=true;
    network_apply_wifi_profile();assert(s_wifi_profile==2); /* Skip empty backup. */
    puts("PASS: primary/backup rotation, successful-network retry and credential isolation");return 0;
}
''')


def offline_ui(args):
    run_case(args, "offline_ui", r'''
#define LV_OBJ_FLAG_HIDDEN 1
static void *s_offline_label=(void*)1;
static void *s_status_label=(void*)2;
static char s_status_text[32]="S1 COMPANION",caption[32]="S1 COMPANION";
static const char *lv_label_get_text(void *obj){return caption;}
static void lv_label_set_text(void *obj,const char *text){strcpy(caption,text);}
static void status_label_place(void){}
static bool s_offline,hidden=true,lock_ok=true;
static unsigned invalidations;
static bool lvgl_port_lock(unsigned ticks){return lock_ok;}
static void lvgl_port_unlock(void){}
static bool lv_obj_has_flag(void *obj,int flag){return hidden;}
static void lv_obj_clear_flag(void *obj,int flag){hidden=false;}
static void lv_obj_add_flag(void *obj,int flag){hidden=true;}
static void lv_obj_move_foreground(void *obj){}
static void lv_obj_invalidate(void *obj){invalidations++;}
''', [("main/ui/julia_avatar.c", ["offline_label_sync", "julia_avatar_set_offline",
    "status_label_sync", "julia_avatar_set_status_text"])], r'''
int main(void){
    julia_avatar_set_offline(true);assert(!hidden);
    lock_ok=false;julia_avatar_set_offline(false);
    assert(hidden==false && s_offline==false);
    lock_ok=true;offline_label_sync();assert(hidden);
    unsigned count=invalidations;offline_label_sync();assert(invalidations==count);
    lock_ok=false;julia_avatar_set_offline(true);assert(hidden);
    lock_ok=true;offline_label_sync();assert(!hidden);
    lock_ok=false;julia_avatar_set_status_text("S3 STANDBY");
    assert(strcmp(caption,"S1 COMPANION")==0);
    lock_ok=true;status_label_sync();assert(strcmp(caption,"S3 STANDBY")==0);
    lock_ok=false;julia_avatar_set_offline(false);julia_avatar_set_offline(true);
    lock_ok=true;offline_label_sync();assert(!hidden);
    puts("PASS: offline label retries failed UI locks and applies latest state");return 0;
}
''')


def boot(args):
    prefix = r'''
#define TAG "boot-test"
#define ESP_ERR_NVS_NO_FREE_PAGES 0x1101
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1102
#define ESP_ERR_OTA_ROLLBACK_FAILED 0x1501
#define JULIA_FAULT_FLASH_IO 1
#define JULIA_FAULT_OTA_ROLLBACK_UNAVAILABLE 2
#define JULIA_FAULT_NVS_UNRECOVERABLE 3
#define JULIA_FAULT_CRITICAL_INIT 4
#define NATIVE_OTA_FAILURE_BOOT_SELF_TEST_FAILED 1
#define NATIVE_OTA_FAILURE_ROLLBACK_UNAVAILABLE 2
#define ESP_ERROR_CHECK(e) assert((e)==ESP_OK)
typedef int native_ota_failure_reason_t;
static bool s_pending_verify,s_fault_nvs_ready,pending_image=true,product_ok=true,confirm_ok=true;
static unsigned confirmed,rejected,succeeded,reconciled;
static jmp_buf reboot;
static int ota_boot_health_begin(bool *pending){*pending=pending_image;return ESP_OK;}
static int nvs_flash_init(void){return ESP_OK;}
static int nvs_flash_erase(void){return ESP_OK;}
static int native_ota_report_init(void){return ESP_OK;}
static int esp_netif_init(void){return ESP_OK;}
static int esp_event_loop_create_default(void){return ESP_OK;}
static int native_ota_report_boot_pending_verify(void){return ESP_OK;}
static bool ota_local_health_check(bool pending){return true;}
static bool esp_ota_check_rollback_is_possible(void){return true;}
static int native_ota_report_boot_rolled_back(int reason){return ESP_OK;}
static int ota_boot_health_reject(const char *why){rejected++;longjmp(reboot,1);return ESP_FAIL;}
static void ota_enter_safe_mode(int reason,int err,const char *why){assert(0);}
static void ota_state_store_log_nvs_usage(const char *t,const char *o,int e){}
static bool ota_boot_health_product_check(void){return product_ok;}
static int ota_boot_health_confirm(void){confirmed++;return confirm_ok?ESP_OK:ESP_FAIL;}
static int native_ota_report_boot_succeeded(void){succeeded++;return ESP_OK;}
static void ota_reconcile_boot_state(bool pending){reconciled++;}
'''
    run_case(args, "boot_recovery", prefix, [("main/ota/ota_boot_flow.c", [
        "ota_boot_flow_run", "ota_boot_flow_complete"])], r'''
int main(void){
    ota_boot_flow_run();assert(s_pending_verify && confirmed==0 && succeeded==0);
    if(!setjmp(reboot)){ota_boot_flow_complete(false);assert(0);}
    assert(rejected==1 && confirmed==0 && succeeded==0);
    ota_boot_flow_run();ota_boot_flow_complete(true);
    assert(!s_pending_verify && confirmed==1 && succeeded==1);
    ota_boot_flow_complete(true);assert(confirmed==1);
    ota_boot_flow_run();product_ok=false;
    if(!setjmp(reboot)){ota_boot_flow_complete(true);assert(0);}
    assert(rejected==2 && confirmed==1);
    product_ok=true;confirm_ok=false;ota_boot_flow_run();
    if(!setjmp(reboot)){ota_boot_flow_complete(true);assert(0);}
    assert(rejected==3 && succeeded==1);
    pending_image=false;ota_boot_flow_run();ota_boot_flow_complete(false);assert(rejected==3);
    puts("PASS: boot never confirms before application health; application/product/confirm failures roll back");return 0;
}
''')


def storage(args):
    run_case(args, "audio_storage_recovery", r'''
#define AUDIO_ENGINE_CHECKPOINT_BYTES 16384U
typedef struct{size_t size,erase_size;} partition_t;
typedef struct{size_t file_size;} manifest_t;
typedef struct{uint32_t verified_offset;} record_t;
typedef struct{const manifest_t *manifest;const partition_t *partition;record_t *record;
    size_t offset,last_checkpoint,erased_until;} audio_engine_sink_ctx_t;
static unsigned char flash[8192];static bool erase_fails;
static int esp_partition_erase_range(const partition_t *p,size_t at,size_t n){
    if(erase_fails)return ESP_FAIL;assert(at%4096==0 && n%4096==0 && at+n<=sizeof(flash));memset(flash+at,255,n);return ESP_OK;
}
static int esp_partition_write(const partition_t *p,size_t at,const uint8_t *d,size_t n){
    assert(at+n<=sizeof(flash));for(size_t i=0;i<n;i++)flash[at+i]&=d[i];return ESP_OK;
}
static int audio_record_save(record_t *r){return ESP_OK;}
static void audio_record_init(record_t *r,const manifest_t *m){r->verified_offset=0;}
''', [("main/audio/audio_engine.c", ["audio_engine_sink", "audio_engine_restart"])], r'''
int main(void){
    partition_t p={8192,4096};manifest_t m={8192};record_t record={0};
    audio_engine_sink_ctx_t ctx={&m,&p,&record,0,0,0};uint8_t bytes[512];memset(bytes,0xa5,512);
    assert(audio_engine_sink(&ctx,bytes,512)==ESP_OK && flash[0]==0xa5 && ctx.erased_until==4096);
    memset(bytes,0xcc,512);assert(audio_engine_sink(&ctx,bytes,512)==ESP_OK && flash[0]==0xa5 && flash[512]==0xcc);
    assert(audio_engine_restart(&ctx)==ESP_OK);memset(bytes,255,512);
    assert(audio_engine_sink(&ctx,bytes,512)==ESP_OK && flash[0]==255);
    memset(flash,0xa5,4096);ctx.offset=ctx.erased_until=ctx.last_checkpoint=4096;
    memset(bytes,0xcc,512);assert(audio_engine_sink(&ctx,bytes,512)==ESP_OK && flash[0]==0xa5 && flash[4096]==0xcc);
    ctx.offset=ctx.erased_until=0;erase_fails=true;
    assert(audio_engine_sink(&ctx,bytes,512)==ESP_FAIL && ctx.offset==0);
    puts("PASS: audio replacement erases NOR sectors and resume preserves the committed prefix");return 0;
}
''')
    run_case(args, "routine_storage_recovery", r'''
#define ROUTINE_PATH "fake"
typedef struct{uint32_t generation,count,crc32;} routine_store_t;
static routine_store_t s_store={1,7,0};static bool s_dirty=true,inject=true;
static unsigned s_change_generation=1;static uint8_t s_active_slot;static int64_t s_last_flush_us;
static void *s_lock=(void*)1,*s_flush_lock=(void*)2;static unsigned data_locked,flush_locked;
static int xSemaphoreTake(void *s,unsigned w){if(s==s_lock)data_locked++;else flush_locked++;return 1;}
static void xSemaphoreGive(void *s){if(s==s_lock){assert(data_locked);data_locked--;}else{assert(flush_locked);flush_locked--;}}
static uint32_t store_crc(const routine_store_t *s){return s->generation+s->count;}
static int64_t esp_timer_get_time(void){return 100;}
static size_t fake_write(const void *p,size_t size,size_t n,FILE *f){
    assert(flush_locked && !data_locked);if(inject){inject=false;s_store.count++;s_change_generation++;s_dirty=true;}return size*n;
}
#define fopen(...) ((FILE*)1)
#define fseek(...) 0
#define fwrite fake_write
#define fflush(...) 0
#define fsync(...) 0
#define fileno(...) 0
#define fclose(...) 0
''', [("main/memory/julia_routine.c", ["write_snapshot"])], r'''
int main(void){
    assert(write_snapshot()==ESP_OK && s_store.count==8 && s_dirty && s_store.generation==2);
    assert(write_snapshot()==ESP_OK && s_store.count==8 && !s_dirty && s_store.generation==3);
    assert(!data_locked && !flush_locked);
    puts("PASS: concurrent activity survives snapshot commit and remains dirty until saved");return 0;
}
''')
    run_case(args, "memory_storage_recovery", r'''
#include <errno.h>
#define EVENT_PATH "fake"
typedef struct{int id;} julia_event_t;
typedef struct{bool clear;julia_event_t event;void *completed;esp_err_t *result;} memory_write_job_t;
static void *s_event_lock=(void*)1,*s_event_queue=(void*)2;
static int s_events[8],s_event_header_slot,stored;static memory_write_job_t jobs[8];static unsigned read_at,write_at;
static void memory_process_job(memory_write_job_t *job);
static void init_event_header(void){stored=0;}
static esp_err_t persist_event(const julia_event_t *event){s_events[stored++]=event->id;return ESP_OK;}
static void *xSemaphoreCreateBinary(void){return calloc(1,sizeof(int));}
static void vSemaphoreDelete(void *s){assert(*(int*)s);free(s);}
static int xQueueSend(void *q,const void *p,unsigned t){jobs[write_at++]=*(const memory_write_job_t*)p;return 1;}
static void xSemaphoreGive(void *s){if(s!=s_event_lock)*(int*)s=1;}
static int xSemaphoreTake(void *s,unsigned t){if(s==s_event_lock)return 1;
    while(!*(int*)s){assert(read_at<write_at);memory_process_job(&jobs[read_at++]);}return 1;}
#define remove(...) 0
''', [("main/memory/julia_memory.c", ["clear_events_owned", "memory_process_job", "julia_memory_forget_all"])], r'''
int main(void){
    jobs[write_at++]=(memory_write_job_t){.event={1}};
    assert(julia_memory_forget_all()==ESP_OK && stored==0 && read_at==write_at);
    memory_write_job_t next={.event={2}};memory_process_job(&next);
    assert(stored==1 && s_events[0]==2);
    puts("PASS: clear executes after prior queued writes; old events cannot reappear after acknowledgement");return 0;
}
''')


def fault(args):
    run_case(args, "fault_recovery", r'''
#include "julia_fault.h"
#define CONFIG_JULIA_FAULT_QUICK_UPTIME_SECONDS 60
#define CONFIG_JULIA_FAULT_AUTO_RESET_LIMIT 3
#define FAULT_SCHEMA_VERSION 1
#define FAULT_NAMESPACE "fault"
#define FAULT_RECORD_KEY "last"
#define MALLOC_CAP_8BIT 1
#define NVS_READWRITE 1
typedef int nvs_handle_t;
typedef struct{char version[32];} esp_app_desc_t;
static esp_app_desc_t app={"0.1.0"};static julia_fault_record_t last;static bool valid;static int64_t now;
esp_err_t julia_fault_read_last(julia_fault_record_t *r){if(!valid)return ESP_ERR_NOT_FOUND;*r=last;return ESP_OK;}
static const esp_app_desc_t *esp_app_get_description(void){return &app;}
static int64_t esp_timer_get_time(void){return now;}
static unsigned heap_caps_get_free_size(int caps){return 99999;}
static int esp_reset_reason(void){return 0;}
static int nvs_open(const char *n,int m,nvs_handle_t *h){*h=1;return ESP_OK;}
static int nvs_set_blob(nvs_handle_t h,const char *k,const void *r,size_t n){assert(n==sizeof(last));memcpy(&last,r,n);valid=true;return ESP_OK;}
static int nvs_commit(nvs_handle_t h){return ESP_OK;}
static void nvs_close(nvs_handle_t h){}
''', [("main/fsm/julia_fault.c", ["julia_fault_record", "julia_fault_reset_allowed"])], r'''
static void record(void){assert(julia_fault_record(JULIA_FAULT_AUDIO_INIT,ESP_FAIL,JULIA_MAIN_STATE_S3_STANDBY,JULIA_S2_SUB_STATE_NONE)==ESP_OK);}
int main(void){
    now=1000000;for(unsigned i=0;i<4;i++)record();assert(last.repeat_count==4 && !julia_fault_reset_allowed());
    now=6LL*3600*1000000;record();assert(last.repeat_count==1 && julia_fault_reset_allowed());
    now=1000000;record();record();assert(last.repeat_count==2);
    strcpy(app.version,"0.2.0");record();assert(last.repeat_count==1);
    now=((int64_t)UINT32_MAX+10000)*1000;record();assert(last.uptime_ms==UINT32_MAX && last.repeat_count==1);
    puts("PASS: healthy uptime and firmware change reset quick-fault chains; uptime does not wrap");return 0;
}
''', ["main/fsm/julia_fsm.c"])


def voice(args):
    prefix = r'''
#include "julia_fsm.h"
#define CONFIG_JULIA_SERVER_WAKE_ENABLE 1
#define CONFIG_JULIA_CLOUD_STATE_SYNC_ENABLE 1
#define CONFIG_JULIA_DIALOG_REPLY_TIMEOUT_SECONDS 30
#define CONFIG_JULIA_DIALOG_LISTEN_TIMEOUT_SECONDS 60
#define CONFIG_JULIA_SPEAKER_VOLUME_PERCENT 50
#define VOICE_SERVICE_URI_MAX_LEN 256
#define VOICE_INTERACTION_ID_MAX_LEN 64
typedef enum{VOICE_PLAYBACK_ROLE_NONE,VOICE_PLAYBACK_ROLE_WAKE_REPLY,VOICE_PLAYBACK_ROLE_DIALOG_REPLY,VOICE_PLAYBACK_ROLE_SELF_TEST,VOICE_PLAYBACK_ROLE_DISMISS_REPLY,VOICE_PLAYBACK_ROLE_GOODNIGHT_REPLY} voice_playback_role_t;
static julia_fsm_t fsm;
static bool busy,playing,s_mic_streaming,s_dialog_listening,s_wake_reply_expected;
static bool s_s4_ready_pending,s_s4_ready_committed,fail_send;
static char s_interaction_id[64];
static julia_main_state_t s_interaction_origin;
static uint32_t s_playback_generation;
static voice_playback_role_t s_playback_role;
static unsigned starts,sends,async_posts;
static bool cloud_ready=true;
static bool voice_state_sync_is_ready(void){return cloud_ready;}
static bool voice_state_sync_handle_text(const uint8_t *t,size_t n){return false;}
static int64_t now,s_reply_deadline_us,s_listen_deadline_us;
static unsigned session_failures;
static int64_t esp_timer_get_time(void){return now;}
static void wss_transport_fail_session(void){session_failures++;}
static void voice_service_on_fsm_state(julia_main_state_t m,julia_s2_sub_state_t s,fsm_event_t e,void *ctx);
static void voice_test_enter(julia_fsm_t *f,julia_main_state_t m,julia_s2_sub_state_t s,fsm_event_t e){voice_service_on_fsm_state(m,s,e,NULL);}
static julia_main_state_t julia_fsm_runtime_get_state(void){return fsm.main_state;}
static julia_s2_sub_state_t julia_fsm_runtime_get_s2_sub_state(void){return fsm.s2_sub_state;}
static esp_err_t julia_fsm_runtime_post(fsm_event_t e){async_posts++;return ESP_OK;}
static esp_err_t julia_fsm_runtime_post_sync(fsm_event_t e){return julia_fsm_handle_event(&fsm,e,NULL)?ESP_OK:ESP_ERR_INVALID_STATE;}
static void julia_idle_display_note_activity(void){}
static void julia_idle_display_set_busy(bool b){busy=b;}
static void voice_service_disarm_companion_timer(void){}
static void voice_service_cancel_file(void){}
static void julia_avatar_talking_start(void){}
static void julia_avatar_talking_stop(void){}
static void board_audio_mic_wake(void){}
static void board_audio_enable_wss_mic(bool b){}
static void board_audio_speaker_set_volume(uint8_t b){}
static bool voice_playback_is_active(void){return playing;}
static void voice_playback_stop(void){playing=false;}
static void voice_playback_finish(void){playing=false;}
static esp_err_t voice_playback_start(uint32_t r,bool t,uint32_t *g){if(r!=16000&&r!=24000)return ESP_FAIL;*g=++starts;playing=true;return ESP_OK;}
/* JSON parsing is the boundary stub; admission and handshake use firmware code. */
typedef struct {const char *valuestring;} cJSON;
static cJSON json_root={0},json_type={"wake_detected"},json_id={"test-id"};
static cJSON *cJSON_ParseWithLength(const char *t,size_t n){return &json_root;}
static bool cJSON_IsObject(const cJSON *j){return j==&json_root;}
static bool cJSON_IsString(const cJSON *j){return j && j->valuestring;}
static const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *j,const char *key){return strcmp(key,"type")==0?&json_type:&json_id;}
static void cJSON_Delete(cJSON *j){}
static esp_err_t voice_service_send_error(const char *s){return ESP_OK;}
static esp_err_t voice_service_push_file(const char *s,bool queued){return ESP_OK;}
static esp_err_t wss_transport_send_now(uint8_t op,const uint8_t *s,size_t n){
    assert(op==1 && strstr((const char*)s,"state_ready") && strstr((const char*)s,"test-id"));
    sends++;return fail_send?ESP_FAIL:ESP_OK;
}
'''
    run_case(args, "voice_recovery", prefix, [("main/voice/voice_service.c", [
        "post_fsm_event", "playback_role_is_terminal", "voice_service_speaker_done",
        "voice_service_apply_mic_start", "voice_service_apply_mic_stop",
        "interaction_id_is_valid", "voice_service_handle_wake_json",
        "voice_service_on_server_text", "voice_service_on_fsm_state", "voice_service_state_ready_poll",
        "voice_service_reply_timeout_poll"])], r'''
int main(void){
    julia_fsm_init(&fsm);fsm.on_enter=voice_test_enter;
    julia_fsm_transition_to(&fsm,JULIA_MAIN_STATE_S3_STANDBY,JULIA_S2_SUB_STATE_NONE,EVT_NONE);
    julia_fsm_handle_event(&fsm,EVT_WAKEUP,NULL);
    voice_service_apply_mic_start();voice_service_apply_mic_stop();
    assert(s_reply_deadline_us==30000000);
    cloud_ready=false;
    voice_service_on_server_text((const uint8_t*)"SPKS 16000",10);
    assert(starts==0 && fsm.s2_sub_state==JULIA_S2_SUB_STATE_S2_2_THINKING);
    cloud_ready=true;
    voice_service_on_server_text((const uint8_t*)"SPKS 16000",10);
    assert(s_reply_deadline_us==0);
    assert(starts==1 && fsm.s2_sub_state==JULIA_S2_SUB_STATE_S2_3_SPEAKING && async_posts==0);
    fsm.main_state=JULIA_MAIN_STATE_S8_OTA;s_dialog_listening=false;busy=false;
    voice_service_apply_mic_start();assert(!s_dialog_listening && !busy);
    fsm.main_state=JULIA_MAIN_STATE_S4_INTERACTION;fsm.s2_sub_state=JULIA_S2_SUB_STATE_NONE;
    strcpy(s_interaction_id,"test-id");s_s4_ready_pending=true;
    voice_service_state_ready_poll();assert(sends==0);
    voice_service_on_fsm_state(fsm.main_state,fsm.s2_sub_state,EVT_WAKEUP,NULL);
    fail_send=true;voice_service_state_ready_poll();assert(s_s4_ready_pending && sends==1);
    fail_send=false;voice_service_state_ready_poll();assert(!s_s4_ready_pending && sends==2);
    voice_service_state_ready_poll();assert(sends==2);
    /* A healthy socket without an answer must not keep THINKING forever. */
    now=1000000;
    voice_service_on_fsm_state(JULIA_MAIN_STATE_S2_DIALOG,JULIA_S2_SUB_STATE_S2_2_THINKING,EVT_START_DIALOG,NULL);
    int64_t deadline=s_reply_deadline_us;assert(deadline==31000000);
    now=deadline-1;assert(!voice_service_reply_timeout_poll());
    assert(s_reply_deadline_us==deadline); /* Poll/heartbeat cannot extend it. */
    now=deadline;assert(voice_service_reply_timeout_poll());
    assert(session_failures==1 && s_reply_deadline_us==0);
    assert(!voice_service_reply_timeout_poll());assert(session_failures==1);
    /* Accepted reply cancels the deadline; a later turn gets a fresh one. */
    voice_service_on_fsm_state(JULIA_MAIN_STATE_S2_DIALOG,JULIA_S2_SUB_STATE_S2_2_THINKING,EVT_START_DIALOG,NULL);
    deadline=s_reply_deadline_us;
    voice_service_on_fsm_state(JULIA_MAIN_STATE_S2_DIALOG,JULIA_S2_SUB_STATE_S2_3_SPEAKING,EVT_MULTI_TURN_DETECTED,NULL);
    now=deadline+1;assert(!voice_service_reply_timeout_poll());
    voice_service_on_fsm_state(JULIA_MAIN_STATE_S2_DIALOG,JULIA_S2_SUB_STATE_S2_2_THINKING,EVT_START_DIALOG,NULL);
    assert(s_reply_deadline_us==now+30000000);
    voice_service_on_fsm_state(JULIA_MAIN_STATE_S3_STANDBY,JULIA_S2_SUB_STATE_NONE,EVT_NONE,NULL);
    now+=30000000;assert(!voice_service_reply_timeout_poll());
    voice_service_on_fsm_state(JULIA_MAIN_STATE_S4_INTERACTION,JULIA_S2_SUB_STATE_NONE,EVT_WAKEUP,NULL);
    deadline=s_listen_deadline_us;assert(deadline==now+60000000);
    now=deadline-1;assert(!voice_service_reply_timeout_poll());
    now=deadline;assert(voice_service_reply_timeout_poll());
    assert(!voice_service_reply_timeout_poll());
    voice_service_on_fsm_state(JULIA_MAIN_STATE_S2_DIALOG,JULIA_S2_SUB_STATE_S2_1_LISTENING,EVT_USER_CALL,NULL);
    deadline=s_listen_deadline_us;
    voice_service_on_fsm_state(JULIA_MAIN_STATE_S2_DIALOG,JULIA_S2_SUB_STATE_S2_2_THINKING,EVT_START_DIALOG,NULL);
    assert(s_listen_deadline_us==0 && s_reply_deadline_us==now+30000000);
    /* Cloud wake-required mode can recover a locally stale S1 and receive S4 ACK. */
    fsm.main_state=JULIA_MAIN_STATE_S1_COMPANION;fsm.s2_sub_state=JULIA_S2_SUB_STATE_NONE;
    assert(voice_service_handle_wake_json((const uint8_t*)"{}",2));
    assert(fsm.main_state==JULIA_MAIN_STATE_S4_INTERACTION);
    assert(s_s4_ready_pending && s_s4_ready_committed && s_wake_reply_expected);
    unsigned before=sends;voice_service_state_ready_poll();assert(sends==before+1);
    /* A duplicate wake while S4 is listening must not rearm the handshake. */
    assert(voice_service_handle_wake_json((const uint8_t*)"{}",2));
    assert(!s_s4_ready_pending);
    /* Reproduce COM9: dismiss prompt starts in S4, then cloud emits MIC_START.
     * The pending S5 transition must survive the echo/queued utterance. */
    s_playback_role=VOICE_PLAYBACK_ROLE_DISMISS_REPLY;s_playback_generation=99;
    s_dialog_listening=false;playing=true;busy=true;
    voice_service_apply_mic_start();
    assert(playing && !s_dialog_listening && s_playback_generation==99);
    voice_service_apply_mic_stop();assert(fsm.main_state==JULIA_MAIN_STATE_S4_INTERACTION);
    voice_service_speaker_done();assert(fsm.main_state==JULIA_MAIN_STATE_S5_SILENT && !busy);
    voice_service_apply_mic_start();assert(fsm.main_state==JULIA_MAIN_STATE_S5_SILENT && !s_dialog_listening);
    assert(voice_service_handle_wake_json((const uint8_t*)"{}",2));
    assert(fsm.main_state==JULIA_MAIN_STATE_S4_INTERACTION && s_s4_ready_pending);
    s_playback_role=VOICE_PLAYBACK_ROLE_GOODNIGHT_REPLY;s_playback_generation=100;
    s_dialog_listening=false;playing=true;
    voice_service_apply_mic_start();assert(s_playback_generation==100 && playing);
    voice_service_speaker_done();assert(fsm.main_state==JULIA_MAIN_STATE_S6_SLEEP);
    voice_service_apply_mic_start();assert(fsm.main_state==JULIA_MAIN_STATE_S6_SLEEP);
    assert(voice_service_handle_wake_json((const uint8_t*)"{}",2));
    voice_service_apply_mic_start();voice_service_apply_mic_stop();
    voice_service_on_server_text((const uint8_t*)"SPKS 16000",10);
    assert(fsm.s2_sub_state==JULIA_S2_SUB_STATE_S2_3_SPEAKING);
    voice_service_apply_mic_start();
    assert(fsm.s2_sub_state==JULIA_S2_SUB_STATE_S2_1_LISTENING && s_dialog_listening && !playing);
    puts("PASS: ordered MIC_STOP/SPKS, retained state_ready, bounded reply wait and deadline cancellation");return 0;
}
''', ["main/fsm/julia_fsm.c"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=("backlight", "rtc", "display", "mqtt", "fsm", "wifi_profiles", "offline_ui", "boot", "storage", "fault", "voice"), required=True)
    args = parser.parse_args()
    args.root = Path(__file__).resolve().parents[2]
    args.out.mkdir(parents=True, exist_ok=True)
    globals()[args.case](args)
