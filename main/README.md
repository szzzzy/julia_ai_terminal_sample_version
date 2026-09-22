# main 源码组织与运行架构

PWR 已接入长按 3 秒、松手关闭电池供电，包含开机首次松手保护和按键消抖；USB 供电不会被切断。详见 [PWR 按键](../docs/PWR_BUTTON.md)。

> 当前默认开发配置已启用 capture-v1：板级输出完整 320 样本 PCM1 给 `voice_local_capture`，由采音任务维护底噪、预录和段编号，再通过同一 FIFO 上传 start / PCM2 / end。普通段结束由本地事件进入 Think；云端不再下发 MIC 起止。S3/S5/S6 保持连接。详见 [当前链路](../docs/LOCAL_CAPTURE.md)。以下有关“连续 PCM1 上传、云端 MIC_START/MIC_STOP”的描述属于 `CONFIG_JULIA_LOCAL_CAPTURE_ENABLE=n` 的兼容路径，不能套用到新路径。
>
> 同一条链路上还有两个需要区分的开关：FFT 门控与 WebRTC VAD 属正常链路（`sdkconfig.defaults` 已打开）；实验性的键盘频谱历史门控（`CONFIG_JULIA_CAPTURE_NOISE_WINDOW` / `..._TAIL`）Kconfig 默认为关，但本机生效的 `sdkconfig` 已打开，取值是文档里的实验组合而非产品定值，见 [键盘门控复核](../docs/KEYBOARD_GATE_REVIEW_20260918.md) 与 [噪声尾段](../docs/NOISE_TAIL.md)。

文档版本：V1.2。实现核对日期：2026-09-03。项目入口见 [README](../README.md)，源文件注册见 [CMakeLists.txt](CMakeLists.txt)，注释写法见 [源码注释规范](../docs/CODE_COMMENT_STYLE.md)。

## 模块职责

所有下列 `main/` 模块属于同一个 ESP-IDF 组件；板级音频独立位于 `components/julia_board_audio`。

| 目录 | 职能 |
| --- | --- |
| `app/` | 应用入口、启动装配和初始化顺序。 |
| `diagnostics/` | 默认关闭的独立 IMU 实验模式：单动作采集、提示与上传；使用说明见 [IMU 工具](../tools/imu_logger/README.md)。 |
| `behavior/` | 状态图、事件运行时、故障恢复，以及运动、夜间、闲置显示和静默电源策略。 |
| `network/` | Wi-Fi 生命周期、MQTT 和 HTTPS 下载；wss/ 管理 WebSocket 连接、认证与发送。 |
| `voice/` | 实时语音业务装配；capture/ 采集与唤醒，playback/ 播放及 PCM 缓冲，protocol/ 状态同步、控制校验与 URI，uplink/ 上行缓冲与发送泵，demo/ 可选推送演示。 |
| `audio_assets/` | 音频素材清单和下载，当前未接通 MQTT 下载触发。 |
| `time/` | 系统时间服务：RTC 恢复、SNTP 校时及写回 RTC。 |
| `hardware/` | I2C 总线、RTC/IMU 外设访问、电池、电源与 LED。 |
| `display/` | LCD 驱动、面板配置、背光；lvgl_port/ 管理 LVGL 任务、缓冲、刷新同步与锁。 |
| `ui/` | 立绘、眼睛/嘴型、RLE 解码和生成资源，负责界面内容。 |
| `ota/` | 固件清单、下载校验、持久化、启动验收/回滚及状态上报。 |
| `storage/` | SDMMC 挂载接口，目前暂停编译。 |
| `legacy/` | 未参与当前构建的旧版实现，按 voice/context/memory/ui/display/hardware/storage 分类归档。 |

所有运行模块仍属于一个 ESP-IDF main 组件，头文件名称保持不变。旧参考实现不加入运行时 include 路径。板级音频继续位于 `components/julia_board_audio`。

## 推荐阅读顺序

第一次阅读代码时，先从用户可观察的行为逐层进入实现：

1. 阅读“语音与行为状态”，理解开机为什么进入 S3、什么时候进入免唤醒 S1，以及 WSS/MQTT 断开为何结束当前交流。
2. 阅读“任务与所有权”，确认哪个任务实际采集麦克风、播放声音、操作网络和修改设备状态。
3. 阅读“语音传输分层”，区分连接与帧、语音业务、麦克风缓冲和扬声器播放各自负责的范围。
4. 最后再进入各源文件查看队列、锁、超时和错误分支；这些机制用于保证前述业务行为，不应反过来定义业务含义。

| 代码中的名称 | 实际含义 |
| --- | --- |
| `streaming` | 是否正在向服务器发送麦克风声音 |
| `listening` | 是否正在等待用户完成本轮话语 |
| `busy` | 本轮交流尚未完成，不能因空闲计时返回待机 |
| `generation` | 当前连接的数据归属编号，用于丢弃断线前的声音和完成通知 |
| `owner` | 唯一允许操作某条连接、文件或硬件的任务 |
| `ring` | 固定容量的循环缓冲区，满时必须执行明确的失败策略 |

## 应用启动顺序

以 [app/main.c](app/main.c) 和 [启动协调器](app/boot_coordinator.c) 为准，完整依赖、故障策略和验证记录见 [并行启动说明](../docs/BOOT_INITIALIZATION.md)。

1. 电源保持、电源管理、一次电池诊断；识别 OTA 镜像，完成 NVS 保护、netif 和事件循环。pending 镜像的可选 GPIO 诊断仍在外设启用前执行，避免重配 GPIO4 LED 引脚。
2. 串行创建共享 I²C，装配语音观察者和主题，注册 MQTT/WSS/SNTP 回调。总线失败时不启动消费者。
3. 启动 Wi-Fi 已有生命周期；并行执行显示/Avatar/动画、音频/播放/可选本地唤醒、RTC/Idle/FSM 资源/IMU 硬件三条分支。
4. SNTP 只等待时区和 RTC 恢复尝试结束，RTC 失败不阻止 SNTP；MQTT/WSS 此时保持关闭。
5. 协调器按默认 20 秒期限收集资源/完成结果。背光、LCD、Avatar、音频和 FSM 等资源成功后就提交运行期电源档位、建立首次云期限并开放 MQTT/WSS，不等待动画尾段。
6. 握手期间 FSM 保持 S0，不修改动画画面、不接受业务交互。全部本地分支和动画完成后，在 S0 初始化夜间→运动→静默管理并交出画面；电池监测异步启动。
7. 动画结束且业务连接已判决后，FSM 才进入 S3；云端先成功不会提前进入。若连接尚未判决，固定开机亮度显示 S0 CONNECTING，动画结束不重置云期限。
8. pending 镜像在全部本地结果明确后异步验收、确认或回滚；确认和对账完成前拒绝新 OTA。网络不可用不影响镜像健康判断。

首次云连接期限默认 30 秒，从开放门控前由 FSM owner 在 S0 建立；成功才进入 S3（不显示 offline），超时才进入 S3（显示 offline） 并继续后台重连。之后的断线仍保留 S7.1 提示和原运行期恢复逻辑。联网创建失败时 app task 保留指数退避重试。当前 SD 初始化仍暂停。

启动只保留一次 `power_hold` 电压采样，之后 ADC 由电池监测任务使用；电量显示和低电量策略不变。`BOOT` 日志分别给出 app 入口、基础完成、分支开始/结束/错误、动画结束、IP、本地交接、云门控和全部业务就绪时间。不能由移除的固定等待推断实机供电稳定或实际提速。

## 任务与所有权

| 执行上下文 | 所有权／限制 |
| --- | --- |
| `board_mic` | 从 I2S 读取 MIC，转换为 PCM16，通过回调写入上行ring；播放时 WSS 上行与本地 AFE 均持续接收，以支持唤醒和语音打断 |
| WSS 会话任务 | 独占 TLS 句柄；分批处理控制队列／PCM PSRAM ring、接收数据、轮询播放完成事件和推进一个文件块 |
| `voice_playback` | 独占运行时扬声器操作，160 样本一块消费 PCM，负责预缓冲、尾音排空、取消和异常完成 |
| MQTT 事件上下文 | 分片重组与路由；语音命令入队；PUBACK 交给报告处理流程 |
| `julia_fsm` | 从 16 槽消息队列读取行为事件或严重故障；修改唯一运行实例、管理 S3 计时并应用呈现 |
| LVGL／Avatar 任务 | LVGL 周期处理；Avatar 每 40ms 读取嘴型和相位状态，使用 LVGL 锁 |
| 闲置／夜间／运动任务 | 闲置和夜间任务投递 FSM 事件；IMU 在 S3/S5/S6 确认明显运动后投递 `EVT_MOTION_WAKE`，由 FSM 进入 S4 发起交互 |
| OTA 下载与报告任务 | 下载／Flash 工作与 MQTT 事件处理分离；报告按 event_id 关联 PUBACK |

MIC 使用 256 槽（约 5.12 秒、168KB）的 PSRAM SPSC ring，MQTT 控制作业使用独立 4 槽队列。2 的幂容量保证 32 位序号回绕后槽位映射仍连续。正常每轮发送 1 个 MIC 帧；检测到积压后每轮最多 8 帧且不超过 8ms，控制队列仍优先处理且每轮最多 4 条。ring 满时 producer 只关闭入口并请求 `audio_overflow`，WSS owner 统一丢弃本轮、销毁连接和重连，不把缺帧音频继续交给 ASR。FILE_SEND 每轮最多发送一个 1200 字节块，文件区间暂停并清空 MIC ring，END 后从实时新帧恢复。

下行 PCM 写入 128KiB PSRAM 环形缓冲，播放任务独占 I2S；共享互斥只保护缓冲和状态，不覆盖 I2S 或 UI。播放分为唤醒回应、正常回答和自检三种角色：唤醒回应播完保持 S4，只有正常回答播完才由 S2.3 回 S1。MIC_START／断链使旧播放代次失效并清空缓冲。

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

“是否向服务器上传麦克风声音”和“是否正在等待用户说完本轮话语”是两项独立状态。默认服务器唤醒模式在待机时也上传声音，但只有收到 `MIC_START` 后才认为用户已经开始本轮表达。

麦克风 I2S 原始采样在送入本地 AFE 和 WSS 上行前统一应用 `CONFIG_JULIA_MIC_GAIN_PERCENT` 数字增益，当前默认 70%；100% 表示不缩放。
扬声器固定音量为 50%，由 `CONFIG_JULIA_SPEAKER_VOLUME_PERCENT` 配置，回答、断网、低电量、晚安及结束交流提示共用；暂时忽略服务端 `SPKV` 命令，调整音量需重新编译烧录。

默认服务器唤醒模式中，WSS 建连后立即持续上传麦克风声音。服务器命中唤醒词后发送带 `interaction_id` 的 `wake_detected`；设备提交 S3→S4 并回 `state_ready`，随后唤醒回应的 `SPKS` 临时启用闭眼底图上的独立嘴层，播完闭嘴并仍停留 S4。实际有效话语以 `MIC_START` 标记开始、`MIC_STOP` 进入 S2.2“想”，正常回答 `SPKS` 进入 S2.3“说”，实际播完回 S1。收到 MQTT `goodnight`／`dismiss` 时，在 S4 播放“好的，晚安”／“那我不烦你了”并同步嘴型，实际播完再进入 S6／S5；若 MIC_STOP 已使设备进入 S2，则先回到 S4 再播报。细节与时序见 [通信协议](../docs/PROTOCOL.md)。

FSM 有九个主状态：S0 开机、S1 陪伴、S2 对话、S3 待机、S4 发起交互、S5 静默、S6 睡眠、S7 异常、S8 OTA。S2 有 S2.1“听”、S2.2“想”、S2.3“说”三个对话阶段；S7.1 表示 WSS 或 MQTT 刚刚断开并播放本地提示，三秒后按来源策略返回稳定状态；S7.2 表示核心能力不可用，需要保存故障并受控复位。S7 本身不作为可驻留状态。

当前背光策略以 `julia_fsm_runtime.c` 的呈现表为准：S1 固定 `CONFIG_JULIA_COMPANION_BRIGHTNESS_PERCENT`（当前 50%），S3 在 `CONFIG_JULIA_DISPLAY_BREATHE_MIN/MAX_PERCENT`（当前 0%–30%，周期 4000ms）间呼吸，S5 固定 `CONFIG_JULIA_SILENT_BRIGHTNESS_PERCENT`（当前 30%），S6 熄灭并关闭显示；S2 三个阶段为 70%，S4 与 S2.1 共用呈现时为 100%，S0 连接等待固定为开机亮度（当前 50%），S7.2/S8 走默认 100%；S7.1 使用基础立绘、状态字幕和本地语音并固定 50% 亮度。本地资源就绪后在 S0 以固定亮度等待 MQTT/WSS 和语音会话同步；门控开放前开始默认 30 秒期限，成功进入 S3（不显示 offline），超时进入 S3 并叠加红色 `offline`。相关连接全部恢复后删除标签，主状态切换不会清除该标签。

| 当前来源 | 生效状态 | 目标状态 |
| --- | --- | --- |
| 关键初始化成功 | S0 | S3 |
| `EVT_USER_CALL` | S1 | S2.1 |
| `EVT_START_DIALOG` | S2.1 或 S4 | S2.2 |
| `EVT_MULTI_TURN_DETECTED` | S2.2 | S2.3 |
| `EVT_INTERRUPT`／`EVT_USER_CALL` | S2.3 | S2.1 |
| `EVT_SILENCE_TIMEOUT` | S2.3 | S1 |
| `EVT_USER_LEAVE` | S1 | S3 |
| MQTT 会话断开 `EVT_MQTT_DISCONNECTED` | S1～S4（非主动休眠恢复期间） | S7.1，并置 `offline` |
| WSS 会话结束 `EVT_WSS_DISCONNECTED` | S1～S4（非主动休眠恢复期间） | S7.1，并置 `offline` |
| 断联提示完成 `EVT_DISCONNECT_NOTICE_TIMEOUT` | 来自 S3 的 S7.1 | 返回来源稳定状态 |
| 断联提示完成 `EVT_DISCONNECT_NOTICE_TIMEOUT` | 来自 S1/S2/S4 的 S7.1 | S3，不恢复旧会话 |
| MQTT/WSS 重连 | 任意行为状态 | 清除对应离线原因；全部恢复后删除 `offline` |
| 初始业务连接超时 `EVT_SERVICE_CONNECT_TIMEOUT` | 初始 CONNECTING | S0 → S3（显示 offline），继续后台重连 |
| 唤醒词 `EVT_WAKEUP` | S1／S3，且业务 ONLINE | S4；未就绪时丢弃 |
| IMU明显运动 `EVT_MOTION_WAKE` | S3/S5/S6 | S4，发起交互（capture-v1 本地入口） |
| `EVT_NIGHT_TIME`／`EVT_STANDBY_TIMEOUT` | S3 | S6 |
| MQTT `intent_result=goodnight` | S4／S2 任一阶段 | S2 先回 S4，在 S4 播“好的，晚安”并动嘴，播完进入 S6 |
| MQTT `intent_result=dismiss` | S4／S2 任一阶段 | S2 先回 S4，在 S4 播“那我不烦你了”并动嘴，播完进入 S5 |
| `EVT_SILENT_TIMEOUT`（默认 30 分钟） | S5 | S6 |
| OTA 引擎接受升级 | S3（S5/S6 静默期间显式拒绝） | S8 |
| OTA 任务失败（非链路、非严重故障） | S8 | S3 |
| OTA 提交成功、即将复位 | S8 | S0 |
| 严重故障消息 | S0～S6／S7.1／S8 | S7.2 |
| 自动复位 | S7.2 | S0 |

`intent_result=normal` 只表示没有特殊语义，不改变状态。OTA 任务、NVS 检查点、目标分区、启动分区设置或普通镜像校验失败都不会触发 S7.2，因为活动固件尚未被替换；这类失败由 S8 回到 S3 等待唤醒。Wi-Fi、TLS、HTTP 等错误导致 OTA 任务退出时同样回到 S3，保留符合恢复条件的下载断点，后续仍按既有检查/通知机制尝试。只有已经无法回滚到可用固件时才从 S8 进入 S7.2。

S7.2 只接收关键初始化、FSM 内部损坏和 OTA 无法安全恢复等严重故障，并保存 NVS 快照后按策略复位。S7.1 不写严重故障快照、不触发复位：每个离线周期只在第一次由 `ONLINE` 变为 `OFFLINE` 时播放固件内嵌的 `network_disconnected_16k_mono_16bit.wav`，嘴型按实际送往扬声器的本地 PCM能量同步，并显示 `S7.1 DISCONNECTED` 与 `offline`；保持离线期间的重复断联不再播报。三秒后，S3 返回原稳定状态；S5/S6 主动休眠不进入断联提示，也不自动重连；S1的免唤醒资格与 S2/S4的交互上下文均绑定旧 WSS generation，断联后统一进入 S3。WSS/MQTT 各自继续后台重连。OTA 任务发生链路错误退出后回 S3；仅业务链路断开而 OTA 仍在运行时，不套用 S7.1。

## 编译范围与参考源码

以下源码不在当前 `srcs` 中，不应作为默认运行链路的修改入口：

- `legacy/voice/julia_voice.c`、`legacy/context/julia_context.c`、`legacy/memory/julia_memory.c`、`legacy/memory/julia_routine.c`。
- `legacy/ui/julia_ui.c`、`legacy/ui/julia_display_theme.c`、`legacy/ui/avatar_micro_motion.c`、`legacy/ui/avatar_micro_action.c`、`legacy/ui/avatar_parts/avatar_face.c`。
- `legacy/voice/julia_lipsync.c`、`legacy/display/st77916_qspi.c`、`legacy/storage/julia_sd.c`。
- `legacy/hardware/PCF85063/PCF85063.c`、`legacy/hardware/QMI8658/QMI8658.c`。

其中部分文件依赖仓库中不存在的头文件。接入这些模块前应先确定模块所有权、依赖、初始化顺序和并发模型，不能只把文件加入 CMake。

`wake_detector.c` 只在 `CONFIG_JULIA_SERVER_WAKE_ENABLE` 关闭时编译。`LISTEN.bin`、`THINK.bin`、`SPEAK.bin` 列入嵌入资源，但当前相位选图不调用它们的 RLE 解码路径；详见 [显示与交互](../docs/UI_L0L1_PORT.md)。

## 开发约定

- 先确认源文件确实参与构建、接口确实有调用者，再判断其运行行为。
- 同一外设只保留一个运行时所有者；RTC 与 IMU 复用 TCA9554 建立的 I2C 总线。
- `wss_transport_send_now()` 仅供 WSS 会话上下文调用，其他任务通过业务入队接口发送。
- 源码注释用于辅助理解，协议与文档结论应由实际调用路径、配置和验证记录支撑。
- 变更帧格式、命令语义、镜像标识或分区布局时，同时维护协议、发布说明和对应验收项。
