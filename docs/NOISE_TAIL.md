# Dialog 噪声收尾：实现与录音验证

本次实现已完成，当前本地实验配置开启；未烧录、未修改服务器。不是仅实现最初的“4/5 帧恢复”：在旧键盘录音上继续比较了 22 组恢复门限／尾部保护组合，采用了有实际收尾改善、并通过源 PCM 覆盖对照的一组。

## 最终逻辑

只作用于 `LC_DIALOG` 的起音后阶段。wake 和起音前算法不变，统一音量门限、VAD 库、FFT、PCM 上传格式都不变。

1. 保留当前噪声证据：高频比 >20% 且质心 >800 Hz；最近 15 帧至少 5 个能量帧、其中疑似噪声 ≥60%，确认 1 帧。
2. 起音后当前噪声帧满足占优条件，进入持续的噪声收尾状态；不确定帧和零散漏过帧不能再扣回 40 ms 静音。窗口后来变安静也不会自动解除该状态。
3. 恢复候选必须同时满足：原能量／VAD／FFT 条件、**高频比 ≤20% 且质心 ≤700 Hz**。原先“不是联合噪声特征”只需其中一项不超过门限，两者不等价。
4. 最近 5 帧里至少 4 帧满足恢复候选，才解除收尾、清零静音，并清掉旧噪声窗口，避免下一帧被残留历史重新锁住。
5. 每次噪声收尾过程仅有一次 5 帧／100 ms 的恢复观察机会。首次恢复候选出现后开始计时，后续每帧都消耗额度；孤立敲击不能续期。恢复观察期间继续发送 PCM。
6. 原 dialog 静音累计 700 ms，噪声收尾状态额外保留 **500 ms 尾部 PCM**；最多再加一次 100 ms 暂缓。因此没有恢复且未先到最大段长时，从进入收尾到结束不超过约 1300 ms（进入前已有的静音进度保留）。普通收音结束仍是原 700 ms，不增加这 500 ms。
7. 最大上传 750 帧始终优先；正常发 `capture_end`，无新增自动 abort、无丢弃前面音频、无长时间禁止收音。
8. 正常 END 清掉本段收尾状态，保留原有短噪声历史供起音拦截；模式切换、连接换代、VAD 重置清理收尾状态。发送失败仍沿用原失败处理。

## 为什么没有停在第一版

第一版只增加“4/5 帧恢复”，在持续阳性 VAD 的压力回放里，对旧键盘录音进入收尾 13 次又恢复 13 次，结束时间没有改善。

改成联合恢复条件后，能切断键盘，但发现少数语音输入的原始帧覆盖比旧规则少。继续扫描 100–600 ms 尾部保护，选定 500 ms，并冻结参数后用另外两个说话人验证。

选择依据不是仅看段数，也不是把高能量当成语音标签：检查每段实际上传的输入帧区间，计算“旧规则上传过、新规则不再上传”的原文件帧。保留 PCM 原字节及顺序的 C 回调断言贯穿回放。

## 旧键盘录音结果

输入：`VOICE DATA BENCHMARK/analysis/keyboard_sixianlou/converted_16k_mono_pcm16.wav`，594 个完整帧；SHA256 在结果 JSON 中。

比较双方的起音／频谱参数完全相同，仅切换 `JULIA_CAPTURE_NOISE_TAIL`。压力条件为 VAD 每帧阳性，输入直接送入生产 C 能量、FFT、窗口和分段代码，不模拟播放器到设备麦克风的声学路径。

| 项目 | 旧逐帧收尾 | 最终新收尾 |
|---|---:|---:|
| 第一次触发帧 | 5 | 5 |
| 首段从触发到结束 | 12.44 秒 | **2.12 秒** |
| 最长单段从触发到结束 | 12.44 秒 | **2.94 秒** |
| 录音首帧到首次结束 | 12.56 秒 | **2.24 秒** |
| 总段数 | 1 | 6 |

六段从各自触发到结束分别为 2.12、2.28、1.56、1.92、2.94、0.96 秒，均正常 end。它解决的是这份回放中的持续拖长，**没有消除再次触发**。回放一直保持 dialog 并继续喂输入；真实 FSM 的 THINKING／SPEAKING 门控会影响之后是否再起音，不能直接把 6 段等同于现场 6 次界面切换。

“两帧疑似噪声 + 一帧旧规则候选”的确定性反例也通过：旧规则会等硬上限，新规则能够正常收尾。这个反例是状态机制验证，不是键盘识别准确率。

## 小声与独立说话人检查

开发阶段使用 S0002 的 365 个完整文件，以及每个文件的 15% 幅度版本。最终参数下，原幅度有 3 个文件边界变化，低幅度有 2 个；无新增分段，无额外原文件帧缺失。

冻结参数之后再读取此前未用于调参的两个说话人档案：

| 数据 | 原文件数 | 原幅度＋15%幅度对照数 | 边界变化 | 新增分段 | 额外原文件帧缺失 |
|---|---:|---:|---:|---:|---:|
| S0003 | 354 | 708 | 9 | 0 | 0 |
| S0004 | 357 | 714 | 0 | 0 | 0 |

总计 1076 个原始文件、2152 个幅度条件。上述“0 缺失”是**相对于旧规则的 PCM 覆盖**，不是声学准确率，也不表示旧规则本身没有漏音。数据仍属于同一录音语料条件；15% 幅度是衰减模拟，不等于真实低声说话。没有人工逐帧标签，没有声称识别率或实机泛化已通过。

同时回放了录音衰减到 70%、插入静音的间歇键盘、孤立敲击代理、短句、300 ms 停顿、讲话加键盘、键盘后讲话、讲话后键盘。详细结果保存在 `analysis/noise_tail_validation_20260918/results.json` 和 `scenario_summary.csv`；变换录音不算独立实采样本。

## 配置与回退

当前根目录 sdkconfig 的实验配置：

```ini
CONFIG_JULIA_CAPTURE_NOISE_WINDOW=y
CONFIG_JULIA_CAPTURE_NOISE_RATIO_PERMILLE=200
CONFIG_JULIA_CAPTURE_NOISE_CENTROID_HZ=800
CONFIG_JULIA_CAPTURE_NOISE_WINDOW_FRAMES=15
CONFIG_JULIA_CAPTURE_NOISE_MIN_ENERGY_FRAMES=5
CONFIG_JULIA_CAPTURE_NOISE_PERCENT=60
CONFIG_JULIA_CAPTURE_NOISE_CONFIRM_FRAMES=1
CONFIG_JULIA_CAPTURE_NOISE_TAIL=y
CONFIG_JULIA_CAPTURE_NOISE_RECOVERY_RATIO_PERMILLE=200
CONFIG_JULIA_CAPTURE_NOISE_RECOVERY_CENTROID_HZ=700
CONFIG_JULIA_CAPTURE_NOISE_END_GUARD_MS=500
```

`NOISE_TAIL` 的 Kconfig 默认 n，本地本次实验明确设为 y。只关闭 `NOISE_TAIL` 即回到此前逐帧收尾，仍保留当前放宽的起音拦截。关闭 `NOISE_WINDOW` 则关闭整个新增噪声窗口。

原 `sdkconfig.keyboard-trial.defaults` 是历史激进起音实验，不代表当前根 sdkconfig 的最终配置；本次没有通过那个脚本覆盖根配置。

## 日志与验证

VT1 增加 `tail_config`、`recovery_shape`、`tail_guard`、`tail_enter`、`tail_recover`、`tail_summary`。电脑 `turns.csv` 可查看是否开启、恢复门限、尾部保护、进入／恢复次数和首次进入／恢复的相对时间。只有状态边界日志，不逐帧打印；沿用独立诊断队列和任务。

52/52 主机 CTest 通过，包含：4/5 边界、一次性观察窗口不能续期、临近收尾的小声恢复、联合恢复条件、不同尾部保护、旧最大段长优先、起音行为不变、wake 行为不变、发送失败、模式与连接代次复位、PCM 字节与序号、日志解析。没有新增 FFT、PCM 缓冲或模型，主要新增固定大小的状态和比较。

ESP32-S3 最终构建成功，`build/julia_fused_base.bin` 大小 `0x27ec40`，分区剩余 64%；最终构建日志无 warning/error。编译器结构探针确认 `local_capture_t` 从 187336 增至 187376 字节，增加 **40 字节**；特征结构仍为 24 字节，FFT 工作区仍为 7052 字节。实机周期耗时未测。构建包含工作区已有的其他修改，本次未回退它们。

复现：

```powershell
python analysis/noise_tail_validation_20260918/replay.py
python analysis/noise_tail_validation_20260918/scan_recovery.py
python analysis/noise_tail_validation_20260918/scan_recovery.py --guard-scan
python analysis/noise_tail_validation_20260918/scan_recovery.py --long-guard-scan
python analysis/noise_tail_validation_20260918/validate_speakers.py
```

录音脚本使用已有 NumPy 与 TinyCC 路径，不访问线上模型。源码保存前的快照在 `build/noise-tail/before/`；构建及测试日志在 `build/noise-tail/`。

实机下一轮优先观察：相同键盘录音、直接敲击、低声短句、收尾前重新开口、讲话结束后持续敲键。查看 `tail_enter` 是否出现、是否频繁 `tail_recover`、`capture_end` 是否实际发出。此次未刷机，真实 ESP-SR VAD、麦克风声学条件和任务耗时仍需现场验证。
