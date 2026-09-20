# S6 WSS 恢复与播放积压：实现、验证和现场诊断

日期：2026-09-18。基于本次开始时已有的未提交工作区增量修改，未刷机、未复位设备、未修改服务器。
本次没有找到适用的 AGENTS.md；修改前相关源文件副本保存在被忽略的 `build-wss-recovery/before/`。
既有 MQTT 重启重试、采音、键盘门控、voice timing 等修改均保留。

## 结论与证据边界

**已修复并通过主机验证的是代码中的恢复缺口；尚未证实现场 S6 断线或回复无声的根因。**

现场输入：已刷入 MQTT 重启失败重试修复；服务器先见 WSS 1006/ping timeout，此后未见握手；
设备处于 S6，语音和摇晃无法唤醒；当前 `CONFIG_JULIA_LOCAL_CAPTURE_ENABLE=y`。
服务器未见握手不能证明设备没有发起连接，仍须区分 owner/FSM 停滞与持续网络不可达。

新增现场输入：播放服务器回复时偶发无声，服务器日志报告播放缓存溢出，暂无对应固件日志。
云端提供 24 kHz、PCM16、单声道和首块至溢出约 4.234 s；128 KiB 相当于约 2.73 s 音频，
前六帧预填的约 3.7 KiB 提前量不能单独解释这次积压。**没有增大播放缓冲，也没有将容量小认定为根因。**

相关原始分析保留在：

- [S6 WSS 复查记录](S6_WSS_OFFLINE_FOLLOWUP_20260918.md)
- [待机离线诊断](STANDBY_OFFLINE_DIAGNOSIS_20260918.md)

## 已确认的代码行为与锁依赖

- WSS owner 串行承担建连、HTTP 升级、收发、保活、上层回调、TLS 销毁和重连。
- 原断线回调清理会话后同步投递 `EVT_WSS_DISCONNECTED`；同步请求的队列发送及确认等待均无限，
  且队列消息持有栈上的 semaphore/applied 指针。不能只把等待改成超时。
- 新会话 RESET、云同步 ACK 后 CONNECTED、唤醒确认也经过同步请求。
- FSM 的 enter/exit 会访问播放状态及 UI。播放状态 mutex 使用无限等待，但 I2S 写入在此 mutex 外；
  板级 speaker mutex 仍有无限等待，保护通道 start/retain/write/stop。
  因而“进 S2.3”不能作为播放器已经启动或持续输出的证据。
- 检查到的语音同步投递点均在释放 `s_mic_state_lock` 后调用；未发现能够凭源码确认的具体闭环死锁。
  LVGL 调用存在有界等锁；播放、板级驱动、文件清理和 TLS 仍可能成为慢路径。
- 新请求锁仅保护小块内存和引用计数；新 WSS 健康锁仅保护快照。持有这些锁时不调用网络、UI、NVS
  或阻塞 RTOS 操作。两把锁不嵌套获取。
- 当前 S6 保留网络与采音。唤醒仍要求原有 MQTT/WSS/云同步准入条件，不因恢复修改而放宽。

## 断线通知与请求生命周期

### 可重试的断线通知

传输层先使 `s_session_ready=false`，由 owner 销毁 TLS，再清理 capture、云同步、MIC ring、文件和
当前播放代次。最后把连接 generation 写入独立通知槽，立即返回，不等待 FSM 队列或完成确认。

FSM 每轮先消费该槽，再核对链路快照；激活后无事件时最多约 1 s 再巡检。通知槽不受 16 槽事件队列
是否已满影响。只有匹配当前 WSS generation 的断线才生效；新会话必须同步 RESET 成功后才启用其
语音资源，再等待云同步 ACK。旧 generation 的通知可被新会话 RESET 取代，不能覆盖新连接 ONLINE。
这是一份持久的待处理状态，不是不断向满队列追加断线副本。

### 有界同步请求

- 固定 18 个确认槽，不再把栈地址或栈上信号量放进消息。
- 队列发送最多 100 ms；普通事件总预算 3 s，启动控制 5 s，OTA 收尾 15 s。
  完成检查每 10 ms 一次；返回时间仍受调度延迟影响，并非硬实时承诺。
- 每个槽有调用方和队列/owner 两份引用。调用方超时标记取消并释放自己的引用；FSM 晚到消费或完成
  只释放自己的引用。最后一个引用释放后槽才可复用，不存在超时删除信号量或写调用栈的问题。
- 到期、取消、旧连接 generation 或过期 revision 的请求不得再开始事件处理。
  FSM 在退出回调之后、写入新状态之前再次检查取消/期限，避免退出回调停滞后迟到唤醒。
  状态期限仅在新状态提交后清理，取消退出不会丢失旧状态的计时期限。
- **提交与完成不同**：在截止时间前已提交的迁移不回滚、不重放。完成确认迟到时返回超时，当前 WSS
  会话失败并清理；延迟 observer 返回后不会再启动过期呈现。已进入的底层外设调用不能强行撤回。
  下一会话仍须等待自己的 RESET 提交成功，不能越过未完成的旧 FSM 操作。
- FSM 永久停滞时，队列/处理中引用会占用有限静态槽；耗尽后快速失败，不持续分配内存。独立监测负责
  升级恢复。只要 FSM 恢复消费，取消槽全部回收。
- WSS 的队列失败/超时使当前会话退出，不把失败的唤醒 ACK 当成成功，不重放请求。

## 独立诊断与分级恢复

启动独立 `wss_health` task（4 KiB 栈、优先级 3），每 5 s 检查一次。它在 WSS owner 创建前启动；
创建失败则不启动无监测的 owner，沿既有初始化错误重试。owner 创建失败时保留并复用空闲监测任务。

健康快照记录阶段、阶段开始时间、最近进展时间、该阶段预算、generation、尝试数和连接失败数。
正常收发空闲轮转也更新进展，**不以最近收到服务器数据的时间判定任务停滞**。

| 阶段 | 无进展阈值 |
| --- | --- |
| connect（含 DNS/TCP/TLS 与连接前电源操作） | 60 s |
| HTTP handshake | 30 s |
| RX、上层 callback、cleanup | 30 s |
| TX、PING/PONG 写入 | 15 s |
| FSM 等待 | 20 s，正常同步请求应更早按自身预算返回 |
| backoff | 本轮实际退避秒数 + 30 s；包括认证/协议要求的长退避 |
| stopped（明确暂停/旧配置的安静停网） | 不检查 WSS 停滞 |
| FSM 自身 owner 循环 | 30 s；读取快照不等待 FSM |

独立监测的动作：

1. 发现停滞，输出状态诊断，调用既有 `wss_transport_request_session_end()` 请求 owner 正常清理。
2. 停滞持续额外 120 s，直接复用 `julia_fault` 保存 `CORE_TASK_STALLED`，不投递给可能卡住的 FSM。
3. 保存并回读成功、同类计数不超限时，按既有复位延迟（当前 3 s）等待，再复查进展；仍停滞才
   `esp_restart()`。等待期间已恢复则取消复位。
4. 当前上限为 3 次。同版本连续的 CORE_TASK_STALLED 不因长 uptime 自动清零，否则检测期长于
   quick-boot 窗口会造成每次都算首次故障。计数超限、读取失败或写入失败均抑制此路径的自动复位。
   每次开机最多尝试记录/升级一次；新固件版本或其它故障记录会按既有记录模型改变故障链。

没有跨任务删除 WSS task，没有跨任务访问或销毁 TLS，没有普通离线定时重启。持续连接失败只诊断，
有进展的网络失败、正常空闲、正常退避均不升级复位。异常快照及连接失败汇总最多每 60 s 一次；
首次停滞请求和一次恢复升级另有明确日志。新日志不包含凭据或音频内容。

## 回复无声、缓冲溢出的检查与修改

源码核对：

- `SPKS 24000` 传入 `voice_playback_start(24000)`，播放 worker 把该 rate 交给
  `board_audio_speaker_start()`，后者用 `I2S_STD_CLK_DEFAULT_CONFIG(rate)` 创建或重新配置通道。
  尚无实际 WS/BCLK 测量，因此不能把请求/配置成功当作物理采样率测量。
- 每次网络开播都复位完整 128 KiB PCM FIFO；单块最大 1200 B。完成/取消按 generation 隔离。
  本地提示音直接读取静态 PCM，不占此 FIFO。正常路径没有发现一个更小的替代播放 FIFO。
- 原有 `ERROR playback_overflow` 来自播放层 `ESP_ERR_NO_MEM`；溢出会中止当前播放，但上层 poll
  原先会先调用正常 speaker_done，再报告错误。这可能把失败回复当作正常说完，已修正。

防护与诊断：

- WSS 每次 recv 前保留一块最大消息的播放空间。空间不足时让出 10 ms，继续服务上行和 poll，借助
  TCP 背压限制集中交付，不扩大缓冲、不丢中间 PCM、不在 binary 回调里等待播放器。
- 若连续 1 s 仍腾不出一块空间，当前会话失败并按原路径清理/重连。播放器自身卡住时，不允许无限
  等待空间。控制帧也可能被这段接收背压延迟，需实机验证；1 s 是其有界故障退出门限。
- 本地提示、已结束或未启动的网络播放不会触发背压。实际溢出仍有兜底：报告错误、退出会话，
  不走成功播完的业务迁移。
- 每个播放 generation 记录收到、成功入队、出队及成功提交 I2S 的单声道源字节数；记录首/末接收时间、
  I2S 启动请求/成功时间、首/末输出时间和配置成功的采样率。开播清零，旧代次不能更新新计数。
- `i2s_bytes` 是驱动成功接受的源 PCM 字节数，**不是已经从扬声器发出的声学字节数**。
  立体声展开后硬件写入字节数为源 PCM 的两倍；表内所有累计字节统一使用单声道源口径。

背压是保护措施，不证明现场发生了网络突发，也不能修复未知的 I2S/扬声器硬件故障。

## 验证结果

主机测试使用全新目录 `build-wss-recovery/host`，避免旧 TinyCC 对象未跟踪头文件变化造成 ABI 混用。
初次旧目录的段错误和采音断言在全新构建中消失；FFT/键盘测试补用既有
`build/local-capture-deps` 的 NumPy，无新增系统依赖。

**完整 CTest：52/52 通过。** 日志：`build-wss-recovery/host-results.log`。

新增/扩展的生产代码故障注入：

- 实际 WSS session/owner 循环：RX 错误、PING 无响应、服务端 CLOSE，每次清理后继续连接；
  实际 voice session_end 在 TLS 失效、云同步与 ring/播放/文件清理后才发送异步通知。
- 实际 FSM 服务逻辑：S6 断线保持睡眠、离线唤醒被拒；重连 RESET、云 ACK 后 ONLINE 恢复；旧断线
  generation 不覆盖新状态。
- 100 ms 满队列、3 s 请求到期、迟到消费、迟到完成、截止点竞争、16 条取消积压后回收、revision 变化。
- 实际退出回调延迟阻止过期唤醒；实际 enter 中 observer 延迟不再启动过期呈现。
- 独立 monitor 完整循环：完全不运行 WSS owner、FSM 停滞、协作清理恢复、持续网络失败、正常空闲、
  300 s 退避、暂停、计数超限、NVS 写/读失败、复位前恢复。
- 播放空间阈值、连续无空间退出、接收暂停期间上行 poll 仍运行；真实播放 worker 处理大于 128 KiB
  的有背压突发，全部 PCM 被消费且不溢出；24 kHz 配置传递和代次计数清零。
- 既有 MQTT restart、云同步、故障记录、唤醒准入、安静状态和播放取消等回归通过。

这些测试替换了 RTOS 调度、时间、网络和 I2S 边界，执行实际生产函数；不等同于 ESP32 多核竞争、
TLS 故障或真实 DMA/声学验证。

固件使用本地 ESP-IDF 5.5.4 / ESP32-S3 工具链构建，包含整个现有工作区的其它未提交改动。
构建结果与 SHA-256 见本文末的最终记录；没有自动刷机。

重跑命令（PowerShell，使用本机现有工具）：

```powershell
& D:/Espressif/tools/cmake/3.30.2/bin/cmake.exe -S tests/host -B build-wss-recovery/host -G Ninja `
  -DCMAKE_C_COMPILER=D:/Espressif/projects/julia-fused-base/build-ota-name/host-tools/tcc/tcc.exe `
  -DCMAKE_MAKE_PROGRAM=D:/Espressif/tools/ninja/1.12.1/ninja.exe `
  -DPython3_EXECUTABLE=D:/Espressif/python_env/idf5.5_py3.13_env/Scripts/python.exe `
  -DIDF_PATH=D:/Espressif/v5.5.4/esp-idf `
  -DJULIA_NUMPY_PATH=D:/Espressif/projects/julia-fused-base/build/local-capture-deps
& D:/Espressif/tools/cmake/3.30.2/bin/cmake.exe --build build-wss-recovery/host
& D:/Espressif/tools/cmake/3.30.2/bin/ctest.exe --test-dir build-wss-recovery/host --output-on-failure
# 在 ESP-IDF 环境中，仅构建：
idf.py build
```

本次构建直接使用已配置的 `ninja -C build`；为沙箱进程补齐 CMake、Ninja、Xtensa、ccache 的 PATH，
并在进程内禁用 ccache（未修改产品 sdkconfig）。详细输出在 `build-wss-recovery/firmware-build.log`。

## 复现时最有区分力的日志

| 日志/字段 | 判读 |
| --- | --- |
| `health kind=connect_failures`，attempts 增长、进展年龄小 | owner 仍重试；查 Wi-Fi/IP、DNS/TLS、服务器可达性 |
| `health kind=stalled phase=...`，progress_age_ms 超预算 | 对应阶段没有推进；结合 fsm_age_ms 区分 FSM 与 WSS |
| `sync request failed ... committed=0` | 请求未提交即失败；查 FSM 调度/队列/退出回调 |
| `sync request failed ... committed=1` | 已提交而完成迟到；该会话退出，不将请求重放 |
| `soft_recovery` → `controlled_restart` / `reset_suppressed` | 协作清理后仍停滞，或受持久化计数/故障限制 |
| `WSS probe`、`session ended`、`connected & authenticated`、`session_sync` | 对齐销毁、清理、下一次连接与云确认边界 |
| `playback backpressure` / `overflow` | 观察同 generation 的 queued/capacity/rate/rx/accepted/dequeued/i2s_bytes |
| i2s_request_us=0，dequeued=0 | 尚未走到启动/消费；查 worker 调度、retain、预缓冲或前一轮停止 |
| i2s_request_us>0，i2s_started_us=0 | 启动尚未返回成功；查 speaker mutex、I2S 通道配置及错误日志 |
| i2s_started_us>0，i2s_bytes 不增长 | 启动成功但后续写入或其前后路径不推进 |
| 配置率 24000，i2s_bytes 增长明显不足 48000 B/s | 查调度、I2S 实际输出时钟、驱动等待；配置值不是测量值 |
| rx 在很短设备时间内猛增，而云端发送平稳 | 有设备侧集中交付的证据，继续对齐 TCP/网络缓存 |
| accepted-dequeued≈queued；dequeued-i2s_bytes 有差 | 先验证计数口径；通常最多一个在途块，出错/取消后另按清场解释 |

`first_rx_us`、`last_rx_us`、`i2s_*_us` 使用设备启动后的单调时钟，不能直接减服务器墙钟。
将两端同一设备、同一会话、同一次 SPKS/同步交互对齐，保留断线或无声前后至少一分钟日志。

## 未完成实机验证与剩余风险

本机 `python -m serial.tools.list_ports` 返回 `no ports found`。以下均未完成，不能声称现场故障已经解决：

- S6 中断网/恢复、服务端主动关闭、丢弃 PONG、DNS/路由持续失败后的恢复时间；
- 实际 FSM 延迟/队列压力注入及多核超时竞争；
- 24 kHz 下长期回复、无声时 I2S WS/BCLK、DMA/功放状态与统计量对照；
- 下行接收背压对服务端队列、PING 与控制帧时延的影响；
- 超过此前约一小时现场窗口的长时间运行、内存余量、monitor 栈余量和重启限制跨启动验证。

静态请求池占 432 B（最终链接 map），monitor 另增加 4 KiB 栈及 TCB，此外有少量诊断快照；
底层 TLS/驱动/NVS/调度仍依赖 ESP-IDF 正常工作。
独立监测不依赖 FSM 执行，但不是 CPU/中断整体死锁或 NVS 驱动永久停滞的替代硬件看门狗。
已经开始的外设操作不能在调用方超时后强行撤销；没有为了回收资源而跨任务杀任务或销毁 TLS。

## 最终构建记录

- ESP32-S3 固件构建成功，无本次编译 warning/error；大小检查通过。
- `build/julia_fused_base.bin`：`0x27e6a0` B；最小 app 分区 `0x700000` B，剩余约 64%。
- SHA-256：`4F9A43A79182FF32B8CDD3E3BFCA811E6AE52A1BFA19AE6A098F6DC45699F019`。
- 最终主机回归 52/52 通过，`git diff --check` 通过（Git 的换行/用户全局配置访问提示不属于代码检查失败）。
