"""Save firmware VT1 serial events locally; never flash or contact a server.

SPKS carries no utterance_id. Links below are explicitly inferred and only made
when exactly one pending dialog has an accepted speech verdict before SPKS.
"""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import time

ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
EVENT = re.compile(r'VT1 boot=([0-9a-fA-F]+) seq=(\d+) event=(\w+) epoch=(\d+) id=(\d+) play=(\d+) us=(-?\d+) a=(-?\d+) b=(-?\d+) dropped=(\d+)')
LOSS = re.compile(r'VT_LOSS boot=([0-9a-fA-F]+) dropped=(\d+)')

def parse(line):
    m=EVENT.search(ANSI.sub('',line))
    if not m: return None
    keys=('boot','seq','event','epoch','id','play','us','a','b','dropped')
    return {k:(v.lower() if k in ('boot','event') else int(v)) for k,v in zip(keys,m.groups())}

def delta(end,start):
    return round((end-start)/1000,3) if end is not None and start is not None and end>=start else None

def summarize(events, loss_boots=()):
    """Order by event timestamps for inference, not delayed UART arrival order."""
    turns={}; plays={}; broken=set(loss_boots); previous={}; sessions={}
    for e in events:
        boot=e['boot']; seq=e['seq']
        if e['dropped'] or (boot in previous and seq != previous[boot]+1): broken.add(boot)
        previous[boot]=seq
        key=(boot,e['epoch'])
        if e['event']=='session_end': sessions[key]=e['us']
        if e['id']:
            t=turns.setdefault((*key,e['id']),{})
            # Retransmitted/duplicate diagnostic events do not overwrite first timing.
            if e['event']=='tail_summary': t[e['event']]=e
            else: t.setdefault(e['event'],e)
        elif e['play']:
            plays.setdefault((*key,e['play']),{}).setdefault(e['event'],e)
    links={}; used=set()
    for pk,p in sorted(plays.items(),key=lambda x:x[1].get('spks',{}).get('us',2**63)):
        if 'spks' not in p or pk[0] in broken: continue
        at=p['spks']['us']
        candidates=[]
        for tk,t in turns.items():
            if tk[:2]!=pk[:2] or tk in used: continue
            start=t.get('capture_start'); end=t.get('end_tx'); verdict=t.get('verdict')
            if not start or start['b']!=2 or start['us']>at: continue
            if any(k in t for k in ('capture_abort','enqueue_failed','tx_failed')): continue
            # Noise/empty has an explicit resolution. Missing verdict is unresolved
            # and must block linking a different turn by convenience.
            if verdict and verdict['us']<=at and verdict['a'] in (1,2): continue
            if sessions.get(tk[:2],2**63)<=at: continue
            candidates.append((tk,end,verdict))
        if len(candidates)==1:
            tk,end,verdict=candidates[0]
            if end and end['us']<=at and verdict and verdict['us']<=at and verdict['a']==3:
                links[tk]=pk;used.add(tk)
    rows=[]
    for tk,t in turns.items():
        def stamp(name): return t.get(name,{}).get('us')
        start=t.get('capture_start',{}); end=t.get('capture_end',{})
        pk=links.get(tk); p=plays.get(pk,{})
        def ps(name): return p.get(name,{}).get('us')
        origin=stamp('capture_start');done=p.get('play_done',{})
        if any(k in t for k in ('enqueue_failed','tx_failed')): status='send_failed'
        elif 'capture_abort' in t: status='aborted'
        elif tk[0] in broken: status='telemetry_loss'
        elif done: status='completed' if done['a']==0 else 'playback_failed'
        elif tk[:2] in sessions: status='disconnected'
        elif 'verdict' in t and t['verdict']['a'] in (1,2): status='noise_or_empty'
        else: status='incomplete_or_unlinked'
        # Requested view: capture_start = 0 ms, every column is a time point,
        # not a network/ASR/TTS latency estimate. No RTC or clock synchronization.
        metrics={name+'_ms':delta(stamp(name),origin) for name in (
            'capture_start','first_pcm_queued','start_tx','first_pcm_tx',
            'capture_end','enqueue_end','last_pcm_tx','end_tx','verdict')}
        metrics.update({name+'_ms':delta(ps(name),origin) for name in (
            'spks','first_pcm_rx','first_i2s','spke','play_done')})
        # Failure/loss is retained, never scored as a successful completed exchange.
        if status!='completed': metrics['play_done_ms']=None
        if status=='telemetry_loss': metrics={k:None for k in metrics}
        rows.append(dict(boot=tk[0],epoch=tk[1],utterance_id=tk[2],mode=start.get('b'),
            noise_window_enabled=t.get('gate_state',{}).get('a'),
            noise_ratio_per_mille=t.get('gate_state',{}).get('b'),
            noise_tail_enabled=t.get('tail_config',{}).get('a'),
            noise_tail_entries=t.get('tail_summary',{}).get('a'),
            noise_tail_recoveries=t.get('tail_summary',{}).get('b'),
            recovery_ratio_per_mille=t.get('recovery_shape',{}).get('a'),
            recovery_centroid_hz=t.get('recovery_shape',{}).get('b'),
            noise_tail_end_guard_ms=t.get('tail_guard',{}).get('a'),
            first_noise_tail_ms=delta(stamp('tail_enter'),origin),
            first_noise_recovery_ms=delta(stamp('tail_recover'),origin),
            status=status,link='unique_pending_speech_inferred' if pk else 'unlinked',
            playback_generation=pk[2] if pk else None,**metrics))
    return rows

def playback_timelines(events):
    """Keep all playback observations even when no capture association is possible."""
    groups={}
    for e in events:
        if not e['id'] and e['play']:
            groups.setdefault((e['boot'],e['epoch'],e['play']),{}).setdefault(e['event'],e)
    rows=[]
    for (boot,epoch,play),p in groups.items():
        origin=p.get('spks',{}).get('us')
        rows.append(dict(boot=boot,epoch=epoch,playback_generation=play,origin='spks',
            result=p.get('play_done',{}).get('a'),
            **{name+'_ms':delta(p.get(name,{}).get('us'),origin) for name in
               ('spks','first_pcm_rx','first_i2s','spke','play_done')}))
    return rows

def save_csv(path,rows,empty_fields):
    with path.open('w',newline='',encoding='utf-8-sig') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]) if rows else empty_fields)
        w.writeheader();w.writerows(rows)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    source=ap.add_mutually_exclusive_group(required=True)
    source.add_argument('--port',help='e.g. COM9; closes normally with Ctrl+C')
    source.add_argument('--input',type=Path,help='Reprocess an existing raw serial log')
    ap.add_argument('--baud',type=int,default=115200)
    ap.add_argument('--out',type=Path)
    ap.add_argument('--duration',type=float,default=0,help='Seconds; zero means until Ctrl+C')
    ap.add_argument('--label',default='')
    ap.add_argument('--firmware',type=Path,help='Optional binary whose SHA256 is saved; never flashed')
    args=ap.parse_args()
    if args.duration<0: ap.error('--duration must be nonnegative')
    root=Path(__file__).resolve().parents[2]
    out=args.out or root/'logs/voice_timing'/datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    out.mkdir(parents=True,exist_ok=False)
    meta=dict(started_utc=datetime.now(timezone.utc).isoformat(),port=args.port,baud=args.baud,
              input=str(args.input) if args.input else None,label=args.label,
              clock='firmware monotonic ticks; turns.csv relative to capture_start=0 ms; no RTC',
              firmware_sha256=hashlib.sha256(args.firmware.read_bytes()).hexdigest() if args.firmware else None)
    (out/'metadata.json').write_text(json.dumps(meta,indent=2,ensure_ascii=False),encoding='utf-8')
    stream=None;events=[];loss=set()
    try:
        if args.input: stream=args.input.open('rb')
        else:
            import serial
            stream=serial.Serial(port=None,baudrate=args.baud,timeout=.5)
            stream.dtr=False;stream.rts=False;stream.port=args.port;stream.open()
        print(f'Saving locally: {out.resolve()}',flush=True)
        began=time.monotonic()
        with (out/'raw.log').open('wb') as raw,(out/'events.jsonl').open('w',encoding='utf-8') as parsed:
            while not args.duration or time.monotonic()-began<args.duration:
                line=stream.readline(8192)
                if not line:
                    if args.input: break
                    continue
                raw.write(line);raw.flush()
                text=line.decode('utf-8',errors='replace')
                m=LOSS.search(text)
                if m: loss.add(m.group(1).lower())
                e=parse(text)
                if e:
                    events.append(e);parsed.write(json.dumps(e)+'\n');parsed.flush()
    except KeyboardInterrupt:
        pass
    finally:
        if stream: stream.close()
        rows=summarize(events,loss)
        playback=playback_timelines(events)
        (out/'summary.json').write_text(json.dumps(dict(events=len(events),loss_boots=sorted(loss),turns=rows,playbacks=playback),indent=2),encoding='utf-8')
        save_csv(out/'turns.csv',rows,['boot','epoch','utterance_id','status'])
        save_csv(out/'playbacks.csv',playback,['boot','epoch','playback_generation','result'])
        print(f'{len(events)} timing events, {len(rows)} capture records; summary: {out / "turns.csv"}',flush=True)

if __name__=='__main__': main()
