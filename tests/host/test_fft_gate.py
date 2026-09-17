"""Numerical reference, production C endpoint replay, and byte/order verification.

No cloud inference or speech/noise labels. --corpus points to the existing FFT
analysis directory; source WAV hashes are checked against its manifest.
"""
import argparse
import ctypes as ct
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import wave

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc', required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--numpy-path', type=Path)
p.add_argument('--corpus', type=Path)
p.add_argument('--esp-dsp', action='store_true', help='Run the actual vendored ESP-DSP ANSI kernel on host')
a = p.parse_args()
if a.numpy_path:
    sys.path.insert(0, str(a.numpy_path))
import numpy as np

root = Path(__file__).resolve().parents[2]
src = root/'components/julia_board_audio'
a.out.mkdir(parents=True, exist_ok=True)
dll = a.out/('fft_gate.dll' if sys.platform == 'win32' else 'fft_gate.so')
cmd = [a.cc, '-shared', '-I'+str(src/'include'), str(Path(__file__).with_name('fft_gate_driver.c')),
       str(src/'local_capture.c'), str(src/'lc_spectrum.c'), '-o', str(dll)]
if sys.platform != 'win32':
    cmd += ['-fPIC', '-lm']
elif 'tcc' in Path(a.cc).name.lower():
    cmd += [str(Path(__file__).with_name('fft_math.def'))]
if a.esp_dsp:
    # Only hardware/config shims; FFT, bit reversal and tables are vendor sources.
    shims = a.out/'dsp-host-includes'
    shims.mkdir(exist_ok=True)
    (shims/'sdkconfig.h').write_text('#pragma once\n', encoding='utf-8')
    (shims/'esp_attr.h').write_text('#pragma once\n', encoding='utf-8')
    (shims/'dsp_common.h').write_text('''#pragma once
#include <stdbool.h>
static inline bool dsp_is_power_of_two(int n) { return n > 0 && !(n & (n-1)); }
static inline int dsp_power_of_two(int n) { int p=0; while(n>1) { n>>=1; ++p; } return p; }
''', encoding='utf-8')
    dsp = root/'components/espressif__esp-dsp/modules'
    cmd += ['-DESP_PLATFORM', '-I'+str(shims), '-I'+str(root/'tests/host/stubs'),
            '-I'+str(dsp/'fft/include'), '-I'+str(dsp/'common/include'),
            str(dsp/'fft/float/dsps_fft2r_fc32_ansi.c'),
            str(dsp/'fft/float/dsps_fft2r_bitrev_tables_fc32.c')]
subprocess.run(cmd, check=True)
lib = ct.CDLL(str(dll.resolve()))
ip = ct.POINTER(ct.c_int16)
fp = ct.POINTER(ct.c_float)
lib.gate_features.argtypes = [ip, fp]
lib.gate_psd.argtypes = [fp, fp]
lib.gate_step.argtypes = [ip, ct.POINTER(ct.c_int)]
features = np.zeros(3, dtype=np.float32)
state = np.zeros(10, dtype=np.int32)
window = .5-.5*np.cos(2*np.pi*np.arange(320)/320)

def reference(pcm):
    x = pcm.astype(np.float64)/32768
    psd = abs(np.fft.rfft((x-x.mean(axis=-1, keepdims=True))*window))**2/(16000*sum(window**2))
    psd[..., 1:-1] *= 2
    spec = psd[..., 1:]
    total = spec.sum(axis=-1)
    valid = total > 0
    q = np.divide(spec, total[..., None], out=np.zeros_like(spec), where=valid[..., None])
    flat = np.divide(np.exp(np.log(np.maximum(spec, 1e-30)).mean(axis=-1)), total/160,
                     out=np.zeros_like(total), where=valid)
    entropy = -(q*np.log(np.maximum(q, 1e-30))).sum(axis=-1)/np.log(160)
    band = np.divide(psd[..., 6:80].sum(axis=-1), psd.sum(axis=-1), out=np.zeros_like(total), where=valid)
    return np.stack((flat, entropy, band), axis=-1)

def calc(pcm):
    valid = lib.gate_features(pcm.ctypes.data_as(ip), features.ctypes.data_as(fp))
    assert np.isfinite(features).all()
    return valid, features.copy()

def step(pcm, ok=True):
    assert bool(lib.gate_step(pcm.ctypes.data_as(ip), state.ctypes.data_as(ct.POINTER(ct.c_int)))) == ok
    return state.copy()

def accepted(f):
    return (f[..., 0] <= .199) & (f[..., 1] >= .070) & (f[..., 1] <= .809)

zero = np.zeros(320, dtype=np.int16)
tone = np.rint(2000*np.sin(2*np.pi*1000*np.arange(320)/16000)).astype(np.int16)
rng = np.random.default_rng(916)
noise = rng.integers(-6000, 6001, 320, dtype=np.int16)
impulse = zero.copy(); impulse[160] = 16000
lib.gate_reset(1, 2)
signals = [zero, np.full(320, 2000, np.int16), tone, noise, impulse,
           np.where(np.arange(320)%2, -32000, 32000).astype(np.int16)]
signals += [rng.integers(-32768, 32768, 320, dtype=np.int16) for _ in range(100)]
errors = []
for pcm in signals:
    valid, actual = calc(pcm)
    expected = reference(pcm)
    errors.append(abs(actual-expected))
    assert np.allclose(actual, expected, atol=2e-5, rtol=2e-4), (actual, expected)
    assert bool(valid) == bool(np.ptp(pcm.astype(np.int32)))
assert accepted(calc(tone)[1]) and not accepted(calc(noise)[1])
for value in (0, -1, float('nan'), float('inf')):
    psd = np.zeros(161, np.float32); psd[20] = value
    assert not lib.gate_psd(psd.ctypes.data_as(fp), features.ctypes.data_as(fp))
    assert np.isfinite(features).all() and not features.any()

# Onset suppression, rolling window, preroll preservation, rejected-frame tail.
for mode, need, tail, maximum in ((1, 1, 25, 400), (2, 6, 35, 750)):
    lib.gate_reset(1, mode)
    for _ in range(100): assert step(noise)[0] == 0
    lib.gate_reset(1, mode)
    for _ in range(25): step(noise)
    for _ in range(need-1): assert step(tone)[0] == 0
    s = step(tone); assert s[0] == 1 and s[1] == 25
    for _ in range(tail-1): assert step(noise)[2] == 0
    s = step(noise); assert s[2] == 1 and s[3] == 0 and s[7] == 25+tail
    # New segment IDs/indexing and unchanged maximum, including preroll.
    for _ in range(need): s = step(tone)
    assert s[0] == 2
    while not s[3]: s = step(tone)
    assert s[7] == maximum
    lib.gate_reset(1, mode)
    for _ in range(need): step(tone)
    lib.gate_mode(3-mode)
    assert step(zero)[4] == 1
    # START, AUDIO (including preroll), END and ABORT failure paths.
    for event in (0, 1, 2, 3):
        lib.gate_reset(1, mode)
        if event < 2:
            for _ in range(need-1): step(tone)
            lib.gate_reject(event); step(tone, False)
        else:
            for _ in range(need): step(tone)
            lib.gate_reject(event)
            if event == 2:
                for _ in range(tail-1): step(zero)
                step(zero, False)
            else:
                lib.gate_mode(3-mode)  # Existing mode switch resets failure state.
                assert step(zero)[5] == 0
                continue
        step(tone, False)
lib.gate_reset(1, 2)
for _ in range(5): step(tone)
for _ in range(15): step(zero)
assert step(tone)[0] == 0
lib.gate_reset(1, 2); lib.gate_unready(); step(tone, False)

# Reproduce 40ms quiet + 20ms high-energy noise: old limit, new 700ms tail.
tails = {}
for enabled in (0, 1):
    lib.gate_reset(enabled, 2)
    for _ in range(6): step(tone)
    for i in range(750):
        s = step(noise if i%3 == 2 else zero)
        if s[2]: break
    tails[str(enabled)] = dict(tail_ms=(i+1)*20, limit=bool(s[3]), frames=int(s[7]))
assert tails['0']['limit'] and tails['1']['tail_ms'] == 700 and not tails['1']['limit']
report = dict(synthetic_max_abs_error=np.max(errors, axis=0).tolist(), synthetic_tests='passed',
              backend='ESP-DSP ANSI' if a.esp_dsp else 'portable radix-2',
              periodic_noise= tails, thresholds=dict(flatness_max=.199, entropy_min=.070, entropy_max=.809),
              fft_size=320, bins=160, hardware_timing='not measured')

if a.corpus:
    config = json.loads((a.corpus/'summary.json').read_text(encoding='utf-8'))
    manifest = json.loads((a.corpus/'source_manifest.json').read_text(encoding='utf-8'))
    records = []; max_error = np.zeros(3); decisions = 0; total = selected = passed = all_passed = 0
    for entry in manifest:
        path = Path(config['source_directory'])/entry['source']
        assert hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256']
        with wave.open(str(path)) as wav:
            assert (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) == (16000, 1, 2)
            pcm = np.frombuffer(wav.readframes(wav.getnframes()), dtype='<i2')
        frames = pcm[:len(pcm)//320*320].reshape(-1, 320)
        expected = reference(frames)
        # Reconcile the new NumPy reference with the pre-existing saved 320-point PSD.
        with np.load(a.corpus/'per_file'/Path(entry['source']).with_suffix('.npz')) as saved:
            psd = saved['psd_fs2_per_hz'][:len(frames)]
            spec = psd[:, 1:]
            sums = spec.sum(axis=1)
            valid = sums > 0
            q = np.divide(spec, sums[:, None], out=np.zeros_like(spec), where=valid[:, None])
            flat = np.exp(np.log(np.maximum(spec, 1e-30)).mean(axis=1))/np.maximum(sums/160, 1e-30)
            entropy = -(q*np.log(np.maximum(q, 1e-30))).sum(axis=1)/np.log(160)
            assert np.allclose(expected[valid, :2], np.stack((flat, entropy), axis=1)[valid], atol=1e-9)
        lib.gate_reset(1, 2)
        actual = np.array([calc(frame)[1] for frame in frames])
        max_error = np.maximum(max_error, np.max(abs(actual-expected), axis=0))
        # Geometric means are sensitive to near-zero FFT bins in float32.
        # Allow 1e-3 absolute flatness error; verify every gate decision separately.
        assert np.allclose(actual, expected, atol=[1e-3, 2e-5, 2e-5], rtol=2e-4), (entry['source'],
            np.max(abs(actual-expected), axis=0),
            actual[np.argmax(np.max(abs(actual-expected), axis=1))],
            expected[np.argmax(np.max(abs(actual-expected), axis=1))])
        decisions += int(np.count_nonzero(accepted(actual) != accepted(expected)))
        energy = np.sqrt(np.mean((frames.astype(float)/32768)**2, axis=1)) >= 10**(-50/20)
        total += len(frames); selected += int(energy.sum())
        passed += int((accepted(actual)&energy).sum()); all_passed += int(accepted(actual).sum())
        row = dict(source=entry['source'], frames=len(frames), modes={})
        for mode in (1, 2):
            for enabled in (0, 1):
                lib.gate_reset(enabled, mode)
                segments = []; start = None
                for idx, frame in enumerate([*frames, *([zero]*40)]):
                    s = step(frame)
                    if s[0] > len(segments) and start is None:
                        start = dict(trigger=idx, first=int(s[6]))
                    if s[2] > len(segments):
                        segments.append(dict(**start, end=idx, frames=int(s[7]), limit=bool(s[3])))
                        start = None
                assert start is None
                row['modes'][f'{mode}_{enabled}'] = segments
        records.append(row)
    summaries = {}
    for mode in (1, 2):
        for enabled in (0, 1):
            key = f'{mode}_{enabled}'; segs = [s for r in records for s in r['modes'][key]]
            summaries[key] = dict(segments=len(segs), files_without_start=sum(not r['modes'][key] for r in records),
                limit_ends=sum(s['limit'] for s in segs), uploaded_frames=sum(s['frames'] for s in segs),
                ends_before_source_eof=sum(s['end']<r['frames'] for r in records for s in r['modes'][key]))
        pairs = [(r['modes'][f'{mode}_0'], r['modes'][f'{mode}_1']) for r in records]
        summaries[f'{mode}_change'] = dict(files_changed=sum(old != new for old,new in pairs),
            first_trigger_delay_ms=[(new[0]['trigger']-old[0]['trigger'])*20 for old,new in pairs if old and new],
            last_end_delta_ms=[(new[-1]['end']-old[-1]['end'])*20 for old,new in pairs if old and new])
    assert decisions == 0, f'{decisions} gate decisions differ from float64 reference'
    report['corpus'] = dict(files=len(records), full_frames=total, selected_frames=selected,
        saved_320_psd_reference='matched within 1e-9',
        selected_passed=passed, selected_pass_percent=100*passed/selected,
        all_passed=all_passed, all_pass_percent=100*all_passed/total,
        max_abs_error=max_error.tolist(), gate_decision_mismatches=decisions, replay=summaries,
        assumptions='Independent WAVs; initial floor -60dBFS; no gain or cloud verdict; 800ms zero tail; incomplete frames excluded.',
        limitations='No frame speech labels or real noise controls; early ends/splits are observations, not proven speech truncation.')
    (a.out/'replay_segments.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
(a.out/'validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(f"{report['backend']}: synthetic tests passed; report: {a.out/'validation.json'}")
if a.corpus:
    print(f'{len(records)} WAVs, {total} frames; selected pass {passed}/{selected}; decision mismatches={decisions}')
