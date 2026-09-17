"""Execute production boot coordination and network/time gates with deterministic RTOS/I/O boundaries."""
import argparse
from pathlib import Path
from test_recovery_paths import run_case


def coordinator(args):
    source = (args.root / 'main/app/boot_coordinator.c').read_text(encoding='utf-8')
    header = (args.root / 'main/app/boot_coordinator.h').read_text(encoding='utf-8')
    source = '\n'.join(line for line in (header + '\n' + source).splitlines()
                       if not line.startswith(('#include', '#pragma')))
    prefix = r'''
#define pdPASS 1
typedef unsigned atomic_uint;
#define atomic_init(p,v) (*(p)=(v))
#define atomic_fetch_add(p,v) ((*(p)+=(v))-(v))
#define atomic_fetch_sub(p,v) ((*(p)-=(v))+(v))
static int64_t now;
static unsigned allocations, frees, queue_frees;
static void *tracked_calloc(size_t n,size_t s){++allocations;return calloc(n,s);}
static void tracked_free(void *p){++frees;free(p);}
typedef struct {unsigned size,count;unsigned char data[4][16];} queue_t;
typedef queue_t *QueueHandle_t;
static QueueHandle_t xQueueCreate(unsigned n,unsigned size){assert(n==4 && size<=16);queue_t *q=calloc(1,sizeof(*q));q->size=size;return q;}
static void vQueueDelete(QueueHandle_t q){++queue_frees;free(q);}
static int xQueueSend(QueueHandle_t q,const void *v,unsigned wait){assert(q && q->count<4);memcpy(q->data[q->count++],v,q->size);return pdTRUE;}
typedef struct {void (*run)(void*);void *arg;bool ran;} job_t;
static job_t jobs[3];
static unsigned created,order[3]={0,1,2},next;
static int fail_task=-1,critical=-1;
static bool timeout,immediate;
static unsigned executed;
static int xTaskCreate(void(*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,void *handle){
    unsigned i=created++;assert(priority==3 && stack>=4096);
    if((int)i==fail_task) {jobs[i].ran=true;return 0;}
    jobs[i]=(job_t){fn,arg,false};
    if(immediate){jobs[i].ran=true;fn(arg);}
    return pdPASS;
}
static void run_job(unsigned i){if(!jobs[i].ran){jobs[i].ran=true;jobs[i].run(jobs[i].arg);}}
static int xQueueReceive(QueueHandle_t q,void *out,unsigned wait){
    if(timeout){now+=20001000;return 0;}
    while(!q->count && next<3) run_job(order[next++]);
    if(!q->count)return 0;
    memcpy(out,q->data[0],q->size);--q->count;
    memmove(q->data[0],q->data[1],q->count*sizeof(q->data[0]));return pdTRUE;
}
static void vTaskDelete(void *task){}
static unsigned uxTaskGetStackHighWaterMark(void *task){return 2048;}
static int64_t esp_timer_get_time(void){return now;}
#define calloc tracked_calloc
#define free tracked_free
'''
    run_case(args, 'boot_coordinator', prefix+source, [], r'''
static unsigned callbacks,early_callbacks;
static esp_err_t ready_error;
static bool slow_ready;
static boot_result_t *observed;
static esp_err_t ready(void){++callbacks;if(!observed[0].completed)++early_callbacks;if(slow_ready)now+=20000000;return ready_error;}
static esp_err_t animation(void){return ESP_OK;}
static esp_err_t display(void){++executed;now+=3300000;return critical==0?ESP_FAIL:ESP_OK;}
static esp_err_t audio(void){++executed;now+=100000;return critical==1?ESP_FAIL:ESP_OK;}
static esp_err_t resources(void){++executed;now+=100000;return critical==2?ESP_FAIL:ESP_OK;}
static void reset(void){memset(jobs,0,sizeof(jobs));created=next=executed=callbacks=0;now=0;timeout=immediate=slow_ready=false;ready_error=ESP_OK;fail_task=critical=-1;}
int main(void){
    const boot_branch_t branches[3]={{"display",display,6144,animation},{"audio",audio,8192},{"resources",resources,4096}};
    boot_result_t results[3];observed=results;
    for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b)if(a!=b){
        reset();order[0]=a;order[1]=b;order[2]=3-a-b;
        assert(boot_coordinator_run(branches,results,20000,ready)==ESP_OK);
        for(unsigned i=0;i<3;++i)assert(results[i].completed && results[i].error==ESP_OK);
        assert(executed==3 && callbacks==1 && allocations==frees && queue_frees==frees);
    }
    assert(early_callbacks>0);
    for(int i=0;i<3;++i){
        reset();fail_task=i;
        assert(boot_coordinator_run(branches,results,20000,ready)==ESP_OK);
        assert(results[i].completed && results[i].error==ESP_ERR_NO_MEM);
        assert(executed==2 && callbacks==0 && allocations==frees);
        reset();critical=i;
        assert(boot_coordinator_run(branches,results,20000,ready)==ESP_OK);
        assert(results[i].completed && results[i].error==ESP_FAIL && callbacks==0);
    }
    reset();ready_error=ESP_FAIL;
    assert(boot_coordinator_run(branches,results,20000,ready)==ESP_FAIL);
    assert(callbacks==1 && allocations==frees);
    reset();slow_ready=true;
    assert(boot_coordinator_run(branches,results,20000,ready)==ESP_ERR_TIMEOUT);
    assert(callbacks==1 && allocations==frees);
    reset();timeout=true;
    assert(boot_coordinator_run(branches,results,20000,ready)==ESP_ERR_TIMEOUT);
    for(unsigned i=0;i<3;++i)assert(!results[i].completed);
    assert(allocations==frees+1); /* Late workers still own a valid queue/context. */
    for(unsigned i=0;i<3;++i)run_job(i);
    assert(allocations==frees && queue_frees==frees);
    reset();immediate=true;
    assert(boot_coordinator_run(branches,results,20000,ready)==ESP_OK);
    assert(executed==3 && allocations==frees); /* Worker finishes before create returns. */
    puts("PASS: six completion orders, each task-create/init failure, timeout/late completion, immediate worker and resource reclamation");
    return 0;
}
''')


def gates(args):
    prefix = r'''
#define CONFIG_JULIA_RTC_ENABLE 1
#define CONFIG_JULIA_TIMEZONE "CST-8"
#define CONFIG_JULIA_SNTP_SERVER "test"
#define atomic_load(p) (*(p))
static bool s_cloud_allowed,s_restore_complete,s_sntp_started,s_power_paused;
static int64_t now;
static unsigned mqtt_starts,voice_starts,sntp_starts,notifications,restores;
static bool rtc_present=true,restore_ok=true,sntp_ok=true;
static int64_t esp_timer_get_time(void){return now;}
static int mqtt_comm_ip_ready(void *p){++mqtt_starts;return ESP_OK;}
static int voice_service_ip_ready(void *p){++voice_starts;return ESP_OK;}
typedef int (*network_ip_ready_cb_t)(void *);
typedef struct {network_ip_ready_cb_t callback;void *arg;bool started_ok;int64_t retry_us;uint32_t attempt;} slot_t;
static slot_t s_slots[3];
static size_t s_slot_count=3;
static void *s_network_task=(void*)1;
static void xTaskNotifyGive(void *task){++notifications;}
static unsigned network_backoff_delay_ms(uint32_t *attempt){++*attempt;return 60000;}
static void network_lifecycle_retry_services(void);
static int julia_time_ip_ready(void *arg);
static int setenv(const char *a,const char *b,int c){return 0;}
static void tzset(void){}
static int board_rtc_init(void){assert(julia_time_ip_ready(NULL)==ESP_ERR_INVALID_STATE);return rtc_present?ESP_OK:ESP_FAIL;}
static int restore_system_time_from_rtc(void){++restores;assert(julia_time_ip_ready(NULL)==ESP_ERR_INVALID_STATE);now+=4000000;return restore_ok?ESP_OK:ESP_FAIL;}
typedef struct {void (*sync_cb)(void*);} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) {0}
static void sync_rtc_from_system(void *arg){}
static int esp_netif_sntp_init(const esp_sntp_config_t *c){assert(s_restore_complete);++sntp_starts;return sntp_ok?ESP_OK:ESP_FAIL;}
'''
    run_case(args, 'boot_gates', prefix, [
        ('main/app/main.c',['cloud_allowed','boot_mqtt_ip_ready','boot_voice_ip_ready']),
        ('main/network/network_lifecycle.c',['network_lifecycle_retry_services','network_dispatch_service_slots']),
        ('main/time/julia_time.c',['julia_time_init','julia_time_ip_ready'])], r'''
static void reset(void){
    s_cloud_allowed=s_restore_complete=s_sntp_started=false;
    mqtt_starts=voice_starts=sntp_starts=notifications=restores=0;now=0;
    s_slots[0]=(slot_t){.callback=boot_mqtt_ip_ready};
    s_slots[1]=(slot_t){.callback=boot_voice_ip_ready};
    s_slots[2]=(slot_t){.callback=julia_time_ip_ready};
}
int main(void){
    for(unsigned mode=0;mode<3;++mode){
        reset();rtc_present=mode!=1;restore_ok=mode!=2;
        /* Fast IP before any local branch completes: all services stay pending. */
        network_dispatch_service_slots();assert(mqtt_starts==0 && voice_starts==0 && sntp_starts==0);
        assert(julia_time_init()==ESP_OK && s_restore_complete && notifications==1);
        network_dispatch_service_slots();assert(sntp_starts==1 && !mqtt_starts && !voice_starts);
        assert(restores==(rtc_present?1:0));
        s_cloud_allowed=true;network_lifecycle_retry_services();network_dispatch_service_slots();
        assert(mqtt_starts==1 && voice_starts==1 && sntp_starts==1);
        network_lifecycle_retry_services();network_dispatch_service_slots();
        assert(mqtt_starts==1 && voice_starts==1 && sntp_starts==1);
    }
    reset();rtc_present=true;sntp_ok=false;julia_time_init();network_dispatch_service_slots();
    assert(!s_sntp_started && sntp_starts==1);sntp_ok=true;
    network_lifecycle_retry_services();network_dispatch_service_slots();assert(s_sntp_started && sntp_starts==2);
    puts("PASS: fast IP/cloud gates, slow/absent/failed RTC, immediate retry with existing IP, SNTP retry and single initialization");
    return 0;
}
''')

def branches(args):
    run_case(args, 'boot_branches', r'''
#define CONFIG_JULIA_IMU_MOTION_ENABLE 1
#define CONFIG_JULIA_SERVER_WAKE_ENABLE 0
#define CONFIG_JULIA_BOOT_BRIGHTNESS_PERCENT 50
static esp_err_t s_motion_prepared;
static int s_audio_fault,s_resources_fault;
#define JULIA_FAULT_AUDIO_INIT 1
#define JULIA_FAULT_VOICE_INIT 2
#define JULIA_FAULT_DISPLAY_INIT 3
#define JULIA_FAULT_FSM_RUNTIME_INIT 4
static int fail=-1;
static unsigned called,brightness;
static int invoke(unsigned i){called|=1U<<i;return fail==(int)i?ESP_FAIL:ESP_OK;}
static int julia_backlight_init(void){return invoke(0);}
static int julia_display_init(void){return invoke(1);}
static int julia_avatar_init(void){return invoke(2);}
static int julia_avatar_play_boot_sequence(void){return invoke(3);}
static void julia_backlight_set(unsigned n){brightness=n;}
static int board_audio_init(void){return invoke(4);}
static int voice_service_init_board_audio(void){return invoke(5);}
static int audio_service_init(void){return invoke(6);}
static int wake_detector_init(void){return invoke(7);}
static int julia_time_init(void){return invoke(8);}
static int julia_idle_display_init(void){return invoke(9);}
static int julia_fsm_runtime_prepare(void){return invoke(10);}
static int board_imu_init(void){return invoke(11);}
static int64_t esp_timer_get_time(void){return 0;}
''', [('main/app/main.c',['module_result','display_branch','animation_branch','audio_branch','resources_branch'])], r'''
int main(void){
    for(int i=0;i<12;++i){
        called=brightness=0;fail=i;
        esp_err_t result=i<3?display_branch():i==3?animation_branch():i<8?audio_branch():resources_branch();
        bool optional=i==3 || i==8 || i==11;
        assert(result==(optional?ESP_OK:ESP_FAIL));
        if(i==3)assert(brightness==50); /* A failed animation still has a working display. */
        if(i==4)assert(!(called&(1U<<5))); /* Wiring cannot run without board audio. */
        if(i==11)assert(s_motion_prepared==ESP_FAIL); /* No motion enable after partial probe. */
    }
    puts("PASS: critical display/audio/wake/idle/FSM errors, animation/RTC/IMU degradation and dependency ordering");return 0;
}
''')


def ota_admission(args):
    run_case(args, 'boot_ota_admission', r'''
#define atomic_load(p) (*(p))
#define pdPASS 1
#define EVT_OTA_AVAILABLE 1
#define EVT_OTA_TASK_FAILED 2
#define NATIVE_OTA_REPORT_DEFERRED 3
#define NATIVE_OTA_FAILURE_NONE 0
#define ESP_ERR_OTA_ROLLBACK_FAILED 0x1501
static bool s_pending_verify=true,s_ota_in_progress,rollback_available;
static unsigned parsed,invalidated;
typedef struct {int dummy;} ota_request_t;
typedef struct {int dummy;} native_ota_report_context_t;
typedef struct {char version[32];} esp_app_desc_t;
static int ota_control_plane_parse_server_response(const char *j,size_t n,ota_request_t *r,bool *d){++parsed;*d=false;return ESP_OK;}
static int julia_fsm_runtime_post_sync(int e){return ESP_OK;}
static const esp_app_desc_t *esp_app_get_description(void){return NULL;}
static int native_ota_report_context_init(void *c,const void *r,const char *v){return ESP_OK;}
static int native_ota_report_event(void *c,int s,int p,int e){return ESP_OK;}
static void ota_clear_in_progress(void){s_ota_in_progress=false;}
static void ota_engine_task(void *p){}
static int xTaskCreate(void(*f)(void*),const char *n,unsigned s,void *p,unsigned pri,void *h){return pdPASS;}
static bool esp_ota_check_rollback_is_possible(void){return rollback_available;}
static int esp_ota_mark_app_invalid_rollback_and_reboot(void){++invalidated;return ESP_FAIL;}
''', [('main/ota/ota_boot_flow.c',['ota_boot_flow_pending']),
         ('main/ota/ota_engine.c',['ota_engine_handle_server_json']),
         ('main/ota/ota_boot_health.c',['ota_boot_health_reject'])], r'''
int main(void){
    assert(ota_engine_handle_server_json("{}",2)==ESP_ERR_INVALID_STATE && parsed==0);
    s_pending_verify=false;
    assert(ota_engine_handle_server_json("{}",2)==ESP_OK && parsed==1);
    assert(ota_boot_health_reject("test")==ESP_ERR_OTA_ROLLBACK_FAILED && invalidated==0);
    rollback_available=true;
    assert(ota_boot_health_reject("test")==ESP_FAIL && invalidated==1);
    puts("PASS: pending-image OTA admission and no-rollback-image protection");return 0;
}
''')


def imu_partial(args):
    run_case(args, 'boot_imu_partial', r'''
#define ESP_RETURN_ON_FALSE(test,err,...) do{if(!(test))return err;}while(0)
#define QMI8658_ADDR_LOW 0x6b
#define QMI8658_ADDR_HIGH 0x6a
#define QMI8658_CTRL1 2
#define QMI8658_CTRL2 3
#define QMI8658_CTRL3 4
#define QMI8658_CTRL6 7
#define QMI8658_AUTO_INCREMENT 64
#define QMI8658_ACC_4G_30HZ 24
#define QMI8658_GYR_64DPS_30HZ 40
typedef void *i2c_master_bus_handle_t;
static void *s_dev;
static int fail_at;
static unsigned operations,probes,removed;
static int tca9554_init(void){return ESP_OK;}
static void *tca9554_i2c_bus(void){return (void*)1;}
static int try_address(void *bus,unsigned addr){++probes;s_dev=(void*)2;return ESP_OK;}
static int board_imu_set_enabled(bool on){assert(!on);return ++operations==fail_at?ESP_FAIL:ESP_OK;}
static int write_reg(unsigned reg,unsigned value){return ++operations==fail_at?ESP_FAIL:ESP_OK;}
static int i2c_master_bus_rm_device(void *dev){assert(dev==s_dev);++removed;return ESP_OK;}
''', [('main/hardware/qmi8658_shared.c',['board_imu_init'])], r'''
int main(void){
    for(int i=1;i<=5;++i){
        s_dev=NULL;fail_at=i;operations=probes=removed=0;
        assert(board_imu_init()==ESP_FAIL && !s_dev && removed==1);
        fail_at=0;operations=0;
        assert(board_imu_init()==ESP_OK && s_dev && probes==2 && operations==5);
        assert(board_imu_init()==ESP_OK && probes==2);
    }
    puts("PASS: every partial IMU configuration failure clears handle and forces complete retry");return 0;
}
''')


def wake_feed(args):
    run_case(args, 'boot_wake_feed', r'''
#define JULIA_SERVICE_ONLINE 1
#define JULIA_MAIN_STATE_S0_BOOT 0
static int julia_fsm_runtime_get_state(void){return 3;}
#define atomic_load(p) (*(p))
#define atomic_store(p,v) (*(p)=(v))
#define atomic_fetch_add(p,v) ((*(p)+=(v))-(v))
static bool s_paused;
static unsigned s_capture_epoch,s_feed_epoch,resets,feeds;
static int service;
static int julia_fsm_runtime_get_service_state(void){return service;}
static void *s_afe_data=(void*)1;
static int16_t buffer[4],last[4];
static int16_t *s_feed_buffer=buffer;
static size_t s_feed_chunk_samples=4,s_feed_samples;
static void reset_buffer(void *p){++resets;}
static void feed(void *p,const int16_t *samples){++feeds;memcpy(last,samples,sizeof(last));}
static struct {void(*reset_buffer)(void*);void(*feed)(void*,const int16_t*);} afe={reset_buffer,feed};
static const void *unused;
#define s_afe (&afe)
''', [('main/voice/capture/wake_detector.c',['wake_afe_feed'])], r'''
int main(void){
    int16_t old[]={1,1,1,1},fresh[]={2,2,2,2};
    wake_afe_feed(old,4,NULL);assert(feeds==0 && s_feed_samples==0);
    service=JULIA_SERVICE_ONLINE;wake_afe_feed(old,2,NULL);
    assert(resets==1 && s_feed_samples==2 && feeds==0);
    service=0;wake_afe_feed(old,4,NULL);assert(feeds==0);
    service=JULIA_SERVICE_ONLINE;wake_afe_feed(fresh,4,NULL);
    assert(resets==2 && feeds==1 && !memcmp(last,fresh,sizeof(last)));
    puts("PASS: pre-ready/offline AFE input is discarded and partial old audio is reset on recovery");return 0;
}
''')


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--cc',required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    args.root=Path(__file__).resolve().parents[2]
    args.out.mkdir(parents=True,exist_ok=True)
    coordinator(args)
    gates(args)
    branches(args)
    ota_admission(args)
    imu_partial(args)
    wake_feed(args)
