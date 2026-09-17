"""Extend the saved S0002 FFT with inspectable speech features; never edit sources.

Uses NumPy only. Outputs full-frame features and machine-readable feature
references. Speech recordings are not frame-level speech labels.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import sys
import wave


DEFINITIONS = {
    'rms_dbfs': ('dBFS', '原始 PCM 的帧 RMS，20log10(RMS)，沿用原分析的静音下限'),
    'zcr': ('比例', '相邻 PCM 样本符号改变的次数 / 319；零按非负处理'),
    'crest_factor_db': ('dB', '20log10(帧绝对峰值/RMS)，全零帧无定义'),
    'centroid_hz': ('Hz', '以帧 PSD 为权重的频率均值'),
    'bandwidth_hz': ('Hz', '以 PSD 为权重、相对频谱质心的频率标准差'),
    'rolloff95_hz': ('Hz', '累积频谱能量首次达到 95% 的频点'),
    'flatness': ('比值', '50–8000 Hz PSD 的几何均值/算术均值；log 前下限 1e-30 FS²/Hz'),
    'spectral_entropy': ('归一化熵', '50–8000 Hz 归一化 PSD 的 −Σp ln(p)/ln(160)'),
    'flux_l2': ('距离', '相邻完整帧的50–8000 Hz归一化PSD向量的L2距离；首帧无定义'),
    'rms_change_db': ('dB', '当前帧 RMS dBFS 减前帧；首帧无定义'),
    'rms_std_200ms_db': ('dB', '最近10个完整帧RMS dBFS的总体标准差；不足10帧无定义'),
    'band_0_300_pct': ('%', '0≤f<300 Hz 的 PSD 占全部0–8000 Hz PSD之比'),
    'band_300_1000_pct': ('%', '300≤f<1000 Hz 的 PSD 占比'),
    'band_1000_2000_pct': ('%', '1000≤f<2000 Hz 的 PSD 占比'),
    'band_2000_4000_pct': ('%', '2000≤f<4000 Hz 的 PSD 占比'),
    'band_4000_8000_pct': ('%', '4000≤f≤8000 Hz 的 PSD 占比'),
    'f0_hz': ('Hz', '复用原 YIN 风格估计；无可靠周期时缺失，不填0'),
    'yin_confidence': ('分数', '复用1−CMNDF，仅真实上下文有效时统计；不是人声概率'),
}


def quantiles(np, values):
    values = values[np.isfinite(values)]
    if not len(values):
        return dict(n=0, p05=None, median=None, p95=None)
    a, b, c = np.quantile(values, [.05, .5, .95])
    return dict(n=len(values), p05=float(a), median=float(b), p95=float(c))


def compute(np, pcm, saved):
    n = len(pcm) // 320
    x = pcm[:n*320].reshape(n, 320).astype(np.float64) / 32768
    complete = saved['complete_frame']
    assert int(complete.sum()) == n and np.all(complete[:n])
    psd = saved['psd_fs2_per_hz'][:n]
    frequency = saved['frequency_hz']
    total = psd.sum(axis=1)
    valid = total > 0
    probability = np.divide(psd, total[:, None], out=np.zeros_like(psd), where=valid[:, None])
    centroid = (probability * frequency).sum(axis=1)
    bandwidth = np.sqrt((probability * (frequency - centroid[:, None]) ** 2).sum(axis=1))
    centroid[~valid] = bandwidth[~valid] = np.nan
    assert np.allclose(centroid, saved['centroid_hz'][:n], equal_nan=True)
    spec = psd[:, frequency >= 50]
    spec_sum = spec.sum(axis=1)
    spec_valid = spec_sum > 0
    q = np.divide(spec, spec_sum[:, None], out=np.zeros_like(spec), where=spec_valid[:, None])
    safe = np.maximum(spec, 1e-30)
    flatness = np.exp(np.log(safe).mean(axis=1)) / safe.mean(axis=1)
    entropy = -(q * np.log(np.maximum(q, 1e-30))).sum(axis=1) / np.log(q.shape[1])
    flatness[~spec_valid] = entropy[~spec_valid] = np.nan
    flux = np.full(n, np.nan)
    if n > 1:
        flux[1:] = np.sqrt(((q[1:] - q[:-1]) ** 2).sum(axis=1))
        flux[1:][~(spec_valid[1:] & spec_valid[:-1])] = np.nan
    rms = saved['rms_fs'][:n]
    db = 20*np.log10(np.maximum(rms, 1e-15))
    peak = np.max(np.abs(x), axis=1)
    crest = np.full(n, np.nan)
    good = rms > 0
    crest[good] = 20*np.log10(peak[good]/rms[good])
    std = np.full(n, np.nan)
    if n >= 10:
        std[9:] = np.std(np.lib.stride_tricks.sliding_window_view(db, 10), axis=1)
    f0 = saved['f0_hz'][:n].copy()
    confidence = saved['yin_confidence'][:n].copy()
    confidence[~saved['f0_context_valid'][:n]] = np.nan
    features = dict(rms_dbfs=db, zcr=np.mean((x[:, 1:] >= 0) != (x[:, :-1] >= 0), axis=1),
                    crest_factor_db=crest, centroid_hz=centroid, bandwidth_hz=bandwidth,
                    rolloff95_hz=saved['rolloff95_hz'][:n], flatness=flatness,
                    spectral_entropy=entropy, flux_l2=flux,
                    rms_change_db=np.r_[np.nan, np.diff(db)], rms_std_200ms_db=std)
    for lo, hi in ((0,300),(300,1000),(1000,2000),(2000,4000),(4000,8000)):
        mask = (frequency >= lo) & ((frequency < hi) if hi < 8000 else (frequency <= hi))
        value = probability[:, mask].sum(axis=1)*100
        value[~valid] = np.nan
        features[f'band_{lo}_{hi}_pct'] = value
    features.update(f0_hz=f0, yin_confidence=confidence)
    assert list(features) == list(DEFINITIONS)
    bands = sum(features[k] for k in features if k.startswith('band_'))
    assert np.allclose(bands[valid], 100)
    for key in ('zcr', 'flatness', 'spectral_entropy'):
        values = features[key][np.isfinite(features[key])]
        assert np.all((values >= -1e-10) & (values <= 1+1e-10)), key
    return features


def write_report(out, summary):
    stats = summary['statistics']['energy_selected_frames']
    def f(v):
        return '—' if v is None else f'{v:.4g}'
    lines = ['# S0002 人声录音声学特征分析（阶段性报告）', '',
        '本阶段已完成单类人声录音的特征提取与分布统计，目标是为固件侧声音准入提供候选特征。'
        '本报告尚未进行人声/噪声分类实验，不声明噪声过滤率或语音识别准确率。', '',
        '## 1. 数据与方法', '',
        f"本批包含 {summary['files']} 条 WAV，总时长 {summary['duration_s']:.2f} 秒（{summary['duration_s']/60:.2f} 分钟）。"
        f"格式为16 kHz、单声道、PCM16。共分析 {summary['full_frames']:,} 个完整20 ms帧。",
        '文件名均带 S0002 标识；该批样本覆盖范围有限，不视为跨说话人泛化验证。',
        '复用已有320点、20 ms、无重叠、周期Hann窗FFT/PSD。频点间隔50 Hz。频域分析沿用逐帧去均值；'
        'RMS、过零率和峰均比根据原始PCM统计。补零尾帧不参与本报告。',
        '基频沿用原分析的60 ms真实上下文YIN风格估计，不把FFT最大峰当作基频。',
        f"额外以RMS≥−50 dBFS选出 {summary['energy_selected_frames']:,} 个活动帧作为主要统计子集。"
        '这是响度筛选，不能视为逐帧人声真值。每个被选帧等权；P05–P95为经验分位范围，不是置信区间。', '',
        '## 2. 特征定义', '', '| 特征 | 单位 | 定义 |', '|---|---|---|']
    lines += [f'| `{key}` | {unit} | {definition} |' for key,(unit,definition) in DEFINITIONS.items()]
    lines += ['', '## 3. 活动帧统计结果', '', '| 特征 | 有效帧数 | P05 | 中位数 | P95 |', '|---|---:|---:|---:|---:|']
    lines += [f'| `{key}` | {value["n"]:,} | {f(value["p05"])} | {f(value["median"])} | {f(value["p95"])} |'
              for key,value in stats.items()]
    lines += ['', '## 4. 可写入阶段汇报的结果', '',
        f"在本批活动帧中，谱平坦度中位数为 {f(stats['flatness']['median'])}，"
        f"P05–P95 为 {f(stats['flatness']['p05'])}–{f(stats['flatness']['p95'])}；"
        f"归一化谱熵中位数为 {f(stats['spectral_entropy']['median'])}。这些统计量描述频谱集中程度，"
        '可作为后续候选特征，但尚未验证它们与空调、键盘或敲桌噪声的可分性。',
        f"可靠F0候选共 {summary['voiced_candidate_frames']:,} 帧，活动帧中无可靠F0的比例为 "
        f"{100*(1-summary['voiced_candidate_frames']/summary['energy_selected_frames']):.2f}%。"
        '无F0不等于噪声，清辅音、停顿和估计失败均可能造成缺失；不应将周期性作为唯一准入条件。',
        '现有能量统计增加了过零率、峰均比、频谱结构和跨帧变化描述，可用于观察声音的强度、周期性与时间连续性。'
        '200 ms电平标准差只是观测特征，并未作为新底噪跟踪器接入正式固件。', '',
        '## 5. 结论与边界', '',
        '已完成18项逐帧声学指标提取、逐文件统计、全量/活动/候选浊音三种口径的分位数汇总。'
        '可以据此排查会大量排除现有人声录音帧的候选规则，但不能只用这批单类数据确定噪声剔除阈值。',
        '尚未完成：真实噪声对照、逐帧人工语音标签、跨说话人验证、固件运行耗时测试、误触/漏触率验证。'
        '当前数值受录音增益、距离和场景影响，不直接迁移为产品硬门限。', '',
        '## 6. 数据与复现', '',
        '- `frame_features.csv`：所有完整帧的18项特征、活动标记、候选浊音标记。',
        '- `file_features.csv`：每文件、每种统计口径、每项特征的P05/中位数/P95。',
        '- `feature_statistics.csv`：全局三种统计口径下的分位数，便于粘贴到报告。',
        '- `summary.json`：统计、算法配置、定义、输入SHA-256与提取脚本SHA-256。',
        '- `feature_extraction.ipynb`：可复跑入口。源音频及原FFT结果均未改写。', '',
        '## 7. 参考依据', '',
        '- Jurafsky & Martin, *Speech and Language Processing*, Chapter 14, Phonetics and Speech Feature Extraction：'
        '[作者公开章节](https://web.stanford.edu/~jurafsky/slp3/old_aug25/14.pdf)。用于短时分析、频谱与发声结构的说明。',
        '- Schafer & Rabiner (1975), *Digital Representations of Speech Signals*, Proceedings of the IEEE：'
        '[作者高校全文](https://web.ece.ucsb.edu/Faculty/Rabiner/ece259/Reprints/081_digital_representations_speech.pdf)。'
        '用于短时能量、过零率、自相关和频域特征的依据。',
        '谱熵、谱平坦度、谱通量及200 ms标准差的具体计算口径以本报告第2节和附带代码为准；不声称复现某篇论文的完整检测器。', '']
    (out/'REPORT.md').write_text('\n'.join(lines), encoding='utf-8')
    return '\n'.join(lines)


def parameter_reference(summary):
    return dict(schema_version=1, purpose='speech-feature reference, not validated gate thresholds',
        extraction=dict(sample_rate_hz=16000, frame_samples=320, frame_ms=20,
                        hop_samples=320, fft_size=320, window='periodic Hann',
                        spectral_remove_frame_mean=True, zcr_uses_original_pcm=True,
                        spectral_shape_min_hz=50, spectral_shape_max_hz=8000,
                        psd_log_floor=1e-30, temporal_std_frames=10,
                        temporal_std_ddof=0, temporal_std_window_ms=200,
                        frame_selection_dbfs=-50),
        scope=dict(files=summary['files'],full_frames=summary['full_frames'],
                   energy_selected_frames=summary['energy_selected_frames'],
                   voiced_candidate_frames=summary['voiced_candidate_frames']),
        feature_definitions=summary['feature_definitions'],
        energy_selected_reference=summary['statistics']['energy_selected_frames'],
        voiced_candidate_reference=summary['statistics']['voiced_candidates'],
        f0_extraction={k:v for k,v in summary['source_fft_config'].items() if k.startswith(('f0_', 'yin_'))},
        gate=dict(enabled=False, thresholds=None,
                  reason='Only positive recordings; quantile intervals are not hard gates, noise models or confidence intervals.'),
        provenance=dict(extractor_sha256=summary['script_sha256'],
                        source_files=summary['source_files']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--numpy-path', type=Path)
    parser.add_argument('--report', action='store_true', help='optionally create a narrative report draft')
    args = parser.parse_args()
    if args.numpy_path:
        sys.path.insert(0, str(args.numpy_path))
    import numpy as np
    source = json.loads((args.input/'summary.json').read_text(encoding='utf-8'))
    assert source['config']['frame_samples'] == source['config']['hop_samples'] == 320
    assert source['config']['rate_hz'] == 16000
    expected = {r['source']:r for r in json.loads((args.input/'source_manifest.json').read_text(encoding='utf-8'))}
    args.output.mkdir(parents=True, exist_ok=False)
    pools = {name:{key:[] for key in DEFINITIONS} for name in ('all_full_frames','energy_selected_frames','voiced_candidates')}
    counts = dict(files=0, full_frames=0, energy_selected_frames=0, voiced_candidate_frames=0)
    manifest = []
    per_file = []
    with (args.output/'frame_features.csv').open('w', newline='', encoding='utf-8-sig') as output:
        writer = csv.writer(output)
        writer.writerow(['source','frame','start_s','energy_selected','voiced_candidate',*DEFINITIONS])
        for name, entry in expected.items():
            path = Path(source['source_directory'])/name
            data = path.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            assert digest == entry['sha256'] and len(data) == entry['bytes'], name
            with wave.open(io.BytesIO(data)) as wav:
                assert (wav.getframerate(),wav.getnchannels(),wav.getsampwidth(),wav.getcomptype()) == (16000,1,2,'NONE')
                pcm = np.frombuffer(wav.readframes(wav.getnframes()), dtype='<i2')
            npz_path = args.input/'per_file'/Path(name).with_suffix('.npz')
            with np.load(npz_path) as saved:
                features = compute(np, pcm, saved)
                n = len(features['rms_dbfs'])
                active = saved['active_above_minus50_dbfs'][:n]
                voiced = saved['voiced_candidate'][:n]
                masks = dict(all_full_frames=np.ones(n, dtype=bool),energy_selected_frames=active,voiced_candidates=voiced)
                for population,mask in masks.items():
                    for key,value in features.items():
                        selected = value[mask]
                        pools[population][key].append(selected)
                        per_file.append(dict(source=name,population=population,feature=key,**quantiles(np,selected)))
                for i in range(n):
                    writer.writerow([name,i,i*.02,int(active[i]),int(voiced[i]),
                                     *[f'{v[i]:.8g}' if np.isfinite(v[i]) else '' for v in features.values()]])
                counts['files'] += 1; counts['full_frames'] += n
                counts['energy_selected_frames'] += int(active.sum())
                counts['voiced_candidate_frames'] += int(voiced.sum())
            manifest.append(dict(source=name,sha256=digest,npz_sha256=hashlib.sha256(npz_path.read_bytes()).hexdigest()))
    assert counts['files']==source['successful_files'] and counts['full_frames']==source['full_frames']
    assert counts['energy_selected_frames']==source['active_full_frames']
    assert counts['voiced_candidate_frames']==source['voiced_candidate_frames']
    statistics = {population:{key:quantiles(np,np.concatenate(parts)) for key,parts in columns.items()}
                  for population,columns in pools.items()}
    for filename,rows in [('file_features.csv',per_file),('feature_statistics.csv',
            [dict(population=p,feature=k,unit=DEFINITIONS[k][0],**v) for p,cols in statistics.items() for k,v in cols.items()])]:
        with (args.output/filename).open('w',newline='',encoding='utf-8-sig') as out:
            writer=csv.DictWriter(out,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    summary=dict(**counts,duration_s=source['duration_s'],statistics=statistics,
                 feature_definitions=DEFINITIONS,source_fft_config=source['config'],source_files=manifest,
                 script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                 validation='Source hashes, WAV formats, frame counts, spectral centroid reconciliation, band sums and feature ranges passed.',
                 limitations='Positive recordings only; energy-selected frames are not annotated speech. No noise-rejection claim.')
    (args.output/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    (args.output/'speech_feature_reference.json').write_text(
        json.dumps(parameter_reference(summary),ensure_ascii=False,indent=2),encoding='utf-8')
    if args.report:
        write_report(args.output,summary)
    command=[sys.executable,str(Path(__file__).resolve()),'--input',str(args.input.resolve()),
             '--output',str(args.output.resolve())+'_rerun']
    if args.numpy_path: command += ['--numpy-path',str(args.numpy_path.resolve())]
    notebook=dict(nbformat=4,nbformat_minor=5,metadata={'kernelspec':{'name':'python3','display_name':'Python 3','language':'python'}},
        cells=[dict(cell_type='markdown',metadata={},source=['# S0002 feature extraction\n','18 features, 365 source speech recordings. Re-run writes a new sibling output directory.\n']),
               dict(cell_type='code',metadata={},execution_count=None,outputs=[],source=['import subprocess\n',
                    'command = '+repr(command)+'\n','subprocess.run(command, check=True)\n'])])
    (args.output/'feature_extraction.ipynb').write_text(json.dumps(notebook,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(counts,ensure_ascii=False))
    print('Parameters:',args.output/'speech_feature_reference.json')


if __name__ == '__main__':
    main()
