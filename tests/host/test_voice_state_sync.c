#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "voice_state_sync.h"
#include "julia_fsm_runtime.h"

static int64_t now;
static julia_fsm_snapshot_t snapshot;
static char sent[512];
static unsigned sends, failures, connected, requests, random_seed;
static bool send_fails;
static bool identity_fails;
esp_err_t mqtt_comm_publish_voice_status(const char *device, const char *data, size_t len)
{
    assert(!strcmp(device,"esp-001122334455"));
    assert(len < 512 && strstr(data,"device_state"));
    return ESP_OK;
}
esp_err_t native_ota_get_device_id(char *id, size_t size)
{
    if (identity_fails) return ESP_FAIL;
    assert(size >= sizeof("esp-001122334455"));
    strcpy(id, "esp-001122334455");
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return now; }
void esp_fill_random(void *buf, size_t len) { memset(buf, ++random_seed, len); }
void julia_fsm_runtime_get_snapshot(julia_fsm_snapshot_t *out) { *out = snapshot; }
esp_err_t wss_transport_send_now(uint8_t op, const uint8_t *data, size_t len)
{
    assert(op == 1 && len < sizeof(sent));
    ++sends;
    memcpy(sent, data, len); sent[len] = 0;
    return send_fails ? ESP_FAIL : ESP_OK;
}
void wss_transport_fail_session(void) { ++failures; }
void wss_transport_defer_retry(unsigned seconds) { assert(seconds==30 || seconds==60); }
esp_err_t julia_fsm_runtime_post_sync(fsm_event_t event)
{
    assert(event == EVT_WSS_CONNECTED); ++connected; return ESP_OK;
}
esp_err_t julia_fsm_runtime_require_wake(uint32_t revision)
{
    ++requests;
    if (snapshot.revision != revision) return ESP_ERR_INVALID_STATE;
    if (snapshot.main_state == JULIA_MAIN_STATE_S1_COMPANION) {
        julia_fsm_t fsm;
        julia_fsm_init(&fsm);
        fsm.main_state = snapshot.main_state;
        assert(julia_fsm_handle_event(&fsm, EVT_REQUIRE_WAKE, NULL));
        snapshot.main_state = fsm.main_state;
        snapshot.companion_remaining_ms = 0;
        snapshot.reason = EVT_REQUIRE_WAKE;
        ++snapshot.revision;
        return ESP_OK;
    }
    return snapshot.main_state == JULIA_MAIN_STATE_S3_STANDBY ||
           snapshot.main_state == JULIA_MAIN_STATE_S5_SILENT ||
           snapshot.main_state == JULIA_MAIN_STATE_S6_SLEEP ? ESP_OK : ESP_ERR_INVALID_STATE;
}
static void receive(const char *message)
{ assert(voice_state_sync_handle_text((const uint8_t *)message, strlen(message))); }
static void session_id(char *id)
{
    cJSON *j = cJSON_Parse(sent); assert(j);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(j,"session_id");
    assert(cJSON_IsString(value) && strlen(value->valuestring)==32);
    strcpy(id,value->valuestring); cJSON_Delete(j);
}
static void ack(const char *type,const char *id,uint32_t revision)
{
    char json[384];
    snprintf(json,sizeof(json),"{\"type\":\"%s\",\"device_id\":\"esp-001122334455\",\"request_id\":\"%s-%u\",\"session_id\":\"%s\",\"protocol_version\":2,\"control_protocol\":1,\"state_revision\":%u,\"accepted\":true}",type,strcmp(type,"session_sync_ack")==0?"session_sync":"device_state",(unsigned)revision,id,(unsigned)revision);
    receive(json);
}
static void require(const char *id,const char *request,uint32_t revision)
{
    char json[384];
    snprintf(json,sizeof(json),"{\"type\":\"require_wake\",\"device_id\":\"esp-001122334455\",\"interaction_id\":\"\",\"session_id\":\"%s\",\"request_id\":\"%s\",\"state_revision\":%u}",id,request,(unsigned)revision);
    receive(json);
}
int main(void)
{
    snapshot=(julia_fsm_snapshot_t){.main_state=JULIA_MAIN_STATE_S3_STANDBY,.revision=1};
    voice_state_sync_start(); assert(!voice_state_sync_is_ready() && sends==1);
    assert(strstr(sent,"session_sync") && strstr(sent,"\"wake_required\":true"));
    assert(strstr(sent,"\"device_id\":\"esp-001122334455\""));
    char id[33]; session_id(id);
#if CONFIG_JULIA_MULTI_DEVICE_ENABLE
    char wrong[384];
    snprintf(wrong,sizeof(wrong),"{\"type\":\"session_sync_ack\",\"device_id\":\"esp-556677889900\",\"session_id\":\"%s\",\"request_id\":\"session_sync-1\",\"protocol_version\":2,\"control_protocol\":1,\"state_revision\":1,\"accepted\":true}",id);
    receive(wrong);assert(!voice_state_sync_is_ready());
    snprintf(wrong,sizeof(wrong),"{\"type\":\"session_sync_ack\",\"device_id\":\"esp-001122334455\",\"session_id\":\"%s\",\"request_id\":\"old-request\",\"protocol_version\":2,\"control_protocol\":1,\"state_revision\":1,\"accepted\":true}",id);
    receive(wrong);assert(!voice_state_sync_is_ready());
#endif
    ack("session_sync_ack","old-session",1); assert(!voice_state_sync_is_ready());
    ack("session_sync_ack",id,2); assert(!voice_state_sync_is_ready());
    ack("session_sync_ack",id,1); assert(voice_state_sync_is_ready() && connected==1);
    ack("session_sync_ack",id,1); assert(connected==1);
    voice_state_sync_poll(); assert(strstr(sent,"device_state"));
    ack("device_state_ack",id,1);
    unsigned count=sends;now+=2000000;voice_state_sync_poll();assert(sends==count);
    snapshot.main_state=JULIA_MAIN_STATE_S1_COMPANION;snapshot.revision=2;snapshot.companion_remaining_ms=600000;
    voice_state_sync_poll(); assert(strstr(sent,"\"wake_required\":false"));
    assert(strstr(sent,"\"companion_remaining_ms\":600000"));
    ack("device_state_ack",id,1);now+=2000000;voice_state_sync_poll();assert(sends==count+2);
    ack("device_state_ack",id,2);
    require(id,"idle-1",1);assert(snapshot.main_state==JULIA_MAIN_STATE_S1_COMPANION && requests==0);
    assert(strstr(sent,"stale_state"));
    require(id,"idle-2",2);assert(snapshot.main_state==JULIA_MAIN_STATE_S3_STANDBY && requests==1);
    assert(strstr(sent,"\"accepted\":true"));
    char response[512];strcpy(response,sent);
    snapshot.main_state=JULIA_MAIN_STATE_S1_COMPANION;snapshot.revision=4;
    require(id,"idle-2",2);assert(requests==1 && strcmp(response,sent)==0);
    assert(snapshot.main_state==JULIA_MAIN_STATE_S1_COMPANION);
    snapshot.main_state=JULIA_MAIN_STATE_S2_DIALOG;snapshot.s2_sub_state=JULIA_S2_SUB_STATE_S2_1_LISTENING;snapshot.revision=5;
    require(id,"busy",5);assert(strstr(sent,"interaction_active") && strstr(sent,"\"sub_state\":\"S2.1\""));
    snapshot.main_state=JULIA_MAIN_STATE_S6_SLEEP;snapshot.s2_sub_state=JULIA_S2_SUB_STATE_NONE;snapshot.revision=6;
    require(id,"sleep",6);assert(snapshot.main_state==JULIA_MAIN_STATE_S6_SLEEP && strstr(sent,"\"accepted\":true"));
    /* Evicted IDs still cannot mutate a later turn with an old revision. */
    for(unsigned i=0;i<10;++i){char req[16];snprintf(req,sizeof(req),"reject-%u",i);require(id,req,0);}
    snapshot.main_state=JULIA_MAIN_STATE_S1_COMPANION;snapshot.revision=7;
    count=requests;require(id,"idle-2",2);assert(requests==count && strstr(sent,"stale_state"));
    voice_state_sync_end();assert(!voice_state_sync_is_ready());
    voice_state_sync_start();char next[33];session_id(next);assert(strcmp(id,next));
    assert(strstr(sent,"\"device_id\":\"esp-001122334455\""));
    count=sends;require(id,"late",7);assert(sends==count);
    now+=2000000;voice_state_sync_poll();now+=2000000;voice_state_sync_poll();
    assert(failures==0);now+=2000000;voice_state_sync_poll();assert(failures==1 && !voice_state_sync_is_ready());
    /* State ACK loss is bounded too, including a replacement snapshot. */
    voice_state_sync_start();session_id(next);ack("session_sync_ack",next,7);voice_state_sync_poll();
    snapshot.revision=8;voice_state_sync_poll();ack("device_state_ack",next,7);
    now+=2000000;voice_state_sync_poll();now+=2000000;voice_state_sync_poll();now+=2000000;voice_state_sync_poll();
    assert(failures==2 && !voice_state_sync_is_ready());
    send_fails=true;voice_state_sync_start();assert(failures==3 && !voice_state_sync_is_ready());
    send_fails=false;identity_fails=true;count=sends;
    voice_state_sync_start();assert(failures==4 && sends==count && !voice_state_sync_is_ready());
    puts("PASS: v2 handshake, revisions, state ACKs, stale sessions, bounded retries and idempotent require_wake");
    return 0;
}
