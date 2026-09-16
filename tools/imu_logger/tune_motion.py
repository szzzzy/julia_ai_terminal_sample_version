#!/usr/bin/env python3
"""Replay IMU CSVs with separate A/G consecutive confirmation. No device changes.

python tools/imu_logger/tune_motion.py
python tools/imu_logger/tune_motion.py --gyro-dps 800 --confirm-frames 1
python tools/imu_logger/tune_motion.py --scan
"""
from __future__ import annotations
import argparse
import bisect
import csv
from collections import deque
from dataclasses import asdict, dataclass, replace
import hashlib
import itertools
import json
import math
from pathlib import Path
import re
from imu_capture import AXES, parse_record

ROOT = Path(__file__).resolve().parents[2]
KEYS = dict(sample_ms='MOTION_SAMPLE_MS', confirm_frames='MOTION_CONFIRM_FRAMES',
            cooldown_ms='MOTION_COOLDOWN_MS', accel_mg='ACCEL_DELTA_MG',
            gyro_dps='GYRO_THRESHOLD_DPS')


@dataclass(frozen=True)
class Params:
    sample_ms: int = 10
    confirm_frames: int = 10
    cooldown_ms: int = 10000
    accel_mg: float | None = 750
    gyro_dps: float | None = 90


def threshold(value):
    if str(value).lower() == 'off':
        return None
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError('threshold must be positive, or off')
    return result


def load_params(path):
    text = Path(path).read_text(encoding='utf-8')
    values = {}
    for name, suffix in KEYS.items():
        match = re.search(r'^CONFIG_JULIA_IMU_' + suffix + r'=(\d+)\s*$', text, re.M)
        if not match:
            raise ValueError(f'missing CONFIG_JULIA_IMU_{suffix} in {path}')
        values[name] = int(match[1])
    return Params(**values)


def sampled_features(rows, period_ms, phase_ms):
    """Causal last-observation hold; recompute accel delta AFTER polling resample.

    No interpolation/anti-alias filter is inserted: production polls registers.
    Reset the baseline across unknown sensor-counter gaps. No tail extrapolation.
    """
    times = [row['t_us'] for row in rows]
    if not times:
        return []
    tick = times[0] + round(phase_ms * 1000)
    previous = None
    previous_index = None
    # Prefix count of data gaps, so a slow polling interval cannot hide a gap.
    gaps = [0]
    for a, b in zip(rows, rows[1:]):
        gaps.append(gaps[-1] + (((b['sensor_counter']-a['sensor_counter']) & 0xffffff) != 1))
    features = []
    while tick <= times[-1]:
        i = bisect.bisect_right(times, tick) - 1
        row = rows[i]
        gap = previous_index is not None and gaps[i] != gaps[previous_index]
        if previous is None or gap:
            features.append((tick / 1000, None, None))
        else:
            accel = sum(abs(row[k] - previous[k]) for k in AXES[:3]) * 1000
            gyro = math.sqrt(sum(row[k] ** 2 for k in AXES[3:]))
            features.append((tick / 1000, accel, gyro))
        previous, previous_index = row, i
        tick += period_ms * 1000
    return features


def replay(features, params, continuous=False, combined_or=False):
    streak = 0
    max_streak = 0
    accel_streak = gyro_streak = 0
    window = deque(maxlen=params.confirm_frames)
    cooldown_until = -1
    baseline_needed = False
    triggers = []
    for ms, accel, gyro in features:
        if ms < cooldown_until:
            streak = accel_streak = gyro_streak = 0
            window.clear()
            baseline_needed = True
            continue
        if baseline_needed or accel is None:
            streak = accel_streak = gyro_streak = 0
            window.clear()
            baseline_needed = False
            continue
        a = params.accel_mg is not None and accel >= params.accel_mg
        g = params.gyro_dps is not None and gyro >= params.gyro_dps
        window.append((a, g))
        streak = streak + 1 if a or g else 0
        accel_streak = accel_streak + 1 if a else 0
        gyro_streak = gyro_streak + 1 if g else 0
        confirmed = streak if combined_or else max(accel_streak, gyro_streak)
        max_streak = max(max_streak, confirmed)
        if confirmed >= params.confirm_frames:
            triggers.append(dict(time_s=ms / 1000,
                                 accel_hits=sum(x[0] for x in window),
                                 gyro_hits=sum(x[1] for x in window)))
            if not continuous:
                break  # Product normally leaves S3/S5/S6 for S4 at this point.
            cooldown_until = ms + params.cooldown_ms
            baseline_needed = True
            streak = accel_streak = gyro_streak = 0
            window.clear()
    return dict(triggered=bool(triggers), first_trigger_s=triggers[0]['time_s'] if triggers else None,
                max_streak=max_streak, triggers=triggers)


def evaluate(records, params, phases, continuous=False, combined_or=False):
    result = []
    false_positive = false_negative = 0
    for record in records:
        outcomes = [replay(features, params, continuous, combined_or) for features in record['features']]
        hits = sum(o['triggered'] for o in outcomes)
        positive = record['positive']
        # Conservative per-record criterion across tested phase offsets.
        if positive and hits != phases:
            false_negative += 1
        if not positive and hits:
            false_positive += 1
        times = [o['first_trigger_s'] for o in outcomes if o['triggered']]
        result.append(dict(file=record['file'], label=record['meta']['label'], expected=positive,
                           phases_triggered=hits, phases=phases,
                           first_s_min=min(times) if times else None,
                           first_s_max=max(times) if times else None,
                           outcomes=outcomes))
    return dict(params=asdict(params), confirmation_rule='combined_or' if combined_or else 'either_axis_consecutive',
                false_positive_records=false_positive,
                false_negative_records=false_negative, records=result)


def show(result):
    print('Parameters:', json.dumps(result['params']))
    print('Confirmation:', result['confirmation_rule'])
    print(f'{"action":24} {"expected":8} {"trigger phases":15} {"first time(s)":15} {"A/G hits (first phase)"}')
    for row in result['records']:
        times = '-' if row['first_s_min'] is None else f'{row["first_s_min"]:.3f}..{row["first_s_max"]:.3f}'
        event = row['outcomes'][0]['triggers']
        branches = f'{event[0]["accel_hits"]}/{event[0]["gyro_hits"]}' if event else '-'
        print(f'{row["label"]:24} {"YES" if row["expected"] else "NO":8} '
              f'{str(row["phases_triggered"])+"/"+str(row["phases"]):15} {times:15} {branches}')
    print(f'False-trigger records: {result["false_positive_records"]}; missed records: {result["false_negative_records"]}')


def parse_values(text, current):
    """Five values in config order; blank keeps all, '-' keeps one field."""
    if not text.strip():
        return current
    parts = text.replace(',', ' ').replace('，', ' ').split()
    if len(parts) != 5:
        raise ValueError('请输入 5 个值：采样间隔 连续次数 冷却时间 加速度阈值 角速度阈值')
    values = {}
    for i, (key, part) in enumerate(zip(KEYS, parts)):
        values[key] = getattr(current, key) if part == '-' else (int(part) if i < 3 else threshold(part))
    result = Params(**values)
    if result.sample_ms < 1 or result.confirm_frames < 1 or result.cooldown_ms < 0:
        raise ValueError('采样间隔和连续次数必须大于 0，冷却时间不能小于 0')
    return result


def interactive(records, args, params):
    print('\n直接输入 5 个值，空格分隔：')
    print('采样间隔(ms)  连续次数  冷却时间(ms)  加速度阈值(mg)  角速度阈值(dps)')
    print('例如：10 10 10000 750 500')
    print('回车运行当前值；- 保留对应值；阈值可填 off；q 退出。不会修改 sdkconfig。')
    while True:
        current = ' '.join('off' if v is None else f'{v:g}' for v in asdict(params).values())
        try:
            line = input(f'参数 [{current}] > ').strip()
            if line.lower() in ('q', 'quit', 'exit'):
                return
            proposed = parse_values(line, params)
        except (EOFError, KeyboardInterrupt):
            print()
            return
        except (ValueError, argparse.ArgumentTypeError) as exc:
            print(f'输入有误：{exc}')
            continue
        params = proposed
        run_once(records, args, params)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--records', type=Path, default=ROOT / 'imu_records/latest')
    parser.add_argument('--config', type=Path, default=ROOT / 'sdkconfig')
    parser.add_argument('--sample-ms', type=int)
    parser.add_argument('--confirm-frames', type=int)
    parser.add_argument('--cooldown-ms', type=int)
    parser.add_argument('--accel-mg', type=threshold, help='number or off (diagnostic only)')
    parser.add_argument('--gyro-dps', type=threshold, help='number or off (diagnostic only)')
    parser.add_argument('--phases', type=int, default=3, help='polling phase offsets to check (default 3)')
    parser.add_argument('--continuous', action='store_true', help='hypothetically stay monitoring after trigger, to test cooldown')
    parser.add_argument('--combined-or', action='store_true', help='compare the old firmware rule: A/G may alternate to confirm')
    parser.add_argument('--scan', action='store_true', help='scan a small threshold/confirmation grid; never edit firmware')
    parser.add_argument('--interactive', action='store_true', help='keep entering parameter values (default without overrides/scan)')
    parser.add_argument('--once', action='store_true', help='run current parameters once without prompting')
    parser.add_argument('--output', type=Path, default=ROOT / 'imu_records/tuning')
    args = parser.parse_args()
    params = load_params(args.config)
    overrides = {k: getattr(args, k) for k in KEYS if getattr(args, k) is not None}
    # Distinguish an explicitly supplied off from an absent override.
    import sys
    for option, key in [('--accel-mg', 'accel_mg'), ('--gyro-dps', 'gyro_dps')]:
        if option in sys.argv or any(a.startswith(option + '=') for a in sys.argv):
            overrides[key] = getattr(args, key)
    params = replace(params, **overrides)
    if params.sample_ms < 1 or params.confirm_frames < 1 or params.cooldown_ms < 0 or not 1 <= args.phases <= 20:
        parser.error('positive sample/confirmation values, nonnegative cooldown, and 1..20 phases required')
    records = []
    for path in sorted(args.records.glob('*.csv')):
        data = path.read_bytes()
        meta, rows = parse_record(data)
        label = meta['label'].lower()
        if not (label.startswith('walk') or 'shake' in label or 'hand' in label):
            parser.error(f'unknown expected label {label}; this experiment assumes walk=NO, hand/shake=YES')
        record = dict(file=str(path.resolve()), meta=meta, rows=rows, positive=not label.startswith('walk'),
                      sha256=hashlib.sha256(data).hexdigest(),
                      features=[sampled_features(rows, params.sample_ms, params.sample_ms*i/args.phases)
                                for i in range(args.phases)])
        records.append(record)
    if not records:
        parser.error(f'no CSV files in {args.records}')
    print('MODEL: replay recorded sensor profile with ideal periodic polling; no new sensor ODR/filter simulation.')
    print('First time is relative to recording start, NOT annotated action latency.')
    print('Cooldown affects repeat triggers only. Default stops after first trigger (entry into S4).')
    if not args.combined_or:
        print('NEW RULE: A or G must independently reach N consecutive hits; production firmware is not changed.')
    print('WARNING: product +/-64 dps ODR~30Hz differs from logger; high gyro thresholds need matching hardware settings.')
    if args.continuous:
        print('CONTINUOUS is hypothetical: ignores the real FSM leaving monitoring after trigger.')
    for r in records:
        m = r['meta']
        if m['clipped_samples'] or m['missed_samples'] or m['read_errors']:
            print(f'Quality {m["label"]}: clipped={m["clipped_samples"]}, missed={m["missed_samples"]}, errors={m["read_errors"]}')
    if args.interactive or (not overrides and not args.scan and not args.once):
        interactive(records, args, params)
    else:
        run_once(records, args, params)


def run_once(records, args, params):
    print('Phases:', ', '.join(f'{params.sample_ms*i/args.phases:.3f} ms' for i in range(args.phases)))
    # Period changes alter both sample selection and adjacent-sample delta.
    for record in records:
        record['features'] = [sampled_features(record['rows'], params.sample_ms,
                              params.sample_ms*i/args.phases) for i in range(args.phases)]
    result = evaluate(records, params, args.phases, args.continuous, args.combined_or)
    show(result)
    args.output.mkdir(parents=True, exist_ok=True)
    result['sources'] = [dict(file=r['file'], sha256=r['sha256'], metadata=r['meta']) for r in records]
    result['model'] = 'Causal last-observation polling; gaps reset baseline; no startup settle, playback or network/FSM simulation.'
    result['continuous'] = args.continuous
    (args.output / 'replay.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    if args.scan:
        candidates = []
        gyros = sorted({90, 300, 500, 600, 700, 800, 900} | ({params.gyro_dps} if params.gyro_dps else set()))
        accels = sorted({750, 1500, 3000} | ({params.accel_mg} if params.accel_mg else set())) + [None]
        frames = sorted({1, 4, 10, 15, 20, params.confirm_frames})
        for a, g, n in itertools.product(accels, gyros, frames):
            p = replace(params, accel_mg=a, gyro_dps=g, confirm_frames=n)
            tested = evaluate(records, p, args.phases, combined_or=args.combined_or)
            candidates.append(dict(**asdict(p), rule=tested['confirmation_rule'], fp=tested['false_positive_records'], fn=tested['false_negative_records']))
        candidates.sort(key=lambda r: (r['fp'], r['fn'], r['confirm_frames'], r['gyro_dps'], r['accel_mg'] or float('inf')))
        with (args.output / 'scan.csv').open('w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=list(candidates[0])); writer.writeheader(); writer.writerows(candidates)
        passed = [r for r in candidates if r['fp'] == r['fn'] == 0]
        print(f'\n{len(passed)}/{len(candidates)} candidates separate all records at all tested phases (same-data fit only).')
        print('First 12 by fewest confirmation samples, then gyro threshold; not a reliability ranking:')
        for row in passed[:12]:
            print(f'  accel={row["accel_mg"] if row["accel_mg"] is not None else "off"} mg, '
                  f'gyro={row["gyro_dps"]} dps, confirm={row["confirm_frames"]}')
    print('Results:', args.output.resolve())


if __name__ == '__main__':
    main()
