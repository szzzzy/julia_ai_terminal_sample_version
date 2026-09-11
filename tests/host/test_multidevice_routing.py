"""Exercise current MQTT ingress and owner-side dispatch with simulated devices."""
import argparse
from pathlib import Path
import subprocess
from test_recovery_paths import COMMON, function

p = argparse.ArgumentParser()
p.add_argument("--cc", required=True)
p.add_argument("--cjson", required=True)
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.out.mkdir(parents=True, exist_ok=True)
source = (root / "main/voice/voice_service.c").read_text(encoding="utf-8")
prefix = '#include "control_service_fixture.h"\n'
main = r'''
static void barrier(unsigned seq,const char *iid,const char *purpose){
 char msg[512];snprintf(msg,sizeof(msg),"{\"type\":\"interaction_sync\",\"device_id\":\"%s\",\"session_id\":\"%s\",\"interaction_id\":\"%s\",\"request_id\":\"round-%u\",\"interaction_seq\":%u,\"purpose\":\"%s\"}",s_voice_device_id,session,iid,seq,seq,purpose);
 assert(voice_service_handle_control_json((uint8_t*)msg,strlen(msg)));
}
static void receive(unsigned seq,const char *business){
 char msg[768];snprintf(msg,sizeof(msg),"{\"device_id\":\"%s\",\"session_id\":\"%s\",\"interaction_id\":\"%s\",\"request_id\":\"r-%u-%u\",\"interaction_seq\":%u,\"control_seq\":%u,\"expires_at_ms\":9000,%s}",s_voice_device_id,session,s_interaction_id,s_control_guard.interaction_seq,seq,s_control_guard.interaction_seq,seq,business);
 voice_service_on_mqtt_command(msg,strlen(msg));
}
static void execute(void){voice_service_apply_scoped_control(queued,queued_len);}
int main(void){
 s_control_guard.active=true;
 receive(1,"\"type\":\"command\",\"command\":\"FILE_SEND SD:/sample.wav\"");execute();assert(files==1 && strstr(last_status,"\"accepted\":true"));s_file=NULL;
 barrier(1,"wake-1","wake");assert(strstr(last_wss,"\"accepted\":true"));
 char first[512];strcpy(first,last_wss);barrier(1,"wake-1","wake");assert(!strcmp(first,last_wss));
 state=JULIA_MAIN_STATE_S4_INTERACTION;s_round_pending=false;
 const char *normal="\"type\":\"intent_result\",\"intent\":\"normal\"";
 const char *goodnight="\"type\":\"intent_result\",\"intent\":\"goodnight\"";
 receive(1,goodnight);assert(sleeps==0);execute();assert(sleeps==1 && strstr(last_status,"control_ack"));
 strcpy(first,last_status);execute();assert(sleeps==1 && !strcmp(first,last_status));
 barrier(2,"turn-2","speech");assert(strstr(last_wss,"terminal_reply"));
 s_playback_role=VOICE_PLAYBACK_ROLE_NONE;playing=false;
 barrier(2,"turn-2","speech");assert(strstr(last_wss,"\"accepted\":true"));
 execute();assert(strstr(last_status,"stale_interaction") && sleeps==1);
 for(unsigned i=1;i<=50;++i){receive(i,normal);execute();assert(strstr(last_status,"\"accepted\":true"));}
 assert(s_control_guard.used==VOICE_CONTROL_CACHE_SIZE && s_control_guard.high_water==50);
 receive(51,goodnight);now=10000000;execute();assert(strstr(last_status,"expired") && sleeps==1);
 now=1000000;receive(52,goodnight);strcpy(session,"session-new");execute();assert(sleeps==1);
 strcpy(session,"session-a");const char *busymsg="{\"type\":\"busy\",\"device_id\":\"esp-001122334455\",\"session_id\":\"session-a\",\"interaction_id\":\"turn-2\",\"request_id\":\"busy-1\",\"interaction_seq\":2,\"code\":\"voice_capacity\",\"retry_after_ms\":1000}";
 assert(voice_service_handle_control_json((const uint8_t*)busymsg,strlen(busymsg)));
 assert(state==JULIA_MAIN_STATE_S3_STANDBY && paused && !s_control_guard.active && s_busy_until_us==2000000);
 now=1999000;assert(voice_service_busy_wait() && paused);now=2000000;assert(!voice_service_busy_wait() && !paused && !s_busy_until_us);
 unsigned count=enqueues;queue_full=true;receive(53,normal);assert(enqueues==count);
 assert(!failures);puts("PASS: actual round barrier/control ACK dispatch, duplicate result, 50 controls, stale queue, expiry and real busy event routing");return 0;
}
'''
code = a.out / "routing.c"
exe = a.out / "routing.exe"
code.write_text(COMMON + prefix + function(source, "voice_service_busy_wait") + "\n" + function(source, "voice_service_handle_control_json") + "\n" + function(source, "voice_service_apply_scoped_control") + "\n" +
                function(source, "voice_service_on_mqtt_command") + main, encoding="utf-8")
subprocess.run([a.cc, "-I"+str(root/"tests/host"), "-I"+str(root/"tests/host/stubs"), "-I"+str(root/"main/fsm"), "-I"+str(root/"main/voice"),
                "-I"+a.cjson, str(code), str(root/"main/voice/voice_control_guard.c"),
                str(Path(a.cjson)/"cJSON.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)

# Compile the actual credential selectors in all supported non-certificate modes.
mqtt = (root / "main/network/mqtt_comm.c").read_text(encoding="utf-8")
wss = (root / "main/voice/wss_transport.c").read_text(encoding="utf-8")
for mode in ("NONE", "USERNAME_PASSWORD", "TOKEN"):
    fixture = COMMON + r'''
#define CONFIG_JULIA_MULTI_DEVICE_ENABLE 1
#define CONFIG_COMM_DEVICE_AUTH_TOKEN_VALUE ""
#define CONFIG_COMM_DEVICE_AUTH_TOKEN_USERNAME "fixture-user"
#define CONFIG_COMM_DEVICE_AUTH_USERNAME ""
#define CONFIG_COMM_DEVICE_AUTH_PASSWORD ""
#define CONFIG_COMM_MQTT_USERNAME "legacy-fixture"
#define CONFIG_COMM_MQTT_PASSWORD "legacy-fixture"
#define CONFIG_WSS_TOKEN "legacy-fixture"
''' + "#define CONFIG_COMM_DEVICE_AUTH_" + mode + " 1\n"
    fixture += function(mqtt, "mqtt_get_device_auth") + "\n" + function(wss, "wss_token")
    fixture += r'''
int main(void){const char *u,*p,*c,*k;assert(mqtt_get_device_auth(&u,&p,&c,&k)!=ESP_OK);
assert(wss_token()[0]==0);puts("PASS: strict credential selection rejects anonymous/missing credentials without legacy fallback");return 0;}
'''
    code.write_text(fixture, encoding="utf-8")
    subprocess.run([a.cc,"-I"+str(root/"tests/host/stubs"),str(code),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
