# main 源码组织与运行架构

文档版本：V1.1。实现核对日期：2026-09-01。项目入口见 [README](../README.md)，源文件注册见 [CMakeLists.txt](CMakeLists.txt)。

## 模块职责

所有下列 `main/` 模块属于同一个 ESP-IDF 组件；板级音频独立位于 `components/julia_board_audio`。

| 目录 | 当前职责与入口 |
| --- | --- |
| `app/` | `main.c` 装配服务；`julia_idle_display.c` 维护活动时间与显示忙碌状态 |
| `voice/` | `voice_service.c` 处理语音业务；`voice_playback.c` 与 `pcm_buffer.c` 管理播放；`wss_transport.c` 管理传输；`voice_uri.c` 映射文件路径 |
| `network/` | Wi-Fi 后台生命周期、MQTT 主题路由与 OTA 控制；`http_downloader.c` 提供通用下载器 |
| `ota/` | OTA 清单校验、下载、持久化、隔离／冷却、启动验收及可靠状态上报 |
| `audio/` | 音频素材清单和下载引擎；应用只调用初始化占位入口，未接通 MQTT 下载触发 |
| `fsm/` | `julia_fsm.c` 定义状态图和现有事件映射；`julia_fsm_runtime.c` 串行处理事件、S3 计时和 S7 呈现；`julia_fault.c` 保存严重故障快照 |
| `context/` | `julia_time.c` 恢复 RTC／执行 SNTP；夜间调度和 IMU 运动检测产生 FSM 事件 |
| `display/` | `julia_display.c` 配置 QSPI 面板；`esp_lcd_st77916.c` 提供面板驱动 |
| `lvgl_port/` | 显示缓冲、刷新完成同步、LVGL 任务与互斥接口 |
| `ui/` | `julia_avatar.c` 管理立绘和对话相位；眼睛、嘴型部件与 `julia_backlight.c` 提供呈现 |
| `hardware/` | TCA9554 共享 I2C、RTC／IMU 访问；LED 模块有编译入口但未由应用初始化 |
| `storage/` | `sd_card.c` 执行 SDMMC 挂载；不是热插拔监视服务 |
| `memory/` | 记忆与例行参考源码，不参与当前构建 |

## 应用启动顺序

以 [app/main.c](app/main.c) 的实际调用顺序为准：

1. 进入 `app_main()` 后立即把 GPIO7 `BAT_Control` 拉高以锁存电池供电，并启用 CPU 240/80MHz 动态调频（不开自动 Light-sleep）；然后由 `ota_boot_flow_run()` 初始化 NVS、网络基础设施与事件循环，处理镜像健康检查、启动确认／回滚和报告对账。
2. 初始化背光、LCD、LVGL、Avatar，交由独立 `boot_animation` 任务执行眨眼序列。
3. 主任务同时注册语音命令、初始化板级音频与独立播放任务、音频素材服务和 RTC，并挂载 SD。
4. 注册 IP-ready 回调并启动 Wi-Fi；扫描／连接及可用时的 SNTP 与动画重叠。
5. 等动画完成后启动闲置显示和 FSM；关键显示、音频或语音依赖失败时进入 S7，否则由 S0 直接进入等待唤醒词的 S3。
6. FSM 就绪后启动夜间、运动及可选本地唤醒输入。
7. 打开交互启动门槛，唤醒网络服务重试；MQTT／WSS 仅在此前置条件满足后启动，避免抢占开机画面。
8. 仅在 `CONFIG_VOICE_PUSH_DEMO_ENABLE` 启用时启动文件推送演示。

动画与非交互初始化并行，动画任务资源不足时回退为顺序启动；两条路径都完成后才允许交互接管显示。网络不可达不阻塞该汇合点。OTA 启动验收仍在最前执行，外设错误不会被此并行流程自动变为产品验收失败。

共享 LCD／I2C 基础设施在启动动画任务前建立，避免动画与外设初始化重复创建总线。夜间和运动任务在 FSM 就绪后启动，防止向不存在的状态实例投递事件。MQTT 等待 `s_runtime_ready`；WSS 还要求语音注册、板级音频／播放任务和 FSM 初始化成功。门槛开放后调用 `network_lifecycle_retry_services()`，使已有 IP 的待启动服务尽快重试。

## 任务与所有权

| 执行上下文 | 所有权／限制 |
| --- | --- |
| `board_mic` | 从 I2S 读取 MIC，转换为 PCM16，通过回调写入上行ring；播放时 WSS 上行与本地 AFE 均持续接收，以支持唤醒和语音打断 |
| WSS 会话任务 | 独占 TLS 句柄；分批处理控制队列／PCM PSRAM ring、接收数据、轮询播放完成事件和推进一个文件块 |
| `voice_playback` | 独占运行时扬声器操作，160 样本一块消费 PCM，负责预缓冲、尾音排空、取消和异常完成 |
| MQTT 事件上下文 | 分片重组与路由；语音命令入队；PUBACK 交给报告处理流程 |
| `julia_fsm` | 从 16 槽消息队列读取行为事件或严重故障；修改唯一运行实例、管理 S3 计时并应用呈现 |
| LVGL／Avatar 任务 | LVGL 周期处理；Avatar 每 40ms 读取嘴型和相位状态，使用 LVGL 锁 |
| 闲置／夜间／运动任务 | 闲置和夜间任务投递 FSM 事件；IMU 在 S6 只记录运动诊断，不覆盖显示或行为状态 |
| OTA 下载与报告任务 | 下载／Flash 工作与 MQTT 事件处理分离；报告按 event_id 关联 PUBACK |

MIC 使用 256 槽（约 5.12 秒、168KB）的 PSRAM SPSC ring，MQTT 控制作业使用独立 4 槽队列。2 的幂容量保证 32 位序号回绕后槽位映射仍连续。正常每轮发送 1 个 MIC 帧；检测到积压后每轮最多 8 帧且不超过 8ms，控制队列仍优先处理且每轮最多 4 条。ring 满时 producer 只关闭入口并请求 `audio_overflow`，WSS owner 统一丢弃本轮、销毁连接和重连，不把缺帧音频继续交给 ASR。FILE_SEND 每轮最多发送一个 1200 字节块，文件区间暂停并清空 MIC ring，END 后从实时新帧恢复。

下行 PCM 写入 64KiB PSRAM 环形缓冲，播放任务独占 I2S；共享互斥只保护缓冲和状态，不覆盖 I2S 或 UI。播放分为唤醒回应、正常回答和自检三种角色：唤醒回应播完保持 S4，只有正常回答播完才由 S2.3 回 S1。MIC_START／断链使旧播放代次失效并清空缓冲。

### 实时音频模块边界

| 模块 | 职责 | 不负责的内容 |
| --- | --- | --- |
| `voice_service.c` | 网络命令语义、播放请求、完成事件收尾、FSM 事件和分块文件外发 | 不直接执行播放 I2S 写入 |
| `voice_playback.c` | 单一播放任务、预缓冲、代次取消、超时、溢出处理、尾音排空 | 不访问 TLS，不直接推进行为 FSM |
| `pcm_buffer.c` | PCM16 环形 FIFO、边界检查、输入结束与重置 | 不包含 RTOS 同步或硬件调用；调用方提供锁 |
| `wss_transport.c` | TLS／WebSocket、独立控制队列、有限批次分派和 on_poll | 不解释 MIC／SPK／文件内容 |
| `voice_uplink_ring.c` | 连接 generation 隔离的 PSRAM PCM SPSC ring | 不访问 TLS、FSM 或板级 MIC |
| `voice_uplink_pump.c` | 正常发送、积压追赶、批次／时间预算与恢复指标 | 不拥有 producer 或 TLS 生命周期 |
| `components/julia_board_audio` | MIC I2S 采集、PCM1 打包、同步扬声器底层操作 | 不承担 WSS 接收或语音播放调度 |

会话结束以及新会话开始时清理控制队列和 MIC ring，未就绪会话拒绝新作业。MIC ring 只允许同一 WSS generation 内继续发送；重连后的新 generation 从空 ring 开始，绝不重放断线前 PCM。transport 为 peer close、收发错误、写停滞、保活超时、协议错误、业务错误和音频溢出保留独立结束原因。文件传输期间只发送文件 binary，语音启动通过 `ERROR file_cancelled` 结束文件区间，防止将文件字节与 MIC PCM1 混淆。

播放任务的完成信息包含本地 generation；它只防止设备内部的旧任务结果覆盖新状态，不是服务端的 turn_id。具体参数与推流约束见 [通信协议](../docs/PROTOCOL.md)。

普通跨模块行为通过 `julia_fsm_runtime_post()` 投递，严重故障通过 `julia_fsm_runtime_raise_fault()` 投递到队首。S3 的驻留计时由 FSM 运行时持有，默认 5 分钟，由 `CONFIG_JULIA_STANDBY_SLEEP_TIMEOUT_SECONDS` 配置；语音播放结束仍由现有 `EVT_SILENCE_TIMEOUT` 生产者推进。

## 语音状态与显示状态

`s_mic_streaming` 表示是否上传音频，`s_dialog_listening` 表示是否处于用户听音阶段；两者不等价。

麦克风 I2S 原始采样在送入本地 AFE 和 WSS 上行前统一应用 `CONFIG_JULIA_MIC_GAIN_PERCENT` 数字增益，当前默认 70%；100% 表示不缩放。
扬声器上电默认音量为 50%，由 `CONFIG_JULIA_SPEAKER_VOLUME_PERCENT` 配置；连接后服务端仍可通过 `SPKV` 命令动态覆盖。

默认服务器唤醒模式中，WSS 建连后 streaming 为真。服务器命中唤醒词后发送带 `interaction_id` 的 `wake_detected`；设备提交 S3/S5/S6→S4 并回 `state_ready`，随后唤醒回应的 `SPKS` 临时启用闭眼底图上的独立嘴层，播完闭嘴并仍停留 S4。实际有效话语以 `MIC_START` 标记开始、`MIC_STOP` 进入 S2.2“想”，正常回答 `SPKS` 进入 S2.3“说”，实际播完回 S1。MQTT `goodnight`／`dismiss` 不播语音，分别直接进入 S6／S5。细节与时序见 [通信协议](../docs/PROTOCOL.md)。

FSM 有九个主状态：S0 开机、S1 陪伴、S2 对话、S3 待机、S4 发起交互、S5 静默、S6 睡眠、S7 故障、S8 OTA。只有 S2 有 S2.1“听”、S2.2“想”、S2.3“说”三个子状态。启动完成后 S0 直接进入等待唤醒词的 S3；对话结束才进入 S1 免唤醒陪伴期。当前默认值为：S1 连续空闲 10 分钟进入 S3，S3 连续驻留 5 分钟或命中 23:00～07:00 夜间条件进入 S6；这些时间均由 `CONFIG_JULIA_*` 配置项控制。

当前背光策略为：常驻 S1 固定 50%，S3 在 5%–30% 间呼吸，S5 固定 50%，S6 熄灭；交互状态和非常驻的 S0/S7/S8 保持 100%。

| 当前来源 | 生效状态 | 目标状态 |
| --- | --- | --- |
| 关键初始化成功 | S0 | S3 |
| `EVT_USER_CALL` | S1 | S2.1 |
| `EVT_START_DIALOG` | S2.1 或 S4 | S2.2 |
| `EVT_MULTI_TURN_DETECTED` | S2.2 | S2.3 |
| `EVT_INTERRUPT`／`EVT_USER_CALL` | S2.3 | S2.1 |
| `EVT_SILENCE_TIMEOUT` | S2.3 | S1 |
| `EVT_USER_LEAVE` | S1 | S3 |
| Wi-Fi 断联 `EVT_WIFI_DISCONNECTED` | S1／S2 任一阶段／S4 | S3 |
| WSS 会话结束 `EVT_WSS_DISCONNECTED` | S1／S2 任一阶段／S4 | S3 |
| 唤醒词 `EVT_WAKEUP` | S3／S5／S6 | S4 |
| `EVT_NIGHT_TIME`／`EVT_STANDBY_TIMEOUT` | S3 | S6 |
| MQTT `intent_result=goodnight` | S4／S2 任一阶段 | S6 |
| MQTT `intent_result=dismiss` | S4／S2 任一阶段 | S5（Companion 底图，默认 50% 亮度） |
| `EVT_SILENT_TIMEOUT`（默认 30 分钟） | S5 | S3 |
| OTA 引擎接受升级 | S0／S1／S3 | S8 |
| OTA 任务失败（非链路、非严重故障） | S8 | S3 |
| OTA 提交成功、即将复位 | S8 | S0 |
| 严重故障消息 | S0～S6／S8 | S7 |
| 自动复位 | S7 | S0 |

`intent_result=normal` 只表示没有特殊语义，不改变状态。OTA 任务、NVS 检查点、目标分区、启动分区设置或普通镜像校验失败都不会触发 S7，因为活动固件尚未被替换；这类失败由 S8 回到 S3 等待唤醒。Wi-Fi、TLS、HTTP 等临时链路失败保持 S8 和下载断点，等待现有恢复流程。只有已经无法回滚到可用固件时才从 S8 进入 S7。

S7 只接收关键初始化、FSM 内部损坏和 OTA 无法安全恢复等严重故障。进入 S7 时使用 `julia_fault` NVS namespace 保存快照；默认显示 3 秒后复位，同类快速故障连续超过三次后保持 S7，具体由 `CONFIG_JULIA_FAULT_*` 配置。当前调试 UI 与 Companion 共用底图，依靠左上 `S7 FAULT` 状态码区分；正式故障素材后续再接入。普通网络断线、单轮会话失败和普通 OTA 包拒绝不进入 S7。

## 编译范围与参考源码

以下源码不在当前 `srcs` 中，不应作为默认运行链路的修改入口：

- `julia_voice.c`、`context/julia_context.c`、`memory/julia_memory.c`、`memory/julia_routine.c`。
- `ui/julia_ui.c`、`ui/julia_display_theme.c`、`ui/avatar_micro_motion.c`、`ui/avatar_micro_action.c`、`ui/avatar_parts/avatar_face.c`。
- `voice/julia_lipsync.c`、`display/st77916_qspi.c`、`storage/julia_sd.c`。
- `PCF85063/PCF85063.c`、`QMI8658/QMI8658.c`。

其中部分文件依赖仓库中不存在的头文件。接入这些模块前应先确定模块所有权、依赖、初始化顺序和并发模型，不能只把文件加入 CMake。

`wake_detector.c` 只在 `CONFIG_JULIA_SERVER_WAKE_ENABLE` 关闭时编译。`LISTEN.bin`、`THINK.bin`、`SPEAK.bin` 列入嵌入资源，但当前相位选图不调用它们的 RLE 解码路径；详见 [显示与交互](../docs/UI_L0L1_PORT.md)。

## 开发约定

- 先确认源文件确实参与构建、接口确实有调用者，再判断其运行行为。
- 同一外设只保留一个运行时所有者；RTC 与 IMU 复用 TCA9554 建立的 I2C 总线。
- `wss_transport_send_now()` 仅供 WSS 会话上下文调用，其他任务通过业务入队接口发送。
- 源码注释用于辅助理解，协议与文档结论应由实际调用路径、配置和验证记录支撑。
- 变更帧格式、命令语义、镜像标识或分区布局时，同时维护协议、发布说明和对应验收项。
