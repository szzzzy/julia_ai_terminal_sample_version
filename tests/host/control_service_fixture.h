/* Hardware/transport boundaries for the actual owner-side control functions. */
#include <inttypes.h>
#include "voice_control_guard.h"
#include "julia_fsm_runtime.h"
#define CONFIG_JULIA_MULTI_DEVICE_ENABLE 1
#define VOICE_SCOPED_CONTROL_MAX_LEN 768U
#define VOICE_JOB_SCOPED_CONTROL 7
#define VOICE_INTERACTION_ID_MAX_LEN 64
#define CONFIG_JULIA_SERVER_WAKE_ENABLE 1
#define CONFIG_JULIA_CLOUD_STATE_SYNC_ENABLE 1
#define CONFIG_JULIA_SPEAKER_VOLUME_PERCENT 50
#define VOICE_SERVICE_URI_MAX_LEN 128
typedef enum {VOICE_PLAYBACK_ROLE_NONE,VOICE_PLAYBACK_ROLE_WAKE_REPLY,VOICE_PLAYBACK_ROLE_DIALOG_REPLY,VOICE_PLAYBACK_ROLE_SELF_TEST,VOICE_PLAYBACK_ROLE_GOODNIGHT_REPLY,VOICE_PLAYBACK_ROLE_DISMISS_REPLY} voice_playback_role_t;
static voice_control_guard_t s_control_guard;
static char s_voice_device_id[32]="esp-001122334455",s_interaction_id[64];
static char session[64]="session-a",s_round_ack[512],s_round_request[64];
static bool s_round_pending,s_round_speech,s_dialog_listening,s_s4_ready_pending,s_s4_ready_committed,s_wake_reply_expected;
static julia_main_state_t s_interaction_origin;
static int64_t now=1000000,s_busy_until_us,s_reply_deadline_us,s_listen_deadline_us;
static voice_playback_role_t s_playback_role;
static uint32_t s_playback_generation;
static struct {uint32_t generation;char session[33],interaction[64];int64_t spks_us,first_pcm_us;bool output_reported;} s_audio_timing;
static FILE *s_file;
static unsigned sleeps,dismisses,starts,stops,files,enqueues,failures,acks;
static bool playing,paused,queue_full;
static uint8_t queued[768];static size_t queued_len;
static char last_status[512],last_wss[512];
static julia_main_state_t state=JULIA_MAIN_STATE_S3_STANDBY;
static julia_s2_sub_state_t sub=JULIA_S2_SUB_STATE_NONE;
static uint32_t revision=1;
static const char *voice_state_sync_session_id(void){return session;}
static int64_t esp_timer_get_time(void){return now;}
julia_main_state_t julia_fsm_runtime_get_state(void){return state;}
julia_s2_sub_state_t julia_fsm_runtime_get_s2_sub_state(void){return sub;}
void julia_fsm_runtime_get_snapshot(julia_fsm_snapshot_t *s){memset(s,0,sizeof(*s));s->revision=revision;s->main_state=state;s->s2_sub_state=sub;}
static void post_fsm_event(fsm_event_t e){++revision;if(e==EVT_WAKEUP)state=JULIA_MAIN_STATE_S4_INTERACTION;else if(e==EVT_MULTI_TURN_DETECTED){state=JULIA_MAIN_STATE_S2_DIALOG;sub=JULIA_S2_SUB_STATE_S2_3_SPEAKING;}else if(e==EVT_VOICE_BUSY){state=JULIA_MAIN_STATE_S3_STANDBY;sub=0;}}
static bool playback_role_is_terminal(voice_playback_role_t r){return r==VOICE_PLAYBACK_ROLE_GOODNIGHT_REPLY || r==VOICE_PLAYBACK_ROLE_DISMISS_REPLY;}
static void voice_service_apply_terminal_intent(fsm_event_t e,const char *intent){if(e==EVT_INTENT_GOODNIGHT){sleeps++;s_playback_role=VOICE_PLAYBACK_ROLE_GOODNIGHT_REPLY;}else{dismisses++;s_playback_role=VOICE_PLAYBACK_ROLE_DISMISS_REPLY;}playing=true;s_dialog_listening=false;}
static void voice_service_apply_mic_start(void){starts++;s_dialog_listening=true;}
static void voice_service_apply_mic_stop(void){stops++;s_dialog_listening=false;}
static esp_err_t voice_service_push_file(const char *uri,bool q){assert(!strcmp(uri,"SD:/sample.wav"));s_file=(FILE*)1;files++;return ESP_OK;}
static bool voice_playback_is_active(void){return playing;}
static bool voice_playback_stop_generation(uint32_t g){playing=false;return true;}
static void voice_service_pause_uplink_for_file(void){paused=true;}
static void voice_service_resume_uplink_after_file(void){paused=false;}
static void julia_avatar_talking_stop(void){}
static void julia_avatar_talking_start(void){}
static void board_audio_mic_wake(void){}
static void voice_service_cancel_file(void){}
static void voice_service_disarm_companion_timer(void){}
static bool voice_state_sync_is_ready(void){return true;}
static bool voice_state_sync_handle_text(const uint8_t *s,size_t n){return false;}
static esp_err_t voice_service_send_error(const char *e){return ESP_OK;}
static esp_err_t voice_playback_start(uint32_t r,bool test,uint32_t *g){if(r!=16000 && r!=24000)return ESP_ERR_INVALID_ARG;playing=true;*g=++s_playback_generation;return ESP_OK;}
static void voice_playback_finish(void){playing=false;}
static void julia_idle_display_set_busy(bool b){}
static void julia_idle_display_note_activity(void){}
static void wss_transport_fail_session(void){failures++;}
static esp_err_t wss_transport_send_now(uint8_t op,const uint8_t *data,size_t n){assert(n<sizeof(last_wss));memcpy(last_wss,data,n);last_wss[n]=0;return ESP_OK;}
static esp_err_t mqtt_comm_publish_voice_status(const char *device,const char *data,size_t n){assert(!strcmp(device,s_voice_device_id)&&n<sizeof(last_status));memcpy(last_status,data,n);last_status[n]=0;acks++;return ESP_OK;}
static esp_err_t voice_service_enqueue(int type,const uint8_t *data,size_t n){assert(type==VOICE_JOB_SCOPED_CONTROL);if(queue_full)return ESP_ERR_NO_MEM;assert(n<=sizeof(queued));memcpy(queued,data,n);queued_len=n;enqueues++;return ESP_OK;}
