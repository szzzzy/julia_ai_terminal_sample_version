"""Build a reviewable cloud patch against the inspected production source.

Usage: python prepare_patch.py REFERENCE_API_SERVER OUTPUT_API_SERVER PATCH_FILE
The output directory must be separate from the reference. No service is restarted.
"""
from pathlib import Path
import difflib
import shutil
import sys
import hashlib
import json

reference, output, patch = map(Path, sys.argv[1:])
if reference.resolve() == output.resolve():
    raise SystemExit('reference and output must differ')
baseline = Path(__file__).with_name('baseline.json')
if baseline.exists():
    for name, digest in json.loads(baseline.read_text(encoding='utf-8'))['sha256'].items():
        if hashlib.sha256((reference / name).read_bytes()).hexdigest() != digest:
            raise SystemExit('Cloud source changed; re-review required: ' + name)
for folder in ('server', 'engine'):
    shutil.copytree(reference / folder, output / folder, dirs_exist_ok=True)


def edit(file, old, new, count=1):
    path = output / file
    text = path.read_text(encoding='utf-8')
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'{file}: expected {count} matches, got {actual}: {old[:80]}')
    path.write_text(text.replace(old, new), encoding='utf-8', newline='\n')


edit('server/state_sync.py', '        self.floor = None\n        self.playback_until',
     '        self.floor = None\n        self.capture_receiver = None\n        self.capture_generation = 0\n        self.capture_dialog_id = 0\n        self.playback_until')
edit('server/state_sync.py', '        if boundary:\n',
     "        if (boundary and self.capture_receiver is not None\n"
     "                and old['state'] in WAIT and state['state'] in WAIT):\n"
     '            boundary = False\n        if boundary:\n')
edit('server/state_sync.py', '        try:\n            if self.strict_identity:\n',
     '''        try:
            if isinstance(payload, str) and payload.startswith('{'):
                local = json.loads(payload)
                if local.get('type') == 'capture_dialog':
                    uid = local.get('utterance_id')
                    if (self.capture_receiver is None or type(uid) is not int or uid <= 0
                            or uid != getattr(engine.stream, 'capture_utterance_id', None)):
                        return False
                    if uid == self.capture_dialog_id:
                        return True
                    if self.strict_identity and not await self.controls.begin(engine, 'speech', 'utt_'+str(uid)):
                        return False
                    self.capture_dialog_id = uid
                    return True
            if self.strict_identity:
''')
edit('server/state_sync.py', "if self.state['state'] == 'S2' and self.state['sub_state'] != 'S2.1':",
     "if (self.state['state'] == 'S2' and self.state['sub_state'] != 'S2.1'\n"
     "                and not (self.capture_receiver and self.capture_receiver.has_pending())):")
edit('server/state_sync.py', '            engine.v2_session = self\n',
     '            engine.v2_session = self\n'
     '            if self.capture_receiver is not None:\n'
     '                engine.stream.local_capture = self.capture_receiver\n'
     '                engine.stream.capture_stop = engine._stop\n')
edit('server/state_sync.py', "        if command == 'MIC_START':\n            return state in {'S1', 'S4'} or self.state['sub_state'] in {'S2.1','S2.3'}",
     "        if command == 'MIC_START':\n            return state in {'S1', 'S4'} or self.state['sub_state'] in {'S2.1','S2.3'} or (self.capture_receiver is not None and self.state['sub_state'] == 'S2.2')")
edit('server/state_sync.py', "        if payload.startswith('{'):\n            return json.loads(payload).get('type') == 'wake_detected' and state in WAIT | {'S1'}",
     "        if payload.startswith('{'):\n            kind = json.loads(payload).get('type')\n            if kind == 'capture_verdict' and self.capture_receiver is not None:\n                return True\n            return kind == 'wake_detected' and state in WAIT | {'S1'}")
edit('server/state_sync.py', '    def close(self):\n        self.controls.close()',
     '    def close(self):\n        if self.capture_receiver is not None:\n            self.capture_receiver.close()\n        self.controls.close()')

edit('server/wss_adapter.py', '        pcm1 = pcm1_parse(message)\n', '''        receiver = getattr(self.sessions.get(ws), 'capture_receiver', None)
        if receiver is not None:
            from local_capture import CaptureProtocolError
            try:
                pcm1 = receiver.audio(message)
            except CaptureProtocolError as exc:
                target.on_bad_frame(str(exc))
                asyncio.create_task(ws.close(code=1002, reason='capture protocol error'))
                return
            target.on_frame(pcm1, raw=None)
            return
        pcm1 = pcm1_parse(message)
''')
edit('server/wss_adapter.py', "                kind = data.get('type')\n", '''                kind = data.get('type')
                if kind in {'capture_hello', 'capture_start', 'capture_end', 'capture_abort'}:
                    from local_capture import LocalCaptureReceiver, CaptureProtocolError, decode_capture_text
                    session = self.sessions.get(ws)
                    if not session or not session.ready or not session.owns():
                        await ws.close(code=1002, reason='capture requires synchronized session')
                        return file_rx
                    try:
                        data = decode_capture_text(t)
                        if kind == 'capture_hello':
                            generation = data.get('stream_generation')
                            if (data.get('session_id') != session.session_id or type(data.get('version')) is not int or data.get('version') != 1
                                    or data.get('sample_rate') != 16000 or data.get('frame_ms') != 20
                                    or type(generation) is not int or not 0 < generation <= 0xffffffff):
                                raise CaptureProtocolError('capture capability mismatch')
                            if generation < getattr(session, 'capture_generation', 0):
                                raise CaptureProtocolError('stale capture generation')
                            if (session.capture_receiver is None and session.engine is not None
                                    and getattr(session.engine, 'frames', 0)):
                                raise CaptureProtocolError('PCM1 requires a new connection before capture-v1')
                            if session.capture_receiver is not None and session.capture_generation != generation:
                                session.capture_receiver.close()
                                session.retire()
                                session.capture_receiver = None
                            if session.capture_receiver is None:
                                session.capture_receiver = LocalCaptureReceiver()
                            session.capture_generation = generation
                            if session.engine is not None:
                                session.engine.stream.local_capture = session.capture_receiver
                                session.engine.stream.capture_stop = session.engine._stop
                            await session.send_json(dict(type='capture_ready', version=1,
                                session_id=session.session_id, stream_generation=generation))
                        else:
                            receiver = session.capture_receiver
                            if receiver is None:
                                raise CaptureProtocolError('capture not negotiated')
                            if kind == 'capture_start':
                                state = session.state
                                allowed = (state['state'] in {'S3','S5','S6'} if data.get('mode') == 'wake'
                                           else state['state'] in {'S1','S2','S4'})
                                if not allowed:
                                    raise CaptureProtocolError('capture mode/state mismatch')
                                receiver.start(data)
                                engine = session.ensure_engine()
                                if engine is None:
                                    raise CaptureProtocolError('capture engine unavailable')
                                engine._wake_worker()
                            else:
                                receiver.end(data, abort=kind == 'capture_abort')
                    except CaptureProtocolError:
                        await ws.close(code=1002, reason='capture protocol error')
                    return file_rx
''')

edit('engine/board_serial_asr_test.py', '    samples = []\n    dbfs_frames = []\n    max_samples = int(max_seconds * sr)\n',
     "    receiver = getattr(ser, 'local_capture', None)\n"
     "    if receiver is not None:\n"
     "        def local_start(uid):\n            ser.capture_utterance_id = uid\n            if on_start:\n                on_start()\n"
     "        return receiver.capture(capture_mode or ('wake' if voice_start_ms == 20 else 'dialog'),\n"
     "                                on_chunk, local_start, getattr(ser, 'capture_stop', None))\n"
     '    samples = []\n    dbfs_frames = []\n    max_samples = int(max_seconds * sr)\n')
edit('engine/board_serial_asr_test.py', '    on_start=None,\n):\n',
     '    on_start=None,\n    capture_mode=None,\n):\n')
edit('server/real_engine.py', '                        self.stream, max_seconds=wk_listen_s,\n',
     '                        self.stream, max_seconds=wk_listen_s, capture_mode="wake",\n')
edit('server/real_engine.py', '                    self.stream, max_seconds=max_seconds,\n',
     '                    self.stream, max_seconds=max_seconds, capture_mode="dialog",\n')
edit('server/real_engine.py', '        if df_cfg.get("enabled", False):\n',
     '        if df_cfg.get("enabled", False) and getattr(self.stream, "local_capture", None) is None:\n')

edit('server/real_engine.py', "if state['state'] in {'S0', 'S7', 'S8'} or state['sub_state'] in {'S2.2', 'S2.3'}:",
     "if (state['state'] in {'S0', 'S7', 'S8'} or (state['sub_state'] in {'S2.2', 'S2.3'} and not self._local_pending())):")
edit('server/real_engine.py', '            seq = pcm1["seq"]\n',
     '            uid = pcm1.get("utterance_id")\n'
     '            if uid is not None and uid != getattr(self, "_last_capture_id", None):\n'
     '                self.last_seq = None\n                self._last_capture_id = uid\n'
     '            seq = pcm1["seq"]\n')
edit('server/real_engine.py', '            "seq": seq, "bytes": pcm1["bytes_len"], "dbfs": pcm1["dbfs_x100"] / 100.0,\n',
     '            "utterance_id": pcm1.get("utterance_id"),\n'
     '            "seq": seq, "bytes": pcm1["bytes_len"], "dbfs": pcm1["dbfs_x100"] / 100.0,\n')
edit('server/real_engine.py', '            if self.stream.real_bytes_total <= self._real_bytes_consumed:\n',
     '            if self.stream.real_bytes_total <= self._real_bytes_consumed and not self._local_pending():\n')
edit('server/real_engine.py', '            if not self._calibration_done and self._floor is None:\n',
     "            if getattr(self.stream, 'local_capture', None) is not None:\n"
     '                self._floor = None\n                self._calibration_done = True\n'
     '            if not self._calibration_done and self._floor is None:\n')
edit('server/real_engine.py', '                self._real_bytes_consumed = self.stream.real_bytes_total\n                if self._stop.is_set():\n',
     '                self._real_bytes_consumed = self.stream.real_bytes_total\n                if self._stop.is_set():\n')
edit('server/real_engine.py', '                    if not ok_sp:\n                        self._nonspeech_count += 1\n',
     "                    if not ok_sp:\n                        self._capture_verdict(w_ep, 'empty')\n                        self._nonspeech_count += 1\n")
edit('server/real_engine.py', '                if hit:\n                    interaction_id =',
     "                self._capture_verdict(w_ep, 'speech' if wake_text else 'empty')\n"
     '                if hit:\n                    interaction_id =')
edit('server/real_engine.py', '                if not ok_sp:\n                    self._nonspeech_count += 1\n',
     "                if not ok_sp:\n                    self._capture_verdict(endpoint, 'empty')\n"
     "                    if endpoint.get('local_capture'):\n                        self._send_empty_round(mic_start=False)\n"
     '                    self._nonspeech_count += 1\n')
edit('server/real_engine.py', '            if self._wake_enabled and not is_noise and recognized:\n',
     "            self._capture_verdict(endpoint, 'noise' if is_noise else ('speech' if recognized else 'empty'))\n"
     "            if is_noise and endpoint.get('local_capture'):\n                recognized = ''\n"
     '            if self._wake_enabled and not is_noise and recognized:\n')
edit('server/real_engine.py', '                if self._wake_enabled and not self._mic_started:\n',
     "                if (getattr(self.stream, 'local_capture', None) is not None\n"
     '                        or (self._wake_enabled and not self._mic_started)):\n')
edit('server/real_engine.py', '    def _push_text(self, text, *, drain=True):\n',
     "    def _local_pending(self):\n        receiver = getattr(self.stream, 'local_capture', None)\n"
     '        return receiver is not None and receiver.has_pending()\n\n'
     "    def _capture_verdict(self, endpoint, verdict):\n        if endpoint.get('local_capture'):\n"
     "            self._push_text(json.dumps(dict(type='capture_verdict',\n"
     "                utterance_id=endpoint['utterance_id'], verdict=verdict,\n"
     "                session_id=self.v2_session.session_id)))\n\n"
     '    def _push_text(self, text, *, drain=True):\n'
     "        if text == 'MIC_STOP' and getattr(self.stream, 'local_capture', None) is not None:\n            return\n")
edit('server/real_engine.py', '        self._push_text("MIC_START")\n        self._mic_started = True\n',
     "        if getattr(self.stream, 'local_capture', None) is not None:\n"
     "            uid = getattr(self.stream, 'capture_utterance_id', None)\n"
     "            if uid is not None:\n                self._push_text(json.dumps(dict(type='capture_dialog', utterance_id=uid)))\n"
     '        else:\n            self._push_text("MIC_START")\n        self._mic_started = True\n')
edit('server/real_engine.py', '        if self._interrupt_enabled and self._asr_model is not None and str(text or "").strip():\n',
     '        if (self._interrupt_enabled and self._asr_model is not None and str(text or "").strip()\n'
     "                and getattr(self.stream, 'local_capture', None) is None):\n")

shutil.copyfile(Path(__file__).with_name('local_capture.py'), output / 'server/local_capture.py')
parts = []
for name in ('server/state_sync.py', 'server/wss_adapter.py', 'server/real_engine.py',
             'engine/board_serial_asr_test.py', 'server/local_capture.py'):
    source = reference / name
    old = source.read_text(encoding='utf-8').splitlines(True) if source.exists() else []
    new = (output / name).read_text(encoding='utf-8').splitlines(True)
    parts.extend(difflib.unified_diff(old, new, fromfile='a/api_server/'+name if old else '/dev/null',
                                    tofile='b/api_server/'+name))
patch.parent.mkdir(parents=True, exist_ok=True)
patch.write_text(''.join(parts), encoding='utf-8', newline='\n')
print(patch)
