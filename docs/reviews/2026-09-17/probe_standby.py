"""Probe offline wake admission and failed MQTT restart without hardware."""
import importlib.util
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('recovery', ROOT/'tests/host/test_recovery_paths.py')
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)
OUT = ROOT/'build-review-20260917'
CC = ROOT/'build-ota-name/host-tools/tcc/tcc.exe'


def run(name, source, extra=()):
    OUT.mkdir(exist_ok=True)
    path = OUT/(name+'.c')
    exe = OUT/(name+'.exe')
    path.write_text(source, encoding='utf-8')
    subprocess.run([str(CC), str(path), '-o', str(exe), *map(str, extra)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    print(result.stdout.strip())


def block(source, condition):
    start = source.index(condition)
    brace = source.index('{', start)
    wrapped = recovery.function('static void selected(void) '+source[brace:], 'selected')
    return condition+' '+wrapped[wrapped.index('{'):]


def main():
    runtime = (ROOT/'main/behavior/julia_fsm_runtime.c').read_text(encoding='utf-8')
    source = r'''
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include "julia_fsm.h"
#define CONFIG_JULIA_LOCAL_CAPTURE_ENABLE 1
#define JULIA_SERVICE_ONLINE 2
static julia_fsm_t fsm;
static bool mqtt=true,wss=true,sync_ready=true;
static julia_main_state_t julia_fsm_runtime_get_state(void){return fsm.main_state;}
static int julia_fsm_runtime_get_service_state(void){return mqtt&&wss&&sync_ready?2:0;}
static bool mqtt_comm_is_ready(void){return mqtt;}
static bool wss_transport_is_ready(void){return wss;}
static bool voice_state_sync_is_ready(void){return sync_ready;}
'''
    source += recovery.function(runtime, 'interaction_event_allowed')
    source += r'''
int main(void){
 julia_fsm_init(&fsm);
 assert(julia_fsm_transition_to(&fsm,JULIA_MAIN_STATE_S3_STANDBY,JULIA_S2_SUB_STATE_NONE,EVT_NONE));
 assert(julia_fsm_handle_event(&fsm,EVT_STANDBY_TIMEOUT,0));
 assert(fsm.main_state==JULIA_MAIN_STATE_S6_SLEEP);
 mqtt=false;
 bool notice=julia_fsm_handle_event(&fsm,EVT_MQTT_DISCONNECTED,0);
 printf("S6 mqtt_offline: motion_allowed=%d voice_allowed=%d disconnect_transition=%d state=%s\n",
  interaction_event_allowed(EVT_MOTION_WAKE),interaction_event_allowed(EVT_WAKEUP),notice,julia_fsm_main_state_name(fsm.main_state));
 assert(!notice && !interaction_event_allowed(EVT_MOTION_WAKE) && !interaction_event_allowed(EVT_WAKEUP));
 mqtt=true;wss=false;sync_ready=false;
 assert(!interaction_event_allowed(EVT_MOTION_WAKE));
 printf("S6 wss_offline: motion_allowed=%d\n",interaction_event_allowed(EVT_MOTION_WAKE));
 wss=true;sync_ready=true;
 assert(interaction_event_allowed(EVT_MOTION_WAKE));
 assert(julia_fsm_handle_event(&fsm,EVT_MOTION_WAKE,0));
 printf("links_restored: motion_transitions_to=%s\n",julia_fsm_main_state_name(fsm.main_state));
 return 0;
}
'''
    run('standby_wake',source,['-DCONFIG_JULIA_LOCAL_CAPTURE_ENABLE=1',
        '-I'+str(ROOT/'main/behavior'),'-I'+str(ROOT/'tests/host/stubs'),ROOT/'main/behavior/julia_fsm.c'])
    mqtt_source = (ROOT/'main/network/mqtt_comm.c').read_text(encoding='utf-8')
    owner = recovery.function(mqtt_source,'mqtt_ota_check_task')
    source = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
typedef int esp_err_t;
typedef unsigned EventBits_t;
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define MQTT_OTA_READY_BIT 1
#define atomic_load(x) (*(x))
#define atomic_store(x,v) (*(x)=(v))
static bool s_power_stopped=false,s_reconnect_requested=true;
static bool was_ready=true;
static int64_t s_suback_deadline_ms=0;
static void *s_client=(void *)1;
static unsigned wait_ticks,start_calls;
static bool client_running=true;
static int esp_mqtt_client_stop(void *c){client_running=false;return ESP_OK;}
static int esp_mqtt_client_start(void *c){start_calls++;return -1;}
static void mqtt_release_cpu_boost(void){}
static int64_t esp_timer_get_time(void){return 3600000000LL;}
static const char *esp_err_to_name(int x){return "injected_failure";}
static void mqtt_request_reconnect(const char *s){s_reconnect_requested=true;}
int main(void){
 // Give the owner extra notifications too: a stopped client must still retry.
 for(unsigned iteration=0;iteration<3;iteration++){
'''
    source += block(owner,'if (atomic_load(&s_power_stopped) && s_client != NULL)')
    source += block(owner,'if (s_reconnect_requested)')
    source += '\nEventBits_t bits=0;\n'
    source += block(owner,'if ((bits & MQTT_OTA_READY_BIT) == 0)')
    source += r'''
 }
 printf("mqtt_restart_failure: start_calls=%u running=%d retry_flag=%d stopped_flag=%d wait_forever=%d\n",
 start_calls,client_running,s_reconnect_requested,s_power_stopped,wait_ticks==portMAX_DELAY);
 assert(start_calls==1 && !client_running && !s_reconnect_requested && !s_power_stopped && wait_ticks==portMAX_DELAY);
 return 0;
}
'''
    run('mqtt_restart_failure',source)


if __name__=='__main__':
    main()
