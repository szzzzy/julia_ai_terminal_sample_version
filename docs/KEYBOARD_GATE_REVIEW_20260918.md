# 键盘噪声 listen 收尾实验，2026-09-18

**交付结论：完成代码核查、候选实现、离线复算、主机回归与固件构建；没有获得可以安全默认启用的门限。新增门控默认关闭。没有烧录、连接或修改线上服务器，也没有替换 ESP-SR 库。**

用户后续强调“主要是切断”。因此本次优先检查已经起音后是否正常结束，以及结束后是否再被同一串噪声触发。这里的切断是发送 `capture_end`，保留全部已发送音频，绝非删除前面的讲话或只改 UI。

## 已确认的机制与边界

- 16 kHz 单声道 PCM16、320 样本／20 ms；逐帧去均值、周期 Hann、真实 320 点变换，无重叠。FFT 实现是 5 个 64 点子变换组合，未改动算法。
- 原实现已经同时在起音和段内使用平坦度／熵，不是“起音后没有 FFT”。但没有近期噪声历史；通过能量、VAD、FFT 的一帧在 dialog 抵消 40 ms 静音，在 wake 抵消 80 ms。每帧未通过仅累计 20 ms，因而间歇阳性足以延长 listen。
- 实际 VAD 适配器每帧调用 ESP-SR 1.9.4 中的 WebRTC C 接口，包含内部延续。不能把阳性当作独立的新语音证据。
- 初始底噪 -60 dBFS，段内冻结；起音门限 wake 为底噪 +3 dB、dialog 为 +9 dB，续听为 +3 dB。本次没有提高这些统一能量门限。原有 wake 1 帧／dialog 15 帧窗口内 6 帧起音、25 帧预录、500／700 ms 尾长、400／750 帧上限保持。
- 工作区原有 FSM、离线恢复、S4 无起音退出等修改保留。它们不是本次 FFT 修复的新增成果。
- 仍未确认：当前设备实际输入、背景底噪、VAD 原始逐帧轨迹、声学回声、任务调度和网络事件下的 listen 时序。不能仅根据文件 PSD 断言现场根因唯一。

## 实验实现

`CONFIG_JULIA_CAPTURE_NOISE_WINDOW` 依赖原 FFT 与 VAD，Kconfig 默认 `n`，当前 sdkconfig 也是关闭。示例结构配置为 `(R=.6, C=1800 Hz, W=15帧, 最少5能量帧, 噪声比例80%, 持续5帧)`，只是可复现实验的固定参考，不是经过选择的产品配置。

特征口径如下：

| 特征 | 定义 |
|---|---|
| 新增 R | 单边 PSD 的 bins 20..160 之和 / bins 0..160 之和，即闭区间 1000..8000 Hz |
| 新增质心 C | `sum(PSD[k] * k * 50 Hz) / sum(PSD[k])`，k=0..160 |
| 原平坦度／熵 | bins 1..160，不含 DC；平坦度日志下限 1e-30，熵按 log(160) 归一化 |
| 原诊断能量比 | 300≤f<4000 Hz，bins 6..79，保留 |

去均值不代表加窗后 DC 必为零，所以 R 和质心明确包含 DC 分母。DC、Nyquist 在已有单边 PSD 中权重 1，其余权重 2；50 Hz 的公共频宽在比值中抵消。负数、NaN、Inf、全零或只有 DC 的退化 PSD 返回无效与有限零值，不视为语音。新增结果也检查有限性。95% 累计能量频率仍留在原离线诊断中，没有额外收益证据，不加入固件门控。

每帧最多执行一次 FFT，在现有 PSD 累计循环增加高频和与频率加权和。开关关闭时保留原 FFT 调用条件；开启时还观察有能量但 VAD 阴性的帧，避免由 VAD 延续决定噪声统计分母。

窗口分为低能量／无有效证据、有能量但不确定、疑似噪声。只有 `energy && VAD && 原FFT通过 && !疑似噪声` 才是恢复原有机会的**候选**，仍不叫“确认人声”。高频、质心同时严格大于阈值才标疑似噪声。窗口噪声比例的分母是有能量帧，不是全窗口帧；低能量帧仍占时间并挤出旧证据。参数非法时门控不拦截并复位自身。

- 起音前：在原预录／起音窗口之外观察近期证据；最少帧数和占比满足时，当前疑似噪声不能贡献起音或依靠旧起音计数启动。未满足证据量时保留原机会。500 ms 预录照常保存。wake 第一帧不能凭未来证据拦下，因此允许先起音再正常收尾。
- 起音后：占优条件连续满足配置帧数后，当前疑似噪声不再抵消静音，改为累计 20 ms。候选出现立即取消确认累计、恢复原规则；这很保守，也会被低频键盘尾声恢复，是实验失败的一个机制。
- 正常收尾继续输出本帧 PCM，然后发 `LC_END`。新增逻辑没有任何 `LC_ABORT` 调用，也不记录“整段确认无人声”。原有预录、帧序号、音频字节保持。
- 正常段结束保留最多 W 帧的短历史，以抑制同串噪声立即再起音；不增加禁收音计时器。模式切换、OFF、连接代次经现有 owner 路径复位；VAD 重置同时清理窗口。
- 新窗口只记住足够的能量和噪声计数，以及 VAD 与原 FFT 共同形成的候选恢复事件，不把持续 VAD 阳性当作确认人声。

## 那段键盘声是否能切断

源文件是 `VOICE DATA BENCHMARK/analysis/keyboard_sixianlou/converted_16k_mono_pcm16.wav`，完整帧 594（11.88 秒），SHA-256 见结果 JSON。文件尾不足一帧排除，回放最后补 1000 ms 零帧。输入没有额外施加麦克风增益；当前固件数字增益为 70%，录音来源与设备输入幅度并不等价。

下表以该文件原幅度、初始底噪 -60 dBFS、VAD 每帧阳性作为延续压力测试。时间是从文件首帧到 end 帧末，**不是人工标注的最后发声到收尾延迟**。

| dialog 配置 | 首次正常 end | 总段数 | 结论 |
|---|---:|---:|---|
| 原实现／新增开关关闭 | 12.56 s | 1 | 等文件结束后才收尾 |
| 固定参考 .6 / 1800 / 15 / 5 / 80 / 5 | 12.56 s | 1 | 没有改善 |
| 激进探针 .1 / 600 / 15 / 5 / 60 / 1 | 2.38 s | 4 | 可以切断，但同一串噪声反复触发 |
| 激进探针 .1 / 600 / 15 / 5 / 40 / 1 | 2.20 s | 5 | 更早切断，重触发更多 |

先扫描 64 组（R/C 四组、200/300 ms 窗、3/5 能量帧、70/80%、60/100 ms 确认），这段键盘录音的 dialog 结果均未改善。按“切断优先”又扫描 72 组更激进条件，其中 4 组在文件结束前发出正常 end，但均重触发 4–5 段。因此没有把任何一组作为安全配置默认启用。

固定参考在 365 段 S0002 中，wake 的分段轨迹没有改变；dialog 有 12 个文件时序变化，总段数仍是 388。高能量帧不是人工语音标签，不能把变化认定为改善或误截断。激进探针 .1/600/15/5/60/1 改变 wake 的 2 个文件、dialog 的 98 个文件时序；总段数仍为 445/388。详细变化另存于 `results.json` 的 `aggressive_cut_probe` 和 `recordings.json`，不能视为准确率。

验证矩阵还包括原键盘连续播放、插入静音的间歇键盘、最大能量单帧敲击代理、0.15 倍幅度讲话、正常讲话、插入 300 ms 停顿讲话、讲话加键盘、键盘后接讲话、人声后接键盘。全部分段触发／开始／结束帧、段数、上限原因可检查 `results.json/scenarios`。合成拼接、衰减、混音和单帧敲击代理都不是独立实采场景；没有提供识别率或声学泛化结论。

参数探索仅用第一批 8 个完整 S0002 文件作开发观察，其他 357 个完整文件以固定配置复算；全部仍来自同一说话人条件。没有随机拆邻帧，也没有独立键盘验证集。此次没有选出通过安全验证的阈值。

## 协议核查

核查了固件 owner、仓库 `cloud/capture-v1/local_capture.py`、历史 `docs/patches/cloud-capture-v1.patch` 以及本地 `build/cloud-live-audit-20260915/server/` 源码快照。历史快照不等同于当前线上运行版本，本次没有上线探测。

1. 前有讲话、尾部噪声：一律按原 `capture_end(reason=silence/limit)` 完整结束，所有已经采集进入该段的 PCM 留给服务器识别。
2. 始终只有疑似噪声：此次同样走 end，不启用自动 abort；是否取消整段留待第二阶段。
3. 起音前拦截，没有 start：不发 end/abort。

本地接收器要求 end/abort 匹配 active 段的 `utterance_id`、mode 和 frames。abort 将段标记 aborted，active 置空并唤醒等待者；pending 中的对象并非在 end() 中立即删除，其消费者取出后检查 aborted。capture() 内会抛取消异常，close() 也使消费者退出。

但接收器在锁外调用 `on_chunk`，30 帧一块的音频可能已经进入 StreamingAsr。历史 real_engine 在 capture TimeoutError 时继续循环，没有足够证据证明所有在途远程推理都会取消；也未验证 abort 与最后一次检查／回调的竞态。历史 WSS 对新 generation 关闭旧 receiver 并 retire session，只有正常 end 调用 `note_capture_end()`。这些机制不足以直接推断 abort 的最终 UI 同步和迟到识别安全性。

第二阶段尚未验证：已取出／仍排队对象的即时释放、远程识别任务取消确认、迟到 partial/final 丢弃、utterance_id 与连接 generation 同时过滤、abort 后服务器/FSM 收音状态一致、断连及重连时竞争顺序。

固件此次没有修改 UI。正常 end 仍先进入上行 FIFO，成功且 epoch/ready 仍匹配才通知 state_event。FIFO 失败会使 capture failed，再由 owner 结束会话；不能把“入队成功”说成“云端已收到”。主机已有 FIFO、断连换代和状态测试继续运行。

## 测试、构建与开销

- 基线：原 46 项主机测试中，44 项首轮通过；2 项 FFT 因主机 NumPy 未配置失败，接入已有依赖后通过。
- 修改后：48/48 主机测试通过；包括新增窗口测试、启用实验开关的 owner 测试、原能量/VAD/FFT、FIFO、断连、会话恢复、状态及其他已有回归。
- 新测试验证最少能量帧数、80% 等号边界、窗口过期、确认期间静音、候选恢复、模式／代次复位、无 VAD 阳性不得仅凭 !noise 起音、START/AUDIO/END 发送失败、收尾与最大段长、预录／逐字节 PCM／序号完整性、抑制重复触发的确定性信号场景。
- 数值验证：高频比与 NumPy 双精度参考的最大绝对误差约 4.2e-7，质心约 0.00271 Hz；验证 DC、Nyquist、1000 Hz 边界、无效数与全零。原 portable/实际 vendored ESP-DSP ANSI FFT 主机测试通过。
- 扩展结构后第一次增量 TinyCC 构建未重编译两个测试调用方，出现结构大小不一致的断言；`--clean-first` 全量重编译后 48/48 通过。后续回归也通过。复现时建议 clean build。
- 本地服务器接收器 13/13 单元测试通过，只验证本地源码，不能替代线上取消语义测试。
- 当前配置固件构建成功，`build/julia_fused_base.bin` 大小 `0x27c600`，应用分区剩余约 64%。新增开关关闭。日志 `build/keyboard-gate/firmware-build-final.log`。首次构建沿用了既有初始化警告，并提示 ROM ELF 环境变量未设置；最终构建设置已安装的 ROM 路径后完成。
- Xtensa 编译器结构大小探针：`local_capture_t` 187256 → 187336 字节，增加 80 字节；特征结构 16 → 24 字节；FFT 工作区仍为 7052 字节。没有新增 FFT 缓冲、动态分配或模型。没有测设备任务栈高水位。
- 主机 TinyCC 计时见 results JSON（三轮特征计算、每轮 20000 帧），仅用于比较，不是 S3 的运行时间。旧特征计算为 14.9/15.6/14.6 us，新计算为 15.5/17.25/15.05 us，单轮有调度波动；开启后 VAD 阴性但有能量的帧也要做 FFT，全链路由 2.15 增至 16.95 us/帧；VAD 阳性样例由 16.85 变为 23.3 us/帧（包含分段路径变化）。实机 PSRAM、CPU 占用、最坏帧耗时和调度抖动未测，不能用主机数字替代。

## 复现与后续实机验证

在项目根目录执行（不涉及烧录）：

```powershell
& D:/Espressif/tools/cmake/3.30.2/bin/cmake.exe -S tests/host -B build-host-local-capture '-DJULIA_NUMPY_PATH=D:/Espressif/projects/VOICE DATA BENCHMARK/tools/_fft_deps'
& D:/Espressif/tools/cmake/3.30.2/bin/cmake.exe --build build-host-local-capture --clean-first
& D:/Espressif/tools/cmake/3.30.2/bin/ctest.exe --test-dir build-host-local-capture --output-on-failure
python tests/host/test_keyboard_gate.py --cc build-ota-name/host-tools/tcc/tcc.exe --out build/keyboard-gate/reproduce --numpy-path 'D:/Espressif/projects/VOICE DATA BENCHMARK/tools/_fft_deps' --benchmark 'D:/Espressif/projects/VOICE DATA BENCHMARK'
```

上述回放比较当前代码开关关／开。此次额外使用修改前保存的四份源码快照 `build/keyboard-gate/baseline/`，命令追加 `--baseline build/keyboard-gate/baseline`，对全部 365 文件、两种模式及合成矩阵核对：新开关关闭的分段轨迹与原源码一致。快照属于本地构建证据，未将旧源码重复并入正式组件。源音频逐文件 SHA-256 与已有 manifest 一致。

后续需单独授权实机操作后，先采多键盘、多距离和不同说话人、正常/小声/短句/断续/混合场景，记录麦克风增益后的原始 PCM、逐帧 VAD、R/C、冻结底噪、窗口计数、静音累计、utterance_id、epoch 和 start/end 时间。人工标出讲话与敲击区间，以整段录音／说话人／条件划分开发与验证集。重点比较最后真人发声到 end 的延迟、讲话后持续敲键的收尾、重触发次数、语音被截掉的样本和完整段字节；同时核对真实服务器接收与 FSM 状态。测 S3 20 ms 帧预算、PSRAM 和栈高水位。没有上述证据不要开启默认门限。

回退方式：保持或恢复 `CONFIG_JULIA_CAPTURE_NOISE_WINDOW=n` 即关闭新增判决，原 FFT、VAD 与分段规则保留。此次构建已经是该状态。实验参数在 `lc_init()` 的 `noise_config`，只允许 capture owner 在 `LC_OFF` 配置；激进探针只在离线测试脚本里，未写入固件默认值。

本次修改文件：`components/julia_board_audio/{local_capture.c,lc_spectrum.c,include/local_capture.h,include/lc_spectrum.h}`、`main/Kconfig.projbuild`、`tests/host/{CMakeLists.txt,test_voice_local_capture.c,keyboard_gate_driver.c,test_keyboard_gate.py}`，以及本报告和 `analysis/keyboard_gate_validation_20260918/` 的分析产物。没有改动既有 review.py/results.json。
