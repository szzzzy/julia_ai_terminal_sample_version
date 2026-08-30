# 显示与交互

文档版本：V1.0。本文描述当前 ST77916／LVGL／Avatar 显示链路，不包含完整转场播放器或相位视频引擎。返回 [项目入口](../README.md)。

## 1. 显示链路

```text
app_main
  → julia_backlight_init
  → julia_display_init → ST77916 panel + lvgl_port
  → julia_avatar_init → 底图 + 眼睛／嘴型部件
  → julia_avatar_play_boot_sequence
  → julia_idle_display_init

语音／时钟／运动事件 → julia_fsm_runtime → Avatar 相位与背光
下行 PCM → julia_avatar_feed_pcm → Avatar 周期任务 → 嘴型
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

动画返回后才初始化语音、行为、情境和网络服务。因此“首屏可见”与“可以对话”是两个时间点，应分别测量。当前没有由应用接通的开机语音提示流程。

## 4. 对话呈现

| 相位 | 当前画面 | 眼睛／嘴型 |
| --- | --- | --- |
| IDLE | S1.1 基础立绘 | 随机眨眼，非说话时嘴型闭合 |
| LISTENING | 完整闭眼立绘 | 独立眼睛和嘴型层隐藏 |
| THINKING | S1.1 基础立绘 | 保持睁眼；不播放回答嘴型 |
| SPEAKING | S1.1 基础立绘 | 保持睁眼；由下行 PCM 能量驱动嘴型 |
| Dozing | 闭眼立绘覆盖 | 与背光策略组合，不等于芯片睡眠 |

相位选图以 `avatar_source_for_phase()` 为准。`main/ui/generated/clips/LISTEN.bin`、`THINK.bin`、`SPEAK.bin` 列入嵌入文件，但当前选图路径不调用其 RLE 解码器，不能把这些文件视为正在播放的相位动画。

`AVATAR_ENABLE_FULL_FRAME_MOTION=0`，整幅立绘的缩放呼吸和点头不执行。背光呼吸、眼睛局部动画和整幅画面微动是不同能力。

## 5. 嘴型

`voice_service_on_binary()` 在 PCM 写入扬声器成功后调用 `julia_avatar_feed_pcm()`。后者计算 RMS 并进行平滑及迟滞判断，得到四档嘴型目标。

- 上升门限：300、950、2300；下降门限：180、650、1650。
- Avatar 每 40ms 消费目标档位。
- PCM 保持时间为 180ms，超时或非 talking 状态时闭嘴。
- `SPKE`、会话结束和语音打断调用 talking stop，清除嘴型状态。

这里是按 PCM 能量同步的嘴型，不是音素／口型识别。驱动写入成功不等于声音已在物理扬声器上播放到同一时刻，音画偏移应上板测量。

## 6. FSM 映射

状态机定义在 [julia_fsm.h](../main/fsm/julia_fsm.h)，呈现由 [julia_fsm_runtime.c](../main/fsm/julia_fsm_runtime.c) 绑定。

| 行为状态 | 呈现 |
| --- | --- |
| S0.1 夜间休眠、S0.3 手动休眠 | SLEEP：闭眼与背光呼吸 |
| S0.2 日间离开、S2.3 睡前陪伴 | QUIET：闭眼，背光 100% |
| S1.2 远场待机 | FAR_STANDBY：闭眼与背光呼吸 |
| S3.3 用户呼唤 | LISTEN |
| S4.1 浅层对话、S4.2 深层对话、S4.4 打断处理 | THINK |
| S4.3 多轮对话 | SPEAK |
| 其余状态 | DEFAULT 基础立绘 |

这是当前呈现映射，不代表代码已具备深层情感识别或多轮理解算法。正常语音命令通过事件驱动这些状态，实际异常路径仍受转换规则约束。

## 7. 待机、夜间与运动

### 活动时间

`julia_idle_display.c` 每 500ms 检查活动时间。非 busy 且连续 300 秒无交互时，闭眼、启动背光呼吸并投递 `EVT_USER_LEAVE`，正常待机进入 S1.2。听音／思考／说话期间 busy 为真，普通闲置逻辑不降档。

默认背光呼吸范围为 5%–100%，周期 4000ms。该亮度范围不等于已经达到待机功耗目标。

### 墙钟调度

`julia_time.c` 从 PCF85063 恢复有效时间，在获得 IP 后进行 SNTP 同步并写回 RTC；默认时区 `CST-8`。

`julia_night_schedule.c` 每 5 秒检查有效墙钟：默认夜间为 23:00–07:00；22 点有睡前陪伴分支。满足条件的空闲状态先经过约 5 分钟宽限，活动对话会延后夜间进入。白天只释放调度持有的夜间休眠，不自动释放手动休眠。时间无效时不执行这些时间判定。

### 运动输入

`julia_motion.c` 每 100ms 读取 QMI8658；加速度差门限 0.20g、陀螺仪幅值门限 25°/s，连续 4 次命中后触发，冷却 3 秒。适用夜间休眠、日间离开与远场待机，播放时不触发运动唤醒。

运动检测是短时活动判断，不是姿态解算、用户定位或有人／无人识别。

## 8. 显示验证

执行 [验收清单](VALIDATION.md) 中的开机、相位、嘴型、闲置、时钟和打断场景。重点观察颜色／字节序、整屏撕裂、相位不一致、音画偏移和断流后嘴型复位。

更换面板前应统一尺寸、像素格式、资源坐标、驱动时序和缓冲预算。单张 360×360 RGB565 全帧为 259200 字节；这只是像素数据，不包含 LVGL 对象、双绘制缓冲和其他资源。摄像头扩展需另行核算总线、GPIO、内存、带宽及电源需求。
