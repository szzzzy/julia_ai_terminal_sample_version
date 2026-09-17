# FFT 频谱候选门控：实现与验证

基线 `b5fb3c81b0752462093154a0bae4af7b9abffab1`，2026-09-16。
默认关闭，供后续实机 A/B。365 条有效人声录音只能评估对现有音频的影响，不能验证噪声可分性。
本次未修改云端、IMU、业务状态图、OTA 或板级增益，未烧录、提交、推送；原分析成果保留。

## 实现和开关

`idf.py menuconfig` → Example Configuration → Candidate spectral flatness/entropy gate for local capture。
开启为 `CONFIG_JULIA_CAPTURE_FFT_GATE=y`；关闭为 `# CONFIG_JULIA_CAPTURE_FFT_GATE is not set`。
Kconfig、sdkconfig.defaults 与本次最终 sdkconfig 均默认/设置为关闭。已有 sdkconfig 的显式选择优先于默认值。
阈值集中在 `components/julia_board_audio/include/lc_spectrum.h`。

开启后仅在能量条件通过时计算频谱，两个条件必须同时满足：

| 候选参数 | 值 |
|---|---:|
| 谱平坦度上限 | 0.199（含边界） |
| 归一化谱熵下限 | 0.070（含边界） |
| 归一化谱熵上限 | 0.809（含边界） |

频谱通过才计入起音累计、才抵扣录音期间静音时间。能量跟踪、冻结底噪、起音窗口、静音累积、500 ms 预录、段长上限、失败处理及采音线程所有权保留。
段内不通过的帧仍先完整发送，再参与段尾计数；不按频谱结果丢帧或改写 PCM。
但起音推迟可能超过固定预录容量，导致部分起音前音频不再被覆盖，见下方回放风险。
300–4000 Hz 能量比例由特征接口返回，仅供观察，不参与门控。

## 计算口径

- 16 kHz，320 样本/20 ms，步长 320；逐帧去均值、周期 Hann 窗。
- 精确的 320 点混合基变换：按 `n=5m+r` 拆为五个 64 点复数 FFT，以 `W320^(rk)` 合并；一次逻辑 FFT 同时给出两个特征。ESP 使用现有 ESP-DSP ANSI FFT 和位反转，支持现有 PSRAM 缓冲，无 SIMD 对齐假设。主机另有可移植 radix-2 后端。
- 未采用 512 点补零，因此没有改变频点、频带或阈值统计口径；保留 50 Hz 间隔、频点 1–160（50–8000 Hz），熵除以 ln(160)。若以后改成 512 点，需要重新按同批音频标定，而不能复用本参数。
- 单边 PSD=`|FFT|²/(16000×sum(window²))`；DC/Nyquist 不加倍，其余乘 2。实数信号 DC/Nyquist 虚部置零。
- 平坦度为 PSD 几何均值/算术均值，log 前下限 1e-30 FS²/Hz；熵为归一化 PSD 的 `−Σp ln p / ln160`。
- 全零/常量帧去均值后无谱能量，返回 invalid 和有限零值，拒绝通过；负值、NaN、Inf PSD 同样拒绝。PCM16 本身不能包含 NaN/Inf。
- 帧内全部使用单精度。窗和合并系数在初始化时用双精度计算后一次舍入为 float32，逐帧不计算三角函数、不分配内存、不打印日志。
- 每采集器新增约 6.9 KiB 固定谱工作区，另有 ESP-DSP 初始化表；不缓存额外完整音段。DSP 初始化失败时，启用门控的采集路径返回失败，不能静默退化为放行。
- 无额外固定多帧确认延迟，仍用原有 20 ms 帧和累计窗口；频谱拒绝导致的实际起音推迟与计算耗时是不同问题。

## 数值与主机测试

41/41 CTest 通过。包含关闭时原回归、开启时 capture-v1 所有权/判决回填/代次隔离、PCM2 字节布局/逐样本/帧序/校验和，及两种 FFT 后端的合成测试。
另运行现有云端底噪/RMS参考差分：5,313 次操作通过，最大电平误差 6.468e-7 dB；云端参考源码未修改。
覆盖静音、常量、正弦、宽带噪声、脉冲、Nyquist、随机 PCM16、零能量/非有限边界；启用路径覆盖起音累计、滚动窗口、预录、拒绝帧尾部、持续噪声、400/750 帧上限、下一段、模式切换和回调失败。

实际 vendored ESP-DSP ANSI 内核也在主机编译执行，仅屏蔽硬件头文件。
365 个源 WAV 的 SHA-256 与原 manifest 一致；独立 NumPy float64 参考与原保存的 320 点 PSD 特征在 1e-9 内一致。
全量 87,865 帧 C/NumPy 特征最大绝对误差：平坦度 2.441e-4、熵 7.625e-7、频带比例 1.245e-6；所有门控判定一致（0 差异）。
平坦度的几何均值对接近零的频点敏感，测试允许 1e-3 绝对误差，并独立约束实际门控判定完全一致；不能推断其它录音的阈值附近也绝无数值差异。

合成“起音后 40 ms 静音 + 20 ms 高能宽带噪声”循环：旧门控到 750 帧上限结束，新门控在起音后 700 ms 静音累计时结束，共 41 帧。这只验证逻辑，不代表真实空调或键盘过滤率。

## 365 条人声回放及风险

每个 WAV 独立初始化底噪为 −60 dBFS，不加额外增益、不模拟云端判决；仅处理完整帧，末尾补 800 ms 零帧以观察自然结束。
离线 RMS≥−50 dBFS 仅用于分母选择，未写入产品起音条件。365 条均为有效录音，但没有逐帧人声真值。

两特征同时通过：全部帧 85,359/87,865（97.15%）；能量选中帧 50,381/51,557（97.72%）。
这不是语音召回率，也不是噪声拒绝率，更不能引用原三特征的 96.92% 作为本实现结果。

| 回放结果 | 唤醒旧 | 唤醒 FFT | 对话旧 | 对话 FFT |
|---|---:|---:|---:|---:|
| 无起音文件数 | 0 | 0 | 0 | 0 |
| 总段数 | 444 | 445 | 388 | 388 |
| 上限结束次数 | 12 | 12 | 0 | 0 |
| EOF 前结束次数 | 89 | 90 | 24 | 24 |
| 上传帧数（含补入尾部静音） | 95109 | 95026 | 95660 | 95428 |

唤醒 81 条、对话 107 条文件的分段记录有变化。唤醒首触发有 26 条推迟，最大 160 ms；对话有 54 条推迟，最大 700 ms。
最后结束位置：唤醒 41 条提前，最多 100 ms；对话 54 条提前，最多 120 ms。
EOF 前结束可能是正常停顿，不能自动记为语音截断。

**预录不能消除全部推迟风险。** 对话有 47 条的首上传帧晚于旧门控，最大少覆盖开头 420 ms：
`BAC009S0002W0495.wav` 触发帧 10→45（+700 ms），首上传帧 0→21（+420 ms）；
`BAC009S0002W0441.wav` 触发帧 14→43（+580 ms），首上传帧 0→19（+380 ms）。
需要听辨/标注这些区间才能判断是否丢了有效语音。未擅自扩大预录或改变起音窗口来掩盖风险。

唤醒额外切分：`BAC009S0002W0136.wav` 2→3 段，`BAC009S0002W0420.wav` 1→2 段；
`BAC009S0002W0440.wav` 3→2 段。总数只增加 1 段，不能掩盖三个文件分别发生的变化。
因此候选门控默认关闭，尚不宜凭单类数据默认启用。

## 构建、证据和复现

ESP-IDF 5.5.4 / ESP32-S3 正常产品固件开关两种构建均成功；两次均未开启 `CONFIG_JULIA_IMU_LOGGER_ENABLE`。
启用二进制 0x269d50 字节，关闭 0x269d40 字节，7 MiB 应用分区约 66% 空余。
最终 `build/julia_fused_base.bin` 为关闭版本；启用对照在 `build/fft-gate-dsp-validation/firmware-enabled.bin`，不自动烧录。

结果在 `analysis/fft_gate_validation_20260916/`：`validation.json`、逐文件 `replay_segments.json`、主机日志、两次构建日志。
常规验证入口（Windows 使用已配置 ESP-IDF/TinyCC 工具）：

```powershell
cmake -S tests/host -B build-host-state-recovery -DJULIA_NUMPY_PATH=D:/Espressif/projects/julia-fused-base/build/local-capture-deps
cmake --build build-host-state-recovery
ctest --test-dir build-host-state-recovery --output-on-failure
python tests/host/test_fft_gate.py --cc build-ota-name/host-tools/tcc/tcc.exe --out build/fft-gate-dsp-validation --numpy-path build/local-capture-deps --esp-dsp --corpus "D:/Espressif/projects/VOICE DATA BENCHMARK/analysis/S0002_fft"
idf.py build
```

主机数值测试需要 NumPy；省略 `--esp-dsp` 可验证可移植后端。TinyCC 的头文件依赖追踪不完整，改变结构体头文件后应 `cmake --build ... --clean-first`。
历史 `capture_tail_probe.c` 明确固定为关闭 FFT 的能量门控探针；新旧 FFT 对照使用 `test_fft_gate.py`，不改云端参考实现。

修改文件：板级音频的 `local_capture.c`、`include/local_capture.h`、`CMakeLists.txt`；新增 `lc_spectrum.c`、`include/lc_spectrum.h`；
`main/Kconfig.projbuild`、`sdkconfig.defaults`；`tests/host/CMakeLists.txt`、`test_voice_local_capture.c`、`capture_tail_probe.c`；
新增主机 `fft_gate_driver.c`、`fft_math.def`、`test_fft_gate.py`；本文及 `LOCAL_CAPTURE.md`，另附本轮验证数据。

## 尚未实测

没有进行 ESP32-S3 每帧耗时 P50/P95/最大值、队列积压/丢帧、联网/显示/播放并行运行测试。
不以主机运行速度声明目标板性能通过；ANSI 内核是否足够快需实机测量后决定。
本轮没有真实键盘/敲桌/空调负样本、逐帧标注、跨说话人/距离/音量对照，未声明任何真实噪声过滤效果。
