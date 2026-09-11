"""Offline A/B message interop using firmware C handlers and cloud Python classes.

No devices, live broker, sockets to production, or inference providers are used.
FSM/audio boundaries are fixtures; barrier, parser, dedup, ACK and sender are real.
"""
import argparse
import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from test_recovery_paths import COMMON, function

p=argparse.ArgumentParser()
p.add_argument('--cloud-root',type=Path,required=True)
p.add_argument('--cc',required=True)
p.add_argument('--cjson',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];a.out.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(a.cloud_root/'server'))
from state_sync import DeviceSession
from mqtt_adapter import MqttAdapter

src=(root/'main/voice/voice_service.c').read_text(encoding='utf-8')
names=['voice_service_handle_control_json','voice_service_apply_scoped_control',
       'interaction_id_is_valid','voice_service_handle_wake_json','voice_service_on_server_text']
driver=COMMON+'\n#include "control_service_fixture.h"\n'+'\n'.join(function(src,n) for n in names)+r'''
int main(int argc,char **argv){assert(argc==3);snprintf(s_voice_device_id,sizeof(s_voice_device_id),"%s",argv[1]);snprintf(session,sizeof(session),"%s",argv[2]);s_control_guard.active=true;
char input[1400];while(fgets(input,sizeof(input),stdin)){
 size_t n=strlen(input);if(n && input[n-1]=='\n')input[--n]=0;
 if(!strncmp(input,"@time ",6)){now=(int64_t)strtol(input+6,NULL,10)*1000;continue;}
 if(!strncmp(input,"@state ",7)){state=atoi(input+7);sub=0;s_playback_role=VOICE_PLAYBACK_ROLE_NONE;s_round_pending=false;continue;}
 last_wss[0]=last_status[0]=0;
 if(!strncmp(input,"m ",2))voice_service_apply_scoped_control((uint8_t*)input+2,n-2);
 else if(!strncmp(input,"w ",2))voice_service_on_server_text((uint8_t*)input+2,n-2);
 printf("{\"wss\":%s,\"ack\":%s,\"sleeps\":%u,\"starts\":%u,\"playing\":%s,\"state\":%d,\"used\":%u,\"high_water\":%lu,\"round\":%lu,\"paused\":%s}\n",
  last_wss[0]?last_wss:"null",last_status[0]?last_status:"null",sleeps,starts,playing?"true":"false",state,s_control_guard.used,(unsigned long)s_control_guard.high_water,(unsigned long)s_control_guard.interaction_seq,paused?"true":"false");fflush(stdout);
}return 0;}
'''
code=a.out/'cross_firmware.c';exe=a.out/'cross_firmware.exe';code.write_text(driver,encoding='utf-8')
subprocess.run([a.cc,*['-I'+str(d) for d in (root/'tests/host',root/'tests/host/stubs',root/'main/voice',root/'main/fsm',a.cjson)],str(code),str(root/'main/voice/voice_control_guard.c'),str(root/'main/fsm/julia_fsm.c'),str(a.cjson/'cJSON.c'),'-o',str(exe)],check=True)

class Firmware:
    def __init__(self,device,sid):
        self.device=device;self.sid=sid;self.started=time.monotonic();self.last={}
        self.process=subprocess.Popen([str(exe),device,sid],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    def exchange(self,channel,data):
        text=json.dumps(data,separators=(',',':')) if isinstance(data,dict) else data
        self.process.stdin.write('@time '+str(1000+int((time.monotonic()-self.started)*1000))+'\n'+channel+' '+text+'\n');self.process.stdin.flush()
        line=self.process.stdout.readline()
        if not line:raise RuntimeError('firmware process ended')
        self.last=json.loads(line);return self.last
    def state(self,value):self.process.stdin.write('@state '+str(value)+'\n');self.process.stdin.flush()
    def close(self):self.process.stdin.close();assert self.process.wait()==0

class Engine:
    _wake_ready_id=None
    _wake_ready_state=''
    def __init__(self):self._history=[];self._session_interaction_id=None
    def on_client_connected(self,*args):pass
    def apply_device_state(self,state):pass
    def set_sink(self,*args):pass
    def start(self):pass
    def stop(self):pass
    def close_session(self):pass

def snapshot(device,sid,state='S3',rev=1):
    return dict(type='session_sync',protocol_version=2,control_protocol=1,device_time_ms=1000,
        device_id=device,session_id=sid,interaction_id='',request_id='sync-1',state=state,
        sub_state='',state_revision=rev,wake_required=state not in {'S1','S2','S4'},companion_remaining_ms=0,reason='fixture')

async def run():
    devices={d:Firmware(d,('a' if d.endswith('55') else 'b')*32) for d in ('esp-001122334455','esp-556677889900')}
    sessions={};background=[];sent=[];lost_ack=set();drop_once=True
    mqtt=MqttAdapter({'mqtt':{'require_auth':True}},None,None,None);mqtt._ready.set()
    async def publish(topic,payload,qos):
        data=json.loads(payload);device=data['device_id'];sent.append(data)
        assert topic==f'voice/{device}/vcmd' and qos==1
        result=devices[device].exchange('m',data)
        if drop_once and data.get('intent')=='goodnight' and data['request_id'] not in lost_ack:
            lost_ack.add(data['request_id']);return
        if result['ack']:mqtt.accept_control_ack(device,result['ack'])
    mqtt.client=SimpleNamespace(publish=publish)
    adapter=SimpleNamespace(identity=SimpleNamespace(enabled=True),cfg={},engine=SimpleNamespace(mqtt=mqtt))
    adapter.spawn=lambda coro:background.append(asyncio.create_task(coro))
    adapter.owns=lambda s:s.active and sessions.get(s.device_id) is s
    async def make(device):
        fw=devices[device]
        class WS:
            async def send(self,text):
                data=json.loads(text) if text.startswith('{') else text
                if isinstance(data,dict) and data['type'] in ('session_sync_ack','device_state_ack'):return
                result=fw.exchange('w',data)
                if result['wss'] and result['wss']['type']=='interaction_sync_ack':
                    session.controls.accept_round_ack(result['wss'])
            async def close(self,**kwargs):pass
        session=DeviceSession(adapter,WS(),Engine,device_id=device);sessions[device]=session
        await session.handle(snapshot(device,fw.sid));session.ensure_engine();return session
    try:
        sa,sb=await make(list(devices)[0]),await make(list(devices)[1]);fa,fb=devices.values()
        for s in (sa,sb):
            assert await s.deliver(s.engine,json.dumps(dict(type='wake_detected',interaction_id='wake_'+s.device_id)))
            s.apply(dict(snapshot(s.device_id,s.session_id,'S4',2),type='device_state'))
        assert await sb.deliver(sb.engine,'SPKS 24000');assert fb.last['playing']
        # First-wake ID, real MQTT sender, real firmware execution, lost execution ACK and retry.
        sa.controls.queue(sa.engine,json.dumps(dict(type='intent_result',intent='goodnight')),sa.controls.sequence,sa.interaction_id)
        await asyncio.gather(*list(sa.controls.tasks))
        assert sa.controls.last_result['accepted'] and fa.last['sleeps']==1
        assert len([x for x in sent if x.get('intent')=='goodnight'])==2
        assert fb.last['playing'] and not fb.last['sleeps']
        old=dict(sent[0]);drop_once=False
        fa.state(1);sa.state=dict(snapshot(sa.device_id,sa.session_id,'S1',3))
        assert await sa.deliver(sa.engine,'MIC_START');assert fa.last['starts']==1
        late=fa.exchange('m',old);assert late['ack']['code']=='stale_interaction' and late['sleeps']==1
        # More than 32 requests in one round, then 200 further barriers/rounds.
        async def normal():
            sa.controls.queue(sa.engine,json.dumps(dict(type='intent_result',intent='normal')),sa.controls.sequence,sa.interaction_id)
            await asyncio.gather(*list(sa.controls.tasks));assert sa.controls.last_result['accepted']
        for _ in range(50):await normal()
        assert fa.last['used']==8 and fa.last['high_water']==50
        for _ in range(200):
            fa.state(1);sa.state=dict(snapshot(sa.device_id,sa.session_id,'S1',3))
            assert await sa.deliver(sa.engine,'MIC_START')
            await normal()
        assert fa.last['used']==1 and len(sa.controls.tasks)==0 and not mqtt.voice_pending
        wrong=dict(sent[-1],device_id=sb.device_id)
        assert fa.exchange('m',wrong)['ack'] is None
        # Busy is per-device and preserves B's playback.
        await sa.send_json(dict(type='busy',code='voice_capacity',retry_after_ms=1000))
        assert fa.last['paused'] and fa.last['state']==3 and fb.last['playing']
        # Old session envelope cannot execute after firmware session replacement.
        prior=dict(sent[-1]);sa.close();fa.close()
        devices[sa.device_id]=Firmware(sa.device_id,'c'*32);fa=devices[sa.device_id]
        fresh=await make(sa.device_id)
        assert fa.exchange('m',prior)['ack'] is None and sessions[sb.device_id] is sb
        result={'first_wake_intent':'applied','lost_ack_retry':'same result, one execution',
                'same_round_controls':50,'further_rounds':200,'late_round':'rejected','late_session':'rejected',
                'wrong_device':'rejected','busy':'A paused, B unchanged','pending_controls':len(mqtt.voice_pending),
                'hardware_tested':False,'paid_api_used':False}
        (a.out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps(result,ensure_ascii=False,indent=2))
    finally:
        for s in sessions.values():s.close()
        await asyncio.gather(*(t for s in sessions.values() for t in s.cleanup),*background,return_exceptions=True)
        for fw in devices.values():fw.close()

asyncio.run(run())
