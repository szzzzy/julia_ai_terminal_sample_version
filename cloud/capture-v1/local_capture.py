"""Receive device-defined 16 kHz utterances without estimating endpoints or noise.

One instance belongs to one authenticated WSS connection. Protocol errors are fatal:
incomplete or reordered audio must never reach ASR as a completed utterance.
"""
from collections import deque
import math
import json
import struct
import threading
import time


class CaptureProtocolError(ValueError):
    pass


def decode_capture_text(text):
    if len(text) > 1024:
        raise CaptureProtocolError('capture control size')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise CaptureProtocolError('duplicate capture field')
            result[key] = value
        return result
    try:
        data = json.loads(text, object_pairs_hook=unique)
    except (ValueError, RecursionError) as exc:
        raise CaptureProtocolError('invalid capture JSON') from exc
    if not isinstance(data, dict):
        raise CaptureProtocolError('capture object required')
    return data


class LocalCaptureReceiver:
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.cond = threading.Condition()
        self.pending = deque()
        self.active = None
        self.last_id = 0
        self.closed = False

    def start(self, data):
        uid = data.get('utterance_id')
        bg = data.get('floor_dbfs')
        mode = data.get('mode')
        if (type(uid) is not int or not 0 < uid <= 0xffffffff or uid <= self.last_id
                or mode not in ('wake', 'dialog') or type(bg) not in (int, float)
                or not math.isfinite(bg) or not -80 <= bg <= -35
                or data.get('sample_rate') != 16000 or data.get('frame_ms') != 20
                or data.get('frames') != 0):
            raise CaptureProtocolError('invalid capture_start')
        with self.cond:
            if self.closed or self.active is not None or len(self.pending) >= 4:
                raise CaptureProtocolError('capture overlap or capacity')
            now = self.clock()
            segment = dict(id=uid, mode=mode, floor=bg, frames=[], end=None,
                           aborted=False, started=now, received=now)
            self.active = segment
            self.last_id = uid
            self.pending.append(segment)
            self.cond.notify_all()

    def audio(self, message):
        if len(message) != 656 or message[:4] != b'PCM2':
            raise CaptureProtocolError('PCM2 length/magic')
        uid, index, db, size, flags, checksum = struct.unpack('<IHhHBB', message[4:16])
        pcm = message[16:]
        if size != 640 or flags or sum(pcm) & 255 != checksum or not -12000 <= db <= 0:
            raise CaptureProtocolError('PCM2 header/checksum')
        with self.cond:
            s = self.active
            if s is None or s['id'] != uid or index != len(s['frames']):
                raise CaptureProtocolError('PCM2 utterance/sequence')
            if index >= (400 if s['mode'] == 'wake' else 750):
                raise CaptureProtocolError('capture sample limit')
            self._check_timeout(s)
            s['frames'].append((pcm, db / 100))
            s['received'] = self.clock()
            self.cond.notify_all()
        return dict(seq=index, utterance_id=uid, bytes_len=size, dbfs_x100=db, pcm=pcm)

    def end(self, data, abort=False):
        with self.cond:
            s = self.active
            if (s is None or data.get('utterance_id') != s['id']
                    or type(data.get('frames')) is not int
                    or data['frames'] != len(s['frames']) or data.get('mode') != s['mode']):
                raise CaptureProtocolError('capture end mismatch')
            self._check_timeout(s)
            if not abort and (not s['frames'] or data.get('reason') not in ('silence', 'limit')):
                raise CaptureProtocolError('capture empty/reason')
            s['aborted'] = abort
            s['end'] = dict(data)
            self.active = None
            self.cond.notify_all()

    def _check_timeout(self, s):
        now = self.clock()
        if now - s['received'] > 5 or now - s['started'] > 25:
            s['aborted'] = True
            raise CaptureProtocolError('capture receive timeout')

    def has_pending(self):
        with self.cond:
            return bool(self.pending)

    def close(self):
        with self.cond:
            self.closed = True
            for s in self.pending:
                s['aborted'] = True
            self.cond.notify_all()

    def capture(self, mode, on_chunk=None, on_start=None, stop_event=None):
        import numpy as np
        with self.cond:
            if not self.pending:
                raise TimeoutError('no device segment')
            s = self.pending.popleft()
        if s['mode'] != mode:
            raise TimeoutError('stale capture mode')
        if on_start:
            on_start(s['id'])
        sent = 0
        while True:
            with self.cond:
                if self.closed or s['aborted'] or (stop_event and stop_event.is_set()):
                    raise TimeoutError('capture cancelled')
                try:
                    if s['end'] is None:
                        self._check_timeout(s)
                except CaptureProtocolError as exc:
                    raise TimeoutError(str(exc)) from exc
                count = len(s['frames'])
                end = s['end']
                blocks = []
                while count - sent >= 30:
                    blocks.append(b''.join(p for p, _ in s['frames'][sent:sent+30]))
                    sent += 30
                if not blocks and end is None:
                    self.cond.wait(.1)
                    continue
            # Call inference outside the receiver lock so WSS remains responsive.
            if on_chunk:
                for block in blocks:
                    on_chunk(np.frombuffer(block, dtype='<i2').copy())
            if end is not None:
                break
        samples = np.frombuffer(b''.join(p for p, _ in s['frames']), dtype='<i2').copy()
        levels = [d for _, d in s['frames']]
        bg = s['floor']
        last = max((i + 1 for i, d in enumerate(levels) if d > bg + 3), default=0)
        return samples, levels, dict(
            utterance_id=s['id'], local_capture=True, speech_started=True,
            endpoint_triggered=end['reason'] == 'silence', speech_start_seconds=0,
            last_active_seconds=last * .02, trailing_silence_ms=(len(levels)-last)*20,
            vad_threshold_dbfs=bg + (3 if mode == 'wake' else 9),
            vad_start_threshold_dbfs=bg + (3 if mode == 'wake' else 9),
            vad_endpoint_threshold_dbfs=bg+3, bg_initial_dbfs=bg, bg_final_dbfs=bg,
            dynamic_floor_enabled=True, bg_trajectory=[])
