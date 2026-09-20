# 固件交流时间记录

新增噪声收尾诊断：`turns.csv` 现在还包含 `noise_tail_enabled`、`noise_tail_entries`、`noise_tail_recoveries`、`first_noise_tail_ms`、`first_noise_recovery_ms`、`recovery_ratio_per_mille`、`recovery_centroid_hz` 和 `noise_tail_end_guard_ms`。旧日志缺少这些事件时留空。含义及录音对照见 `docs/NOISE_TAIL.md`。

本功能记录固件可观察到的节点，不推算网络延迟，也不增加云端埋点。每个录音段以固件确认起音、准备发送 `capture_start` 的时刻为 **0 ms**。这是门控起音，不是人工标注或声学测量的第一声。

使用 `esp_timer_get_time()` 启动后单调计时，不读取 RTC、不需要设置设备日期、不需要设备与电脑或云端时钟同步。原始 VT1 中的 `us` 是内部计数；电脑导出的时间表已经转换为相对毫秒。

## 已记录节点

| CSV 列 | 相对起音的时间点 |
|---|---|
| capture_start_ms | 固件起音，0 ms |
| first_pcm_queued_ms | 第一帧 PCM（包含预录）准备入上行 FIFO |
| start_tx_ms | capture_start 的 WebSocket 发送调用成功返回 |
| first_pcm_tx_ms | 第一帧 PCM 的发送调用成功返回 |
| capture_end_ms | 固件判定收音结束，准备提交 capture_end |
| enqueue_end_ms | capture_end 成功放入上行 FIFO |
| last_pcm_tx_ms | 本段最后一帧 PCM 的发送调用成功返回 |
| end_tx_ms | capture_end 的发送调用成功返回 |
| verdict_ms | 收到现有协议的有效 capture_verdict，仅记到达，不增加云端逻辑 |
| spks_ms | 接受服务器的播放开始指令 |
| first_pcm_rx_ms | 首个被播放缓冲接受的下行 PCM 到达 |
| first_i2s_ms | 播放任务首次成功写入 I2S，复用已有时间戳 |
| spke_ms | 收到服务器发送结束指令，区别于实际播放完成 |
| play_done_ms | 播放任务正常完成，包括既有排空流程，复用已有时间戳 |

所有列都是从起音起算的时间点，不是各步骤耗时，也不是网络延迟。想比较两个节点时可自行相减。负差值或缺失数据留空，不补零。发送成功返回不代表服务器已经收到；首次 I2S 写入也不等于扬声器声学起声。

原始事件另外保留发送调用进入时刻、上行 PCM 帧数与最大单帧发送调用时长、capture_abort、入队失败、发送失败、会话结束、播放错误码。这些只是辅助核查，不会作为“正常交流完成”填入时间表。

## 本地采集

固件开关 `CONFIG_JULIA_VOICE_TIMING` 默认开启；需要 INFO 日志级别。新增 FFT 噪声候选仍保持关闭。本次没有烧录，因此当前设备不会因源码修改而自动产生新 VT1 日志；需后续单独烧录本次构建，才能记录这些新节点。

当前设备串口为用户确认的 COM8。在项目根目录运行（本次未打开串口）：

```powershell
python tools/voice_timing/record.py --port COM8 --baud 115200 --label keyboard_then_speech
```

按 Ctrl+C 停止并整理时间表；也可增加 `--duration 120` 自动记录两分钟。默认写入电脑项目目录：

```text
logs/voice_timing/<本次记录目录>/
  raw.log          原始串口字节，持续写入并刷新
  events.jsonl     已解析的 VT1 事件，持续写入并刷新
  turns.csv        每段起音为 0 ms 的时间表
  playbacks.csv    独立播放时间表，SPKS 为 0 ms，保留无法关联的播放
  summary.json    状态和上述时间表
  metadata.json   本次记录说明、串口参数、可选固件哈希
```

电脑目录名／metadata 的记录日期只是文件管理信息，不参与固件耗时统计。`--firmware build/julia_fused_base.bin` 可保存该文件的 SHA256，不会烧录它。`--out` 可指定一个尚不存在的输出目录；不会覆盖已有记录。脚本只依赖 Python 标准库和串口采集时的 pyserial，本机已确认 pyserial 3.5 可用。

已有日志可以重新整理，不需要连接设备：

```powershell
python tools/voice_timing/record.py --input logs/voice_timing/<记录目录>/raw.log --out build/voice-timing/reparsed
```

CSV 在正常停止／Ctrl+C 时生成。若电脑进程异常中断，可使用已持续保存的 raw.log 重新整理。没有 VT1 的旧固件日志也会原样保存，但时间表为空，不会编造数据。

## 防止串轮与误报

事件带有启动随机标识、连接代次、utterance_id 和播放 generation；这些是关联键，不是日期。固件不修改 capture-v1 或播放协议。

当前 SPKS 没有 utterance_id。因此只有同一启动和连接内恰好存在一个待回答 dialog、该段 end 已发出且在 SPKS 前收到 speech verdict，电脑才建立**推断关联**，列 `link=unique_pending_speech_inferred`。同时有多个未解决段、缺 verdict、时间点缺失或跨连接时，不强行配对。该关联仍不是服务器显式绑定；原始播放记录始终保留在 playbacks.csv，用户可结合已有云端记录核对。

断连、abort、发送失败、播放失败、未完成分别记录状态；失败不会填成正常 play_done。序号缺口、固件队列丢记录或 `VT_LOSS` 会标记 `telemetry_loss`，相关完整段时间表留空。不会因为缺少某一事件而计算成零耗时。

## 开销与线程

- 64 槽固定事件队列，单条 56 字节，约 3584 字节静态存储；短临界区复制。记录路径不分配内存、不格式化或打印字符串。
- 诊断任务优先级 1，栈 3072 字节，每次最多输出 8 条后让出；不在采音、I2S 或 WSS owner 上打印新事件。任务创建失败输出 VT_DISABLED 并停用诊断，不影响语音业务。
- 只记录阶段边界；没有逐帧日志、PCM 保存或新 FFT，分段判定不变。上行首／末 PCM 时间统计每帧只作少量计数和时间读取；JSON 检查仅在 capture 控制记录上执行。
- 日志仍占 UART 带宽及少量 CPU；实机影响、任务栈高水位未测。本次只完成主机验证和固件构建。
- 关闭 `CONFIG_JULIA_VOICE_TIMING` 后重新构建即可关闭新队列、任务及埋点；串口脚本不会改动固件、云端或设备配置。

实现位于 `main/voice/voice_timing.{c,h}`、`main/voice/capture/voice_local_capture.c` 和 `main/voice/voice_service.c`。51/51 主机测试通过，覆盖队列容量与丢失、复位、连接换代、发送失败、相对时间、无可靠关联、播放失败和本地文件输出；测试日志在 `build/voice-timing/`。ESP32-S3 固件构建成功，应用大小 `0x27cf40` 字节，分区剩余 64%；最终日志 `build/voice-timing/firmware-build-final.log`。Xtensa 对象检查确认固定队列为 3584 字节。未打开 COM8，未烧录，未验证实机时间与采集稳定性。
