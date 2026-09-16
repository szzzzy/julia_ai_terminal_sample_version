import asyncio
import ast
import json
import logging
import os
from pathlib import Path
import struct
import sys
import threading
import time
from types import SimpleNamespace
import unittest

from local_capture import LocalCaptureReceiver, CaptureProtocolError, decode_capture_text


def start(uid=1, mode='dialog'):
    return dict(type='capture_start',utterance_id=uid,mode=mode,floor_dbfs=-60,
                sample_rate=16000,frame_ms=20,frames=0)


def audio(uid=1,index=0,value=200):
    pcm=struct.pack('<320h',*[value]*320)
    return b'PCM2'+struct.pack('<IHhHBB',uid,index,-4429,640,0,sum(pcm)&255)+pcm


def end(uid=1,frames=1,mode='dialog',reason='silence'):
    return dict(type='capture_end',utterance_id=uid,frames=frames,mode=mode,reason=reason)


class ReceiverTest(unittest.TestCase):
    def test_duplicate_control_fields_are_rejected(self):
        for text in ('{"type":"capture_start","utterance_id":1,"utterance_id":2}',
                     '[]', '{"type":'+ '['*2000):
            with self.assertRaises(CaptureProtocolError):decode_capture_text(text)
    def test_actual_firmware_wire(self):
        path=Path(os.environ.get('CAPTURE_WIRE_FILE', str(Path(__file__).resolve().parents[2]/'build/capture-wire.bin')))
        if not path.exists():self.skipTest('run test_voice_local_capture with wire output first')
        raw=path.read_bytes();at=0;r=LocalCaptureReceiver();events=[]
        while at<len(raw):
            n=struct.unpack_from('<I',raw,at)[0];at+=4
            record=raw[at:at+n];at+=n
            if record.startswith(b'{'):
                data=json.loads(record);events.append(data['type'])
                if data['type']=='capture_start':r.start(data)
                else:r.end(data)
            else:r.audio(record)
        samples,_,diag=r.capture('dialog')
        self.assertEqual(events,['capture_start','capture_end'])
        self.assertEqual(len(samples),60*320)
        self.assertEqual(list(samples[19*320:25*320]),[200]*(6*320))
        self.assertEqual(list(samples[25*320:]),[1]*(35*320))
        self.assertEqual(diag['utterance_id'],1)

    def test_complete_tail_and_identity(self):
        r=LocalCaptureReceiver();r.start(start());r.audio(audio(value=201));r.audio(audio(index=1,value=7))
        r.end(end(frames=2));samples,levels,diag=r.capture('dialog')
        self.assertEqual(list(samples[:320]),[201]*320)
        self.assertEqual(list(samples[320:]),[7]*320)
        self.assertEqual(diag['utterance_id'],1)
        self.assertTrue(diag['endpoint_triggered'])

    def test_streaming_waits_for_explicit_end(self):
        r=LocalCaptureReceiver();r.start(start())
        chunks=[];result=[];started=[];ready=threading.Event()
        def chunk(x):chunks.append(x);ready.set()
        t=threading.Thread(target=lambda:result.append(r.capture('dialog',chunk,started.append)))
        t.start()
        for i in range(30):r.audio(audio(index=i))
        self.assertTrue(ready.wait(2));self.assertEqual(started,[1]);self.assertEqual(len(chunks[0]),9600)
        self.assertFalse(result)
        r.audio(audio(index=30,value=9));r.end(end(frames=31));t.join(2)
        self.assertFalse(t.is_alive());self.assertEqual(len(result[0][0]),31*320)
        self.assertEqual(result[0][0][-1],9)

    def test_order_length_checksum_and_overlap(self):
        for frame in (audio(uid=2),audio(index=1),audio()[:-1],audio()[:20]+b'\x00'+audio()[21:]):
            with self.subTest(frame=frame[:16]):
                r=LocalCaptureReceiver();r.start(start())
                with self.assertRaises(CaptureProtocolError):r.audio(frame)
        r=LocalCaptureReceiver();r.start(start())
        with self.assertRaises(CaptureProtocolError):r.start(start(2))
        r.audio(audio())
        with self.assertRaises(CaptureProtocolError):r.audio(audio())
        with self.assertRaises(CaptureProtocolError):r.end(end(frames=2))

    def test_no_synthetic_endpoint_and_timeout(self):
        clock=[0];r=LocalCaptureReceiver(lambda:clock[0]);r.start(start());r.audio(audio())
        clock[0]=5.01
        with self.assertRaises(TimeoutError):r.capture('dialog')
        with self.assertRaises(CaptureProtocolError):r.end(end())

    def test_abort_and_disconnect_never_produce_asr(self):
        for close in (False,True):
            r=LocalCaptureReceiver();r.start(start());r.audio(audio())
            if close:r.close()
            else:r.end(end(),abort=True)
            with self.assertRaises(TimeoutError):r.capture('dialog')

    def test_limits_and_reconnection(self):
        for mode,count in (('wake',400),('dialog',750)):
            r=LocalCaptureReceiver();r.start(start(mode=mode))
            for i in range(count):r.audio(audio(index=i))
            with self.assertRaises(CaptureProtocolError):r.audio(audio(index=count))
            r.end(end(frames=count,mode=mode,reason='limit'))
            self.assertFalse(r.capture(mode)[2]['endpoint_triggered'])
            with self.assertRaises(CaptureProtocolError):r.start(start(mode=mode))
        LocalCaptureReceiver().start(start())

    def test_invalid_metadata_and_pending_bound(self):
        for change in (dict(utterance_id=True),dict(floor_dbfs=float('nan')),
                       dict(sample_rate=8000),dict(frame_ms=10),dict(frames=1)):
            data=start();data.update(change)
            with self.assertRaises(CaptureProtocolError):LocalCaptureReceiver().start(data)
        r=LocalCaptureReceiver()
        for uid in range(1,5):r.start(start(uid));r.audio(audio(uid));r.end(end(uid))
        with self.assertRaises(CaptureProtocolError):r.start(start(5))


class CloudAdaptationTest(unittest.IsolatedAsyncioTestCase):
    async def test_standby_to_sleep_preserves_pending_wake_audio(self):
        root=Path(os.environ.get('CAPTURE_CLOUD_ROOT', str(Path(__file__).resolve().parents[2]/'build/local-capture-work')))/'server'
        sys.path.insert(0,str(root))
        from state_sync import DeviceSession
        session=DeviceSession(SimpleNamespace(),None)
        session.capture_receiver=LocalCaptureReceiver()
        session.state=dict(state='S3',sub_state='',state_revision=1)
        engine=SimpleNamespace(apply_device_state=lambda s:None)
        session.engine=engine
        session.capture_receiver.start(start(mode='wake'))
        session.capture_receiver.audio(audio())
        self.assertTrue(session.apply(dict(state='S6',sub_state='',state_revision=2,
            wake_required=True,companion_remaining_ms=0,reason='night')))
        self.assertIs(session.engine,engine)
        session.capture_receiver.end(end(mode='wake'))
        self.assertEqual(len(session.capture_receiver.capture('wake')[0]),320)

    async def test_dispatch_handshake_and_pcm2(self):
        # Execute the actual patched WSS dispatcher with transport and API edges mocked.
        root=Path(os.environ.get('CAPTURE_CLOUD_ROOT', str(Path(__file__).resolve().parents[2]/'build/local-capture-work')))/'server'
        if not root.exists():self.skipTest('prepare_patch.py must run first')
        sys.path.insert(0,str(root))
        tree=ast.parse((root/'wss_adapter.py').read_text(encoding='utf-8'))
        cls=next(n for n in tree.body if isinstance(n,ast.ClassDef) and any(
            isinstance(f,ast.AsyncFunctionDef) and f.name=='_dispatch_text' for f in n.body))
        cls.body=[n for n in cls.body if isinstance(n,(ast.FunctionDef,ast.AsyncFunctionDef)) and n.name in ('_dispatch_text','_dispatch_bytes')]
        ns=dict(asyncio=asyncio,json=json,FRAME_LIMIT=65536,log=logging.getLogger('test'))
        exec(compile(ast.Module(body=[cls],type_ignores=[]),'actual_wss_dispatch','exec'),ns)
        adapter=ns[cls.name]()
        class WS:
            def __init__(self):self.sent=[];self.closed=[]
            async def send(self,x):self.sent.append(x)
            async def close(self,**kw):self.closed.append(kw)
        ws=WS();frames=[];stream=SimpleNamespace()
        engine=SimpleNamespace(stream=stream,_stop=threading.Event(),_wake_worker=lambda:None,
                               on_frame=lambda f,raw=None:frames.append(f),on_bad_frame=lambda x:None)
        async def send_json(data):ws.sent.append(data)
        session=SimpleNamespace(ready=True,active=True,owns=lambda:True,session_id='session-a',
            capture_receiver=None,engine=engine,send_json=send_json,
            state=dict(state='S3',sub_state=''),ensure_engine=lambda:engine)
        adapter.sessions={ws:session};adapter.modes={ws:'v2'};adapter.allow_legacy=False
        await adapter._dispatch_text(ws,json.dumps(dict(type='capture_hello',version=1,
            session_id='session-a',sample_rate=16000,frame_ms=20,stream_generation=10)),None)
        self.assertEqual(ws.sent[-1]['type'],'capture_ready')
        self.assertEqual(ws.sent[-1]['session_id'],'session-a')
        self.assertIs(stream.local_capture,session.capture_receiver)
        await adapter._dispatch_text(ws,json.dumps(start(mode='wake')),None)
        adapter._dispatch_bytes(audio(),ws)
        await adapter._dispatch_text(ws,json.dumps(end(mode='wake')),None)
        self.assertEqual(len(frames),1);self.assertFalse(ws.closed)
        result=session.capture_receiver.capture('wake')
        self.assertEqual(len(result[0]),320)
        adapter._dispatch_bytes(audio(),ws);await asyncio.sleep(0)
        self.assertTrue(ws.closed)

    async def test_no_cloud_mic_commands_for_local_capture(self):
        root=Path(os.environ.get('CAPTURE_CLOUD_ROOT', str(Path(__file__).resolve().parents[2]/'build/local-capture-work')))/'server'
        tree=ast.parse((root/'real_engine.py').read_text(encoding='utf-8'))
        cls=next(n for n in tree.body if isinstance(n,ast.ClassDef) and n.name=='RealVoiceEngine')
        method=next(n for n in cls.body if isinstance(n,ast.FunctionDef) and n.name=='_send_mic_start')
        ns=dict(time=time,json=json)
        exec(compile(ast.Module(body=[method],type_ignores=[]),'actual_start','exec'),ns)
        sent=[]
        fake=SimpleNamespace(r={'mic_start_min_delay_s':0},_last_spke_at=0,
            stream=SimpleNamespace(local_capture=object(),capture_utterance_id=42),_push_text=sent.append)
        ns['_send_mic_start'](fake)
        self.assertEqual(json.loads(sent[0]),dict(type='capture_dialog',utterance_id=42))
        self.assertTrue(fake._mic_started)

    async def test_think_before_start_delivery_still_binds_control_round(self):
        root=Path(os.environ.get('CAPTURE_CLOUD_ROOT', str(Path(__file__).resolve().parents[2]/'build/local-capture-work')))/'server'
        sys.path.insert(0,str(root))
        from state_sync import DeviceSession
        sent=[]
        class Socket:
            async def send(self,payload):
                data=json.loads(payload);sent.append(data)
                if data.get('type')=='interaction_sync':
                    ack=dict(data,type='interaction_sync_ack',accepted=True,code='applied',device_time_ms=1000)
                    session.controls.accept_round_ack(ack)
        session=DeviceSession(SimpleNamespace(identity=SimpleNamespace(enabled=True)),Socket(),device_id='device-a')
        session.ready=True;session.session_id='session-a'
        session.state=dict(state='S2',sub_state='S2.2',state_revision=4)
        session.capture_receiver=LocalCaptureReceiver()
        engine=SimpleNamespace(stream=SimpleNamespace(capture_utterance_id=42))
        session.engine=engine
        payload=json.dumps(dict(type='capture_dialog',utterance_id=42))
        self.assertTrue(await session.deliver(engine,payload))
        self.assertEqual(session.controls.sequence,1)
        self.assertEqual(engine._session_interaction_id,'utt_42')
        self.assertTrue(await session.deliver(engine,payload))
        self.assertEqual(len(sent),1)
        self.assertEqual(sent[0]['type'],'interaction_sync')
        self.assertFalse(await session.deliver(engine,json.dumps(dict(type='capture_dialog',utterance_id=41))))


if __name__=='__main__':unittest.main()
