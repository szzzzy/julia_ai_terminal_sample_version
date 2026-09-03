# 显示与交互

文档版本：V1.0。本文描述当前 ST77916／LVGL／Avatar 显示链路，不包含完整转场播放器或相位视频引擎。返回 [项目入口](../README.md)。

## 1. 显示链路

```text
app_main
  → julia_backlight_init
  → julia_display_init → ST77916 panel + lvgl_port
  → julia_avatar_init → 底图 + 眼睛／嘴型部件
  → boot_animation 任务执行眨眼
  ├─ 主任务：音频 / RTC / SD / Wi-Fi 初始化
  └─ 动画与初始化汇合 → idle / FSM / 情境输入 → 放行交互

语音／时钟／运动事件 → julia_fsm_runtime → Avatar 相位与背光
下行 PCM → PSRAM 缓冲 → voice_playback / I2S → julia_avatar_feed_pcm → 嘴型
```

实际文件是 `main/display/julia_display.c`、`main/display/esp_lcd_st77916.c`、`main/lvgl_port/lvgl_port.c`、`main/ui/julia_avatar.c` 和 `main/ui/julia_backlight.c`。`julia_ui.c`、`st77916_qspi.c`、`julia_display_theme.c` 不参与当前构建。

## 2. 板级参数与刷新

| 项目 | 当前值／来源 |
| --- | --- |
| 分辨率 | 360×360；驱动、LVGL 和 Avatar 中均有对应常量 |
| 接口 | SPI2 QSPI，像素时钟 40MHz |
| QSPI 引脚 | SCK=40，D0=46，D1=45，D2=42，D3=41，CS=21 |
| 背光 | GPIO 5，LEDC，由 `julia_backlight.c` 管理 |
| 像素格式 | RGB565，`LV_COLOR_16_SWAP=y` |
| LVGL 缓冲 | 两个 12960 像素绘制缓冲，各为整屏的 1/10 |
| LVGL 周期 | tick 2ms，handler 循环间隔 10ms |
| Avatar 周期 | 40ms，处理嘴型、相位重试等 |

任务周期不是屏幕实测帧率。整屏传输会拆分为多条带，刷新受总线、DMA、锁竞争和重绘面积影响。

所有 LVGL 对象访问应使用 `lvgl_port_lock()`／`unlock()`。音频入口只维护嘴型目标数据，不在 PCM 回调中绘制图像。具体同步与刷新完成处理见 [lvgl_port.c](../main/lvgl_port/lvgl_port.c)。

## 3. 开机呈现

背光与最小显示链路先初始化，再执行一次眨眼序列：8 次睁眼／闭眼，时长分别为 255ms／120ms，眨眼部分合计约 3 秒，另有背光渐变与刷新等待。

眨眼在独立任务中执行，音频、RTC、SD 和 Wi-Fi 初始化同时进行；网络服务和行为呈现受启动门槛保护，不覆盖动画。动画和初始化均完成后才启动闲置／FSM 呈现、夜间与运动输入，并放行 MQTT／WSS。动画任务创建失败时回退顺序执行，OTA 启动检查顺序不变。

日志分别记录动画开始／结束、并行服务耗时和交互门槛开放耗时。“首屏可见”“初始化汇合”和“WSS 可以对话”应分别测量，当前没有开机语音提示流程。

| 日志 | 计时含义 |
| --- | --- |
| `Boot animation start` | 动画任务开始执行 |
| `Boot animation done in ...ms` | 该动画任务自身的执行时长，包含刷新／渐变等待 |
| `Parallel services initialized in ...ms` | 从 OTA 启动流程返回后的计时点，到音频／RTC／SD／网络启动调用完成；不是联网成功时间 |
| `Boot interaction gate open in ...ms (voice_ready=...)` | 从同一计时点到动画汇合及运行时初始化完成；voice_ready 还反映语音依赖是否就绪 |

后两项包含显示基础设施初始化，但不包含更早的 bootloader 和 OTA 启动验收，不能当作上电总时长。WSS 会话建立发生在交互放行和取得 IP 之后，仍受网络与服务器响应影响。并行编排只重叠无画面争用的初始化，没有把所有初始化任意并发。

## 4. 对话呈现

| 相位 | 当前画面 | 眼睛／嘴型 |
| --- | --- | --- |
| IDLE | 基础立绘（资源仍沿用旧名 S1.1） | 随机眨眼，非说话时嘴型闭合 |
| LISTENING | 完整闭眼立绘 | 独立眼睛和嘴型层隐藏 |
| THINKING | 基础立绘（资源仍沿用旧名 S1.1） | 保持睁眼；不播放回答嘴型 |
| SPEAKING | 基础立绘（资源仍沿用旧名 S1.1） | 保持睁眼；由下行 PCM 能量驱动嘴型 |
| Dozing | 闭眼立绘覆盖 | 与背光策略组合，不等于芯片睡眠 |

相位选图以 `avatar_source_for_phase()` 为准。`main/ui/generated/clips/LISTEN.bin`、`THINK.bin`、`SPEAK.bin` 列入嵌入文件，但当前选图路径不调用其 RLE 解码器，不能把这些文件视为正在播放的相位动画。

独立眼层的生成基准坐标为左眼 `(112,108)`、右眼 `(194,108)`。为覆盖闭眼时底图下缘残留，半闭帧临时下移 1px、全闭帧下移 2px；恢复睁眼时回到基准坐标。该偏移只影响周期眨眼和持续闭眼帧，不移动正常睁眼位置。

`AVATAR_ENABLE_FULL_FRAME_MOTION=0`，整幅立绘的缩放呼吸和点头不执行。背光呼吸、眼睛局部动画和整幅画面微动是不同能力。

## 5. 嘴型

`voice_service_on_binary()` 只投递 PCM；独立播放任务每写入一个 160 样本块后调用 `julia_avatar_feed_pcm()`，避免网络突发直接推动嘴型。RMS 平滑与 talking start／stop 使用同一状态锁，非 talking 时忽略迟到 PCM。

- 上升门限：300、950、2300；下降门限：180、650、1650。
- Avatar 每 40ms 消费目标档位。
- PCM 保持时间为 180ms，超时或非 talking 状态时闭嘴。
- 正常 `SPKE` 在缓冲与尾音排空后清除嘴型；会话结束和语音打断立即取消嘴型。

这里是按 PCM 能量同步的嘴型，不是音素／口型识别。驱动写入成功不等于声音已在物理扬声器上播放到同一时刻，音画偏移应上板测量。

## 6. FSM 映射

状态机定义在 [julia_fsm.h](../main/fsm/julia_fsm.h)，呈现由 [julia_fsm_runtime.c](../main/fsm/julia_fsm_runtime.c) 绑定。

| 行为状态 | 呈现 |
| --- | --- |
| S0 开机、S1 陪伴、S5 静默、S7 故障、S8 OTA | 调试阶段共用 Companion 基础立绘，S1、S5 固定 50% 背光，其余由左上状态码区分 |
| S2.1 听 | 复用现有 LISTEN 呈现 |
| S2.2 想 | 复用现有 THINK 呈现 |
| S2.3 说 | 复用现有 SPEAK 呈现和 PCM 嘴型 |
| S3 待机 | 闭眼与 5%–30% 背光呼吸 |
| S6 睡眠 | 闭眼且背光熄灭 |
| S4 发起交互 | 暂时复用 S2.1 的 LISTEN 呈现；正常话语结束进入 S2.2，特殊语义可进入 S5 |

这是调试阶段呈现映射。听、想、说继续复用项目已有语音和播放链路；尚无正式素材的主状态统一使用 Companion 基础立绘，依靠固定状态码观察迁移。S7 的 NVS 记录和复位逻辑不受临时 UI 复用影响。

屏幕固定覆盖当前已启用的 14px 黑色小号状态叠字，坐标为 `(60,100)`、宽度为 220px，不随 Avatar 微动根对象移动。主状态显示 `S0 BOOT`～`S8 OTA`，S2 显示 `S2.1 LISTEN`、`S2.2 THINK`、`S2.3 SPEAK`。状态在 UI 初始化前发生时先缓存，标签创建后再应用；每次更新都会重新设置坐标，避免其他 UI 操作覆盖位置。

## 7. 待机、夜间与运动

### 活动时间

`julia_idle_display.c` 按 `CONFIG_JULIA_DISPLAY_ACTIVITY_POLL_MS` 检查活动时间。非 busy 且连续达到 `CONFIG_JULIA_DISPLAY_SLEEP_TIMEOUT_SECONDS`（默认 600 秒）无交互时投递 `EVT_USER_LEAVE`，由 S1 陪伴进入 S3 待机。闲置任务不直接操作立绘；FSM 运行时在进入 S3 后统一应用闭眼和背光呼吸。听音／思考／说话期间 busy 为真，普通闲置逻辑不降档。

进入 S3 后由 FSM 运行时启动独立的一次性计时器；连续驻留达到 `CONFIG_JULIA_STANDBY_SLEEP_TIMEOUT_SECONDS`（默认 300 秒）仍未唤醒时投递 `EVT_STANDBY_TIMEOUT`，由 S3 进入 S6。默认 23:00～07:00 的 RTC 夜间事件仍独立生效，可在驻留计时到期前先进入 S6。

进入 S5 后同样启动由 `CONFIG_JULIA_SILENT_STANDBY_TIMEOUT_SECONDS` 控制的一次性计时器（默认 1800 秒）；期间没有唤醒词时投递 `EVT_SILENT_TIMEOUT`，由 S5 回到 S3。S3 和 S5 计时器在离开各自状态时立即取消。

默认 S3 背光呼吸范围为 5%–30%，周期 4000ms；S6 停止呼吸、将背光置为 0%，并让 ST77916 执行 `DISPOFF + SLPIN`。离开 S6 或运动唤醒显示时先执行 `SLPOUT + DISPON`，面板稳定等待约 120ms。这仍不等于芯片进入硬件深度睡眠。

### 墙钟调度

`julia_time.c` 从 PCF85063 恢复有效时间，在获得 IP 后进行 SNTP 同步并写回 RTC；默认时区 `CST-8`。

`julia_night_schedule.c` 每 5 秒检查有效墙钟：默认夜间为 23:00–07:00；22 点仍会投递现有 `EVT_BEDTIME`，但当前 FSM 不用它改变状态。满足夜间条件的 S1/S3/S5 先经过约 5 分钟宽限，活动对话会延后进入 S6。07:00 只恢复显示，行为状态仍停留在 S6，直到检测到唤醒词。时间无效时不执行这些时间判定。

### 运动输入

`julia_motion.c` 在非 S6 时不读取 QMI8658；进入 S6 后每 200ms 采样，加速度三轴差值门限为 0.80g、陀螺仪幅值门限为 120°/s，并要求连续 4 次（约 800ms）命中，触发后冷却 10 秒。运动只恢复显示，播放期间不触发；它不会把 S3/S5/S6 切换到 S4。

运动检测是短时活动判断，不是姿态解算、用户定位或有人／无人识别。

## 8. 显示验证

执行 [验收清单](VALIDATION.md) 中的开机、相位、嘴型、闲置、时钟和打断场景。重点观察颜色／字节序、整屏撕裂、相位不一致、音画偏移和断流后嘴型复位。

更换面板前应统一尺寸、像素格式、资源坐标、驱动时序和缓冲预算。单张 360×360 RGB565 全帧为 259200 字节；这只是像素数据，不包含 LVGL 对象、双绘制缓冲和其他资源。摄像头扩展需另行核算总线、GPIO、内存、带宽及电源需求。
