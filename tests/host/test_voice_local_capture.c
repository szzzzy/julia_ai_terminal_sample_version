#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Include the production owner module to inspect only its private test state. */
#include "../../main/voice/capture/voice_local_capture.c"

static struct { unsigned count,size; unsigned char data[4][64]; } queue;
static int64_t now;
static unsigned failures,start_events,end_events,sends;
static uint32_t expected_epoch;
static char session[]="session-a";
static FILE *wire;
static uint8_t wire_records[100][656];
static size_t wire_lengths[100];
QueueHandle_t xQueueCreate(unsigned count,unsigned size){assert(count==4 && size<64);queue.size=size;return &queue;}
void vQueueDelete(QueueHandle_t q){(void)q;}
int xQueueSend(QueueHandle_t q,const void *v,unsigned wait){if(queue.count==4)return 0;memcpy(queue.data[queue.count++],v,queue.size);return 1;}
int xQueueReceive(QueueHandle_t q,void *v,unsigned wait){if(!queue.count)return 0;memcpy(v,queue.data[0],queue.size);memmove(queue.data,queue.data+1,--queue.count*64);return 1;}
int64_t esp_timer_get_time(void){return now;}
const char *voice_state_sync_session_id(void){return session;}
void wss_transport_fail_session(void){++failures;}
void wss_transport_defer_retry(uint32_t seconds){assert(seconds==60);}
esp_err_t wss_transport_send_now(uint8_t opcode,const uint8_t *p,size_t n){assert(opcode==1 && strstr((const char*)p,"capture_hello"));return ESP_OK;}
static esp_err_t push(const uint8_t *data,size_t bytes,uint32_t generation)
{
    assert(generation==expected_epoch);
    if(sends<100){memcpy(wire_records[sends],data,bytes);wire_lengths[sends]=bytes;}
    ++sends;
    if(wire){uint32_t n=bytes;fwrite(&n,4,1,wire);fwrite(data,bytes,1,wire);}
    return ESP_OK;
}
static void event(lc_event_t e,lc_mode_t m)
{
    if(e==LC_START)++start_events;
    if(e==LC_END)++end_events;
}
static void input(int16_t value)
{
    uint8_t pcm1[656]={0};memcpy(pcm1,"PCM1",4);
    for(unsigned i=0;i<320;i++){
        int16_t sample=value;
#ifdef TEST_FFT_GATE
        if(value>1) sample=(int16_t)lrint(value*sin(6.283185307179586*i/16));
#endif
        pcm1[16+i*2]=sample;pcm1[17+i*2]=(uint16_t)sample>>8;
    }
    now+=20000;voice_local_capture_frame(pcm1,sizeof(pcm1));
}
static void text(const char *msg){assert(voice_local_capture_text((const uint8_t*)msg,strlen(msg)));}
static void connect(unsigned epoch_value)
{
    expected_epoch=epoch_value;voice_local_capture_connection(epoch_value);
    assert(!voice_local_capture_poll());
    text("{\"type\":\"capture_ready\",\"session_id\":\"wrong\",\"version\":1}");
    assert(!voice_local_capture_ready());
    char ack[160];
    snprintf(ack,sizeof(ack),"{\"type\":\"capture_ready\",\"session_id\":\"session-a\",\"version\":1,\"stream_generation\":%u}",epoch_value);
    text(ack);
    assert(voice_local_capture_ready());voice_local_capture_mode(LC_DIALOG);
}
int main(int argc,char **argv)
{
    bool vad_init_failure = argc>1 && !strcmp(argv[1], "--vad-init-fail");
#if CONFIG_JULIA_CAPTURE_VAD_ENABLE
    extern int vad_test_fail_create;
    vad_test_fail_create = vad_init_failure;
#endif
    assert(voice_local_capture_init(push,event)==ESP_OK);
#if CONFIG_JULIA_CAPTURE_VAD_ENABLE
    assert((s->capture.voice_frame == NULL) == vad_init_failure);
#endif
#ifdef TEST_FFT_GATE
    s->capture.fft_enabled=true;
#endif
    connect(10);
    if(argc>1 && !vad_init_failure){wire=fopen(argv[1],"wb");assert(wire);}
    for(unsigned i=0;i<25;i++)input(1);
    for(unsigned i=0;i<6;i++)input(200);
    assert(start_events==1 && sends==26);
    uint32_t first_id=s->capture.id;
    for(unsigned i=0;i<35;i++)input(1);
    assert(end_events==1 && sends==62 && s->history[first_id%4].complete);
    assert(wire_records[0][0]=='{' && !memcmp(wire_records[1],"PCM2",4));
    assert(strstr((char*)wire_records[61],"capture_end"));
    for(unsigned frame=0;frame<60;++frame) {
        const uint8_t *wire_pcm=wire_records[frame+1];
        assert(wire_lengths[frame+1]==656 && !memcmp(wire_pcm,"PCM2",4));
        assert(wire_pcm[8]==frame && wire_pcm[9]==0);
        assert(wire_pcm[12]==0x80 && wire_pcm[13]==2);
        unsigned checksum=0;
        for(unsigned j=16;j<656;++j) checksum+=wire_pcm[j];
        assert(wire_pcm[15]==(uint8_t)checksum);
        for(unsigned i=0;i<320;++i) {
            int16_t expected=(frame>=19 && frame<25)?200:1;
#ifdef TEST_FFT_GATE
            if(expected>1) expected=(int16_t)lrint(expected*sin(6.283185307179586*i/16));
#endif
            assert((int16_t)((uint16_t)wire_pcm[16+2*i] |
                ((uint16_t)wire_pcm[17+2*i]<<8))==expected);
        }
    }
    if(wire){fclose(wire);wire=NULL;}
    char verdict[160];snprintf(verdict,sizeof(verdict),"{\"type\":\"capture_verdict\",\"session_id\":\"session-a\",\"utterance_id\":%u,\"verdict\":\"noise\"}",first_id);
    for(unsigned i=0;i<6;i++)input(200);
    assert(s->capture.active);
    double frozen=s->capture.floor.bg;
    text(verdict);input(1);assert(s->capture.floor.bg==frozen);
    for(unsigned i=0;i<34;i++)input(1);
    assert(!s->capture.active && s->capture.floor.bg==frozen);
    input(1);assert(s->capture.floor.bg==-80 && last_applied==first_id);
    /* A duplicate verdict must not reset history twice. */
    s->capture.floor.bg=-70;text(verdict);input(1);assert(s->capture.floor.bg==-70);
    voice_local_capture_connection(0);connect(11);input(1);
    text(verdict);input(1);assert(last_applied==0);
    assert(!failures);
    puts("voice capture owner: negotiation, wire FIFO, frozen/deferred verdict, duplicate and epoch isolation passed");
    return 0;
}
