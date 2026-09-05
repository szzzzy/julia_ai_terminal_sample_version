# 工程边界与已知限制

文档版本：V1.1。实现核对日期：2026-09-04。以下结论以当前工作区源代码、构建清单和生效配置为依据。它们描述代码可确认的边界及需要验证的风险，不代表全部问题均已在设备上复现。返回 [项目入口](../README.md)。

## 1. 运行链路限制

| 编号 | 范围 | 事实与影响 | 验证／处理方向 |
| --- | --- | --- | --- |
| VOICE-01 | 对话超时 | `MIC_STOP` 保持 busy 并进入思考，没有回答业务超时；连接保活不能让业务自动结束 | 模拟服务器继续心跳但不返回回答，定义可取消／可退出的超时策略 |
| VOICE-02 | 播放缓冲 | 独立播放任务使用 64KiB PSRAM；服务端长期快于播放速率会触发显式溢出中止 | 服务端节流；上板验证高水位、尾音和实际打断延迟 |
| VOICE-03 | 播放断流 | 欠载后可重新预缓冲；无待播数据且距最后输入／开播达到 15 秒时中止 | 主机模拟覆盖 1 秒间隔；真实网络与 I2S 时序仍需上板 |
| VOICE-04 | 队列压力 | MIC 使用 256 帧（约 5.12 秒）PSRAM ring，正常1帧／积压最多8帧且受8ms预算约束；ring 满明确终止整轮 WSS，不静默继续残缺 ASR | 验证弱网、追赶耗时、命令突发、owner teardown 与新连接无旧 PCM |
| VOICE-05 | 打断关联 | 本地代次隔离已取消的 PCM 和完成事件，但网络消息无 session_id／turn_id | 服务器仍需停止旧回答；新播放期间迟到的旧命令不能可靠辨别 |
| VOICE-06 | 命令确认 | MQTT 仅支持 MIC_START、MIC_STOP、FILE_SEND，没有 vstatus 应用回执 | 服务器不能把 PUBACK 当作执行成功；定义实际回执再对接 |
| FILE-01 | 文件与语音 | 文件按块推进、读取失败关闭会话；语音启动可发 file_cancelled 结束文件区间 | 服务端需处理取消并丢弃部分文件；SD 底层 I/O 时延仍需测试 |
| FILE-02 | SD 生命周期 | `sd_card_start()` 只尝试挂载，没有后台重试／拔卡检测；文件服务的 SD 锁是弱默认实现 | 验证无卡、失败挂载和读取中断；建立共享访问和卡状态管理 |
| HW-01 | 共享 I2C／IMU | TCA9554 初始化失败清理已修复；QMI8658 为每轴 ±64dps，而默认 120dps 向量门限高于三轴满量程约 111dps | 验证低内存恢复；陀螺仪量程或门限仍需上板标定 |
| DISPLAY-01 | 显示驱动契约 | `julia_display_set_backlight()` 只有声明；ST77916 `swap_xy` 的 QSPI 路径绕过命令封装并忽略错误；panel 开关错误现在保留为待重试状态 | 新代码使用 `julia_backlight`；修复 QSPI 命令与错误传播后再开放对应 API |

源码定位：[voice_service.c](../main/voice/voice_service.c)、[wss_transport.c](../main/voice/wss_transport.c)、[board_audio.c](../components/julia_board_audio/board_audio.c)、[sd_card.c](../main/storage/sd_card.c)、[tca9554.c](../main/hardware/tca9554.c)、[qmi8658_shared.c](../main/hardware/qmi8658_shared.c)、[esp_lcd_st77916.c](../main/display/esp_lcd_st77916.c)。

## 2. OTA 与发布

### Range 恢复

OTA 和音频下载现通过 HTTP_EVENT_ON_HEADER 收集响应头，严格校验 Content-Range／ETag，已移除对不存在的 SDK 开关/取头 API 的依赖。合法 206 不再因客户端缺少该能力被隔离。

完整 200 下载与恢复下载必须分别验收；服务器忽略 Range 后的从零下载，也不能记作 Range 恢复通过。源码定位：[ota_engine.c](../main/ota/ota_engine.c)、[http_downloader.c](../main/network/http_downloader.c)。

### 提交与启动验收

OTA 下载已经通过 FSM 确认准入，S2/S4/S5/S6 的清单以已有 deferred 状态结束本次处理；任务退出（包括链路错误）回到 S3。新镜像只有在关键显示、音频、语音和 FSM 初始化成功后才确认。电源提交钩子与额外产品自检钩子仍为预留接口，本轮未新增电量阈值或在线服务健康标准。

准入名称应与工程名 `julia_fused_base` 一致。镜像名、产品 ID、硬件版本、应用版本和安全版本分别校验，不能混用。源码定位：[ota_stability.c](../main/ota/ota_stability.c)、[ota_boot_health.c](../main/ota/ota_boot_health.c)、[main.c](../main/app/main.c)。

### 上报边界

关键报告队列容量有界，队满时允许牺牲部分中间状态；终态／重启事件可替换最早记录。服务器应容忍重复与中间状态缺失。

`STORAGE_UNAVAILABLE` 错误枚举未映射同名字符串，会返回 `UNKNOWN`；提交时堆空间不足使用 `PRECONDITION_LOW_POWER`，不能据此推断电池电量。源码定位：[ota_report.c](../main/ota/ota_report.c)、[ota_stability.c](../main/ota/ota_stability.c)。

## 3. 安全、隐私与功耗

当前开发配置与行为：

- MQTT 使用 `mqtt://`，设备认证方式为 NONE；共享语音主题没有自动设备后缀。
- WSS 使用 CA，但 `skip_common_name=true`；服务器名称校验尚未作为生产约束落实。
- OTA URL 主机允许列表为空时放行；未启用安全启动、Flash 加密及强制签名镜像。
- 默认服务器唤醒会持续上传 MIC；MICS 在该模式下被忽略，MIC_STOP 不关闭上传。
- 屏幕休眠只有显示策略，不等于 MIC、I2S、功放、Wi-Fi 和 CPU 的联合节能。
- Wi-Fi 使用 `WIFI_PS_NONE`；当前无按行为状态实施的完整电源管理闭环。

持续 16kHz 单声道 PCM16 的裸数据速率为 32000 字节／秒，即 256kbps；全天连续上传约 2.7648GB，不包含协议开销。这是格式推算，不是网络流量或功耗实测。

生产部署需要独立设备身份与权限、凭据注入／轮换、服务器身份校验、用户可理解的采音状态和数据删除策略。真实私钥、token、网络密码和用户语音不应写入文档或普通发布记录。

## 4. 能力接入边界

- **音频素材：** 服务和引擎参与编译，但检查请求无调度调用，MQTT 响应无注册入口，下载完成播放为弱默认钩子。
- **记忆／例行：** `julia_memory.c`、`julia_routine.c` 不参与当前编译；不能将参考函数视为已工作的用户画像或长期记忆。
- **情感／对话理解：** FSM 有对应事件名称，但没有完整感知输入；服务端算法不在仓库内。
- **LED：** LED 源码参与编译，应用没有调用对应初始化，不应承诺外部 LED 与 FSM 联动。
- **定位／摄像头／App：** 没有当前可用的实现链路或设备协议；S6 的 IMU 运动诊断不是定位，也不再直接唤醒显示。
- **显示动画：** 当前使用静态底图和局部眼／嘴动作；全帧微动关闭，完整转场引擎未接入。

未参与构建的源码清单见 [源码组织](../main/README.md)，不能把参考文件中的缺陷等同于当前运行故障。

## 5. 工程与测量

根 CMake 含本机工具路径；辅助编译数据库脚本依赖另一工程。跨机器构建应使用本工程实际 CMake 产物，并记录 SDK、工具链及配置。

编辑器以 `build/compile_commands.json` 为固件索引，配置头来自 `build/config/sdkconfig.h`。只在独立目录构建不会自动为 `build/` 生成这些文件；`.vscode/` 也不随 Git 分发。目录不一致时优先按 [构建与发布](BUILD_AND_RELEASE.md) 重新配置，不能把所有头文件目录或主机测试 stub 加入全局搜索路径掩盖问题。

播放初始化需要分配 64KiB PSRAM 并创建任务；资源不足会使 voice_ready 为假，WSS 不放行。该状态与服务器不可达应分别诊断。启动门槛保护初始化顺序，不是完整的产品自检，也不提供硬件故障后的自动重建流程。

项目有启动阶段耗时、播放缓冲高水位、溢出及 MIC 入队失败计数，仍缺少统一的唤醒命中率、端到端首音延迟和状态功耗实测记录。没有硬件测量记录时，不声明续航、远场唤醒率、实际帧率或最大并发量。

优先处理顺序：业务回答超时、播放与启动实机验收、OTA 恢复／启动验收和设备安全；随后建立功耗与性能基线，再接入记忆、配置管理及硬件扩展。具体验收记录方式见 [验证与验收](VALIDATION.md)。
