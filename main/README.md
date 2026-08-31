# main 源码组织与运行架构

文档版本：V1.0。实现核对日期：2026-08-31。项目入口见 [README](../README.md)，源文件注册见 [CMakeLists.txt](CMakeLists.txt)。

## 模块职责

所有下列 `main/` 模块属于同一个 ESP-IDF 组件；板级音频独立位于 `components/julia_board_audio`。

| 目录 | 当前职责与入口 |
| --- | --- |
| `app/` | `main.c` 装配服务；`julia_idle_display.c` 维护活动时间与显示忙碌状态 |
| `voice/` | `voice_service.c` 处理语音业务；`voice_playback.c` 与 `pcm_buffer.c` 管理播放；`wss_transport.c` 管理传输；`voice_uri.c` 映射文件路径 |
| `network/` | Wi-Fi 后台生命周期、MQTT 主题路由与 OTA 控制；`http_downloader.c` 提供通用下载器 |
| `ota/` | OTA 清单校验、下载、持久化、隔离／冷却、启动验收及可靠状态上报 |
| `audio/` | 音频素材清单和下载引擎；应用只调用初始化占位入口，未接通 MQTT 下载触发 |
| `fsm/` | `julia_fsm.c` 是状态转换逻辑；`julia_fsm_runtime.c` 持有运行实例并应用显示映射 |
| `context/` | `julia_time.c` 恢复 RTC／执行 SNTP；夜间调度和 IMU 运动检测产生 FSM 事件 |
| `display/` | `julia_display.c` 配置 QSPI 面板；`esp_lcd_st77916.c` 提供面板驱动 |
| `lvgl_port/` | 显示缓冲、刷新完成同步、LVGL 任务与互斥接口 |
| `ui/` | `julia_avatar.c` 管理立绘和对话相位；眼睛、嘴型部件与 `julia_backlight.c` 提供呈现 |
| `hardware/` | TCA9554 共享 I2C、RTC／IMU 访问；LED 模块有编译入口但未由应用初始化 |
| `storage/` | `sd_card.c` 执行 SDMMC 挂载；不是热插拔监视服务 |
| `memory/` | 记忆与例行参考源码，不参与当前构建 |

## 应用启动顺序

以 [app/main.c](app/main.c) 的实际调用顺序为准：

1. `ota_boot_flow_run()` 初始化 NVS、网络基础设施与事件循环，处理镜像健康检查、启动确认／回滚和报告对账。
2. 初始化背光、LCD、LVGL、Avatar，交由独立 `boot_animation` 任务执行眨眼序列。
3. 主任务同时注册语音命令、初始化板级音频与独立播放任务、音频素材服务和 RTC，并挂载 SD。
4. 注册 IP-ready 回调并启动 Wi-Fi；扫描／连接及可用时的 SNTP 与动画重叠。
5. 等动画完成后启动闲置显示和 FSM，再启动夜间、运动及可选本地唤醒输入。
6. 打开交互启动门槛，唤醒网络服务重试；MQTT／WSS 仅在此前置条件满足后启动，避免抢占开机画面。
7. 仅在 `CONFIG_VOICE_PUSH_DEMO_ENABLE` 启用时启动文件推送演示。

动画与非交互初始化并行，动画任务资源不足时回退为顺序启动；两条路径都完成后才允许交互接管显示。网络不可达不阻塞该汇合点。OTA 启动验收仍在最前执行，外设错误不会被此并行流程自动变为产品验收失败。

共享 LCD／I2C 基础设施在启动动画任务前建立，避免动画与外设初始化重复创建总线。夜间和运动任务在 FSM 就绪后启动，防止向不存在的状态实例投递事件。MQTT 等待 `s_runtime_ready`；WSS 还要求语音注册、板级音频／播放任务和 FSM 初始化成功。门槛开放后调用 `network_lifecycle_retry_services()`，使已有 IP 的待启动服务尽快重试。

## 任务与所有权

| 执行上下文 | 所有权／限制 |
| --- | --- |
| `board_mic` | 从 I2S 读取 MIC，转换为 PCM16，通过回调入队；播放时继续向 WSS 上传，暂停本地 AFE 输入 |
| WSS 会话任务 | 独占 TLS 句柄；分批处理控制／PCM 队列、接收数据、轮询播放完成事件和推进一个文件块 |
| `voice_playback` | 独占运行时扬声器操作，160 样本一块消费 PCM，负责预缓冲、尾音排空、取消和异常完成 |
| MQTT 事件上下文 | 分片重组与路由；语音命令入队；PUBACK 交给报告处理流程 |
| `julia_fsm` | 从 16 槽事件队列读取事件，修改唯一运行实例并应用呈现 |
| LVGL／Avatar 任务 | LVGL 周期处理；Avatar 每 40ms 读取嘴型和相位状态，使用 LVGL 锁 |
| 闲置／夜间／运动任务 | 分别观察活动时间、有效墙钟和 IMU；通过事件接口影响 FSM |
| OTA 下载与报告任务 | 下载／Flash 工作与 MQTT 事件处理分离；报告按 event_id 关联 PUBACK |

MIC 使用 8 槽发送队列，MQTT 控制作业使用独立 4 槽队列。每次会话循环最多各处理 4 条，给下行与保活留出机会；MIC 入队失败有累计日志。FILE_SEND 每轮最多发送一个 1200 字节块，文件区间暂不发送 PCM1。

下行 PCM 写入 64KiB PSRAM 环形缓冲，播放任务独占 I2S；共享互斥只保护缓冲和状态，不覆盖 I2S 或 UI。正常 SPKE 只标记输入结束，播放完成事件回到 WSS 任务后才清 busy。MIC_START／断链使本地播放代次失效并清空缓冲，旧完成事件不能将新监听恢复成待机。

### 实时音频模块边界

| 模块 | 职责 | 不负责的内容 |
| --- | --- | --- |
| `voice_service.c` | 网络命令语义、播放请求、完成事件收尾、FSM 事件和分块文件外发 | 不直接执行播放 I2S 写入 |
| `voice_playback.c` | 单一播放任务、预缓冲、代次取消、超时、溢出处理、尾音排空 | 不访问 TLS，不直接推进行为 FSM |
| `pcm_buffer.c` | PCM16 环形 FIFO、边界检查、输入结束与重置 | 不包含 RTOS 同步或硬件调用；调用方提供锁 |
| `wss_transport.c` | TLS／WebSocket、独立控制与 MIC 队列、有限批次分派和 on_poll | 不解释 MIC／SPK／文件内容 |
| `components/julia_board_audio` | MIC I2S 采集、PCM1 打包、同步扬声器底层操作 | 不承担 WSS 接收或语音播放调度 |

会话结束以及新会话开始时清理发送队列，未就绪会话拒绝新作业；这些队列不是断线后的可靠重放存储。文件传输期间只发送文件 binary，语音启动通过 `ERROR file_cancelled` 结束文件区间，防止将文件字节与 MIC PCM1 混淆。

播放任务的完成信息包含本地 generation；它只防止设备内部的旧任务结果覆盖新状态，不是服务端的 turn_id。具体参数与推流约束见 [通信协议](../docs/PROTOCOL.md)。

跨模块行为通过 `julia_fsm_runtime_post()` 投递，调用方应检查返回值；FSM 逻辑本身不提供超时定时器。`EVT_SILENCE_TIMEOUT` 是事件名称，必须有实际生产者才能触发退出。

## 语音状态与显示状态

`s_mic_streaming` 表示是否上传音频，`s_dialog_listening` 表示是否处于用户听音阶段；两者不等价。

默认服务器唤醒模式中，WSS 建连后 streaming 为真，界面保持待机。`MIC_START` 进入听音；`MIC_STOP` 进入思考并保留上传；正常对话中的 `SPKS` 进入说话；`SPKE` 排空已经接收的音频后请求返回待机。细节与异常边界见 [通信协议](../docs/PROTOCOL.md)。

FSM 有六个主状态：S0 休眠、S1 待机、S2 陪伴、S3 主动、S4 对话、S5 静默。状态定义不表示全部感知来源已实现；例如情感、用户拒绝、充电和低电量事件还需要产品侧输入链路。

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
