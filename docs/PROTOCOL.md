# 设备通信协议

文档版本：V1.0。本文描述当前设备实现，供服务器联调使用。身份与版本基线见 [构建与发布](BUILD_AND_RELEASE.md)，限制与验证分别见 [工程边界](COMMENT_AUDIT_FINDINGS.md) 和 [验收清单](VALIDATION.md)。

实现核对日期：2026-08-31。语音传输、音频播放和行为状态分别有自己的所有者；服务器不能以 TCP 写入成功或 MQTT PUBACK 代替设备播放完成。

## 1. 通道与职责

| 通道 | 设备侧用途 | 当前连接方式 |
| --- | --- | --- |
| MQTT | OTA 检查、响应、通知、状态，以及三类语音作业命令 | `CONFIG_COMM_MQTT_BROKER_URI`；开发配置为 `mqtt://`，不加密 |
| WSS | MIC PCM1 上行、PCM 下行、语音控制和 WAV 文件外发 | TLS + WebSocket，Bearer 认证头 |
| HTTPS | 下载 OTA 镜像 | 清单中的 `url`，内嵌根证书 |

MQTT 与 WSS 各自配置地址，不存在统一的 `JULIA_SERVER_ADDR` 配置项。MQTT 语音命令集小于 WSS，二者不能互相替代。

## 2. WSS 建连与消息

地址由 `CONFIG_WSS_SERVER_HOST`、`CONFIG_WSS_SERVER_PORT`、`CONFIG_WSS_PATH` 组成，端口默认 9443，路径默认 `/voice`。

设备发送 `Authorization: Bearer <token>`，token 优先取 `CONFIG_COMM_DEVICE_AUTH_TOKEN_VALUE`，为空时取 `CONFIG_WSS_TOKEN`。该选择独立于 MQTT 的认证模式；空 token 不在本地拒绝，服务器应负责拒绝无效认证。

WSS 使用 `server_certs/ca_cert.pem` 验证证书链；当前传输实现设置 `skip_common_name=true`，跳过服务器名称校验。此行为属于开发安全限制，不应描述为完整服务器身份校验。

| 项目 | 实现约束 |
| --- | --- |
| 文本／二进制 opcode | `0x1`／`0x2` |
| 单帧载荷 | 最多 1200 字节 |
| 分片消息 | 支持 continuation 重组；完整消息合计也最多 1200 字节 |
| 控制帧 | 不得分片，载荷最多 125 字节 |
| 服务端帧 | 不带掩码，不使用未协商的扩展位 |
| 文本编码 | UTF-8；每条文本消息只放一个命令，建议不带行尾换行 |
| 保活 | 空闲接收时检查 PING；默认间隔 15 秒 |
| 断链判定 | 发出 PING 后默认 10 秒未收到下行帧；任意合法下行帧均可清除等待状态，不限 PONG |
| 重连 | 默认等待 5 秒后再次连接 |

连接与 socket 读写仍由单一会话任务负责；播放经缓冲交给独立任务，文件每轮推进一个块。每轮控制和 MIC 队列分别最多处理 4 条；socket 写等待配置为 500ms。SD 文件打开／读取和 TLS 自身仍可能耗时，不能将这些设置解释为整条命令链路的硬实时保证。TCP／TLS 在线不表示 ASR 或回答生成仍正常。

## 3. MIC 上行与对话语义

### 3.1 唤醒模式

`CONFIG_JULIA_SERVER_WAKE_ENABLE=y` 为默认模式：WSS 会话建立后立即打开持续上传，界面保持待机。服务器检测唤醒或确认新的用户话语后发送 `MIC_START`。

关闭该开关时，设备编译本地 WakeNet，模型名称为 `wn9_nihaoxiaozhi_tts`、显示唤醒词为“你好小智”。本地命中后请求打开上传；该动作不是向服务器发送一条 `MIC_START` 文本。模型是否已正确烧入 `model` 分区必须单独验证。两种模式是编译选择，没有自动断网切换。

语义边界：

- `MIC_START`：确认进入听音，必要时打开上传，并停止当前扬声器播放。
- `MIC_STOP`：确认当前话语结束，进入思考；保留音频上传。
- `SPKE`：标记音频输入结束，排空已接收的 PCM 和 DMA 尾音后回到待机；默认服务器唤醒模式继续上传。
- 本地唤醒模式在播放完成后启动陪伴上传计时，默认 300 秒无后续对话时停止上传。
- `MIC_STOP`、闭眼表情、夜间状态均不是隐私静音命令。
- 会话结束时关闭上传、停止播放并清除监听／忙碌状态；新的会话按所选唤醒模式启动。

### 3.2 PCM1 格式

每条上行 binary 消息包含 16 字节头和 PCM 正文。常规采集块为 320 个采样点，即 640 字节 PCM、656 字节完整消息，周期约 20ms。服务器以头部 `bytes` 校验实际载荷，不应仅凭固定长度解析。

| 字节偏移 | 类型 | 内容 |
| --- | --- | --- |
| 0–3 | 4 字节 ASCII | `PCM1` |
| 4–7 | uint32，小端 | `seq`，MIC 任务的发送序号 |
| 8–9 | uint16，小端 | PCM 正文字节数，常规为 640 |
| 10–11 | int16，小端 | dBFS × 100，例如 −6000 表示 −60dBFS |
| 12–14 | 3 字节 | 保留，当前为 0 |
| 15 | uint8 | PCM 正文字节和模 256 |
| 16 起 | int16，小端数组 | 单声道、16kHz、PCM16 |

序号在 MIC 任务内递增，不在每次 `MIC_START` 或 WSS 重连时清零；考虑 uint32 回绕。队列满时可能产生序号缺口，没有音频重传。校验和只用于载荷一致性检查，不是密码学完整性机制。

PCM1 不包含会话编号、话语编号或采样时间戳。服务器应把连接状态、控制命令和帧序号结合使用，不能把序号当作对话轮次。

## 4. WSS 下行命令与 PCM

| 命令 | 设备行为 |
| --- | --- |
| `MIC_START` | 清空待播 PCM、使旧播放代次失效并进入听音；播放任务在当前小块写入返回后停止 I2S |
| `MIC_STOP` | 结束当前监听并进入思考；没有活动话语时忽略；不关闭 streaming |
| `SPKS <rate>` | 配置扬声器并开播；正常对话中发送 FSM 事件进入 SPEAK；联调使用 16000 或 24000 |
| `SPKV <n>` | 设置音量，合法整数范围 0–100；越界值被拒绝，文本入口不做钳位 |
| `SPKE` | 有序结束：先排空待播数据，再停播／闭嘴／回待机；不存在活动播放代次时忽略 |
| `SPKT` | 独立播放任务生成 440／660／880Hz 三音，可被 MIC_START 或断链取消 |
| `MICS <bg>` | 仅本地唤醒配置生效；背景值范围 −10000–0，启用门限触发上传；服务器唤醒模式明确忽略 |
| `MICW` | 退出门限触发模式；不单独改变 streaming 开关或发起新一轮听音 |
| `FILE_SEND <uri>` | 启动分块文件外发；监听、播放或另一文件传输期间拒绝，格式见第 5 节 |

命令区分大小写。参数发送端应使用完整十进制整数；不要依赖 `strtol` 对尾随内容的宽松解析。`SPKS` 只接受 16000 或 24000，且数字之后不能有尾随内容。

下行 binary 是不带 PCM1 头的单声道、小端 PCM16，非空、长度为偶数、完整消息最多 1200 字节。采样率与前面的 `SPKS` 一致。

正常对话顺序：

```text
WSS 建立       设备 → 服务器：连续 PCM1（默认模式）
唤醒确认       服务器 → 设备：MIC_START
用户话语结束   服务器 → 设备：MIC_STOP
回答开播       服务器 → 设备：SPKS 24000
回答音频       服务器 → 设备：binary PCM × N
回答结束       服务器 → 设备：SPKE
待机           设备 → 服务器：继续 PCM1（默认模式）
```

不要把裸 `SPKS` 当作任意 FSM 状态下的完整对话启动命令。播放期间服务端确认用户插话时可发送 `MIC_START`，同时应停止发送旧回答。设备用本地播放代次隔离取消后的缓冲及完成事件，未开新播放时迟到的 SPKE 不结束监听。但网络消息本身没有轮次编号，迟到旧 SPKS 或新播放期间迟到旧 SPKE 仍需服务器避免。

播放任务使用 64KiB PSRAM 缓冲：启动／欠载后以 80ms 音频量为预缓冲目标，从首个缓冲数据开始最多等待 120ms；SPKE 可立即放行不足目标的短尾段。播放小块为 160 样本，I2S 写入使用 50ms 等待参数并校验短写。750ms 断流不会自动关播；在无待播数据时，距最近输入／开播达到 15 秒则报告播放超时。

服务端应按播放速率推流，64KiB 约对应 24kHz 的 1.37 秒或 16kHz 的 2.05 秒 PCM。缓冲满时明确中止该次播放并返回 `ERROR playback_overflow`，不静默截断；超时／驱动失败分别返回 `ERROR playback_timeout`／`ERROR playback_failed`。未开播或 SPKE 后继续到来的 PCM 不被接受。

### 4.1 接收、播放和完成的区别

`SPKS` 被接受后，工作任务才实际配置 I2S；设备不发送“硬件已开播”的应用回执。输入 PCM 入缓冲也不表示已出声。`SPKE` 只结束输入，设备在排空软件缓冲与尾音后才处理完成事件、关闭嘴型并清除 busy。

| 返回文本 | 含义与服务器处理 |
| --- | --- |
| `ERROR playback_overflow` | 未消费 PCM 超出固定缓冲；该次播放中止，应停止继续发送并调整推流速度 |
| `ERROR playback_timeout` | 播放等待超时，包括空缓冲缺少输入或底层 I2S 返回超时；需结合设备日志定位 |
| `ERROR playback_failed` | 其他扬声器初始化／写入错误；当前播放中止，不继续沿用该次 SPKS |
| `ERROR file_busy` | 文件请求与监听、播放或既有传输冲突；该文件请求未开始 |
| `ERROR file_cancelled` | 文件外发被语音启动终止；丢弃部分文件，退出文件接收状态 |

错误文本只能在会话仍可写时发送；网络故障时可能只发生断链。服务器应同时处理错误文本与连接关闭，不等待不存在的统一成功回执。

## 5. WAV 文件外发

URI 前缀区分大小写：`SD:/x.wav` 对应 `/sdcard/x.wav`，`SPIFFS:/x.wav` 对应 `/spiffs/x.wav`。映射支持不表示相应文件系统已挂载；当前应用只接通 SD 挂载。路径检查拒绝 `.`／`..` 路径段，扩展名仅允许 `.wav`，单文件最多 8MiB。

```text
服务器 → 设备：FILE_SEND SD:/sample.wav
设备 → 服务器：BEGIN FILE <size> <name>     文本
设备 → 服务器：文件字节 × N                binary，每帧最多 1200 字节
设备 → 服务器：END <bytes>                 文本
```

`BEGIN FILE` 与 `END` 之间的 binary 属于文件传输，不按 PCM1 解析；每轮最多发送一个 1200 字节块，期间暂不发送 MIC PCM1。控制、下行与保活仍有执行机会。MIC_START、成功的 SPKS 或 SPKT 可终止文件并返回 `ERROR file_cancelled`，随后恢复语音上行；服务器应丢弃该不完整文件并退出文件接收状态。

前置拒绝可返回：`ERROR bad_uri`、`ERROR bad_extension`、`ERROR file_open_failed`、`ERROR sd_busy`、`ERROR file_size_failed`、`ERROR file_name_too_long`、`ERROR file_busy`。

仅在完整读取且全部写出后发送 END。本地读取失败、实际长度不符或网络写入失败会关闭文件并标记会话故障；连接结束也释放文件。服务器仍需接收超时，缺少正确 END 或收到 file_cancelled 时丢弃文件。

## 6. MQTT 控制面

### 6.1 主题

设备 ID 从 eFuse 基础 MAC 生成，格式为 `esp-` 加 12 位小写十六进制字符。

| 方向 | 默认主题 | 语义 |
| --- | --- | --- |
| 设备 → 服务器 | `/device/ota/check` | OTA 检查，QoS 1，非保留 |
| 服务器 → 设备 | `/device/ota/response/<device_id>` | OTA 响应，订阅 QoS 1 |
| 服务器 → 设备 | `/device/ota/notify/<device_id>` | 检查通知，订阅 QoS 1 |
| 设备 → 服务器 | `/device/ota/status/<device_id>` | OTA 生命周期／进度，QoS 1 |
| 服务器 → 设备 | `voice/esp32s3/vcmd` | 语音作业，订阅 QoS 1，非 critical |

语音主题是配置中的完整字符串，不自动拼接 device_id。多设备使用同一主题会收到相同命令，需在部署与权限设计中隔离。

### 6.2 语音命令

每条 MQTT 消息只包含以下一种命令：

```text
MIC_START
MIC_STOP
FILE_SEND SD:/sample.wav
```

当前处理器只支持这三类，允许末尾空白和换行，不支持一条消息中的多行命令列表。注册载荷上限为 128 字节，FILE_SEND URI 缓冲区含 NUL 共 128 字节。

这些命令进入独立 4 槽控制队列，不与 MIC 的 8 槽队列争用容量，仍须由 WSS 会话执行；会话未就绪或队列已满时拒绝，连接边界清理队列，不重放断链期间的命令。当前没有 `vstatus` 发布，也没有 `mic_started`／`mic_stopped` 等应用层回执。MQTT PUBACK 不表示命令已执行。

## 7. OTA 检查、清单与通知

### 7.1 设备检查请求

MQTT 连接并收到 critical 主题的 SUBACK 后执行检查；默认周期为 21600 秒，附加 0–1800 秒抖动。默认响应等待 15 秒，后续重试与恢复由配置控制。

```json
{
  "type": "ota_check",
  "request_id": "12ab34cd",
  "device_id": "esp-001122334455",
  "product": "julia-ai-device",
  "hardware_version": "1.0",
  "current_version": "0.1.0"
}
```

`request_id` 由设备生成，当前为 8 位十六进制。服务器回显最近一次请求；较早响应可能被后续请求覆盖后判为过期。

### 7.2 无需升级

即使 `update=false`，身份与请求关联字段仍必填：

```json
{
  "type": "ota_check_response",
  "update": false,
  "request_id": "12ab34cd",
  "device_id": "esp-001122334455",
  "product": "julia-ai-device",
  "hardware_version": "1.0"
}
```

### 7.3 升级清单

以下为字段示例，不是可直接发布的清单；版本、大小、摘要、URL、时间和安全版本必须由真实产物生成。

```json
{
  "type": "ota_check_response",
  "update": true,
  "request_id": "12ab34cd",
  "device_id": "esp-001122334455",
  "product": "julia-ai-device",
  "hardware_version": "1.0",
  "job_id": "job-example-001",
  "artifact_id": "julia-example-0.1.1",
  "version": "0.1.1",
  "url": "https://firmware.example.com/julia/0.1.1/app.bin",
  "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "image_size": 1940000,
  "security_version": 0,
  "expires_at": 1893456000,
  "force_update": false
}
```

`job_id` 和 `force_update` 可省略；其他所示字段在 `update=true` 时必填。解析器同时接受 `type="ota"`，对接统一使用 `ota_check_response`。

主要校验：

- JSON 不超过 1024 字节；字段类型正确，不隐式转换数字字符串。
- request_id、device_id、product、hardware_version 与设备／最近请求匹配。
- `artifact_id` 非空且最多 95 字节，版本最多 31 字节，URL 最多 255 字节。
- `version` 为三段数字，并与镜像内版本一致；默认要求高于当前版本。
- `url` 使用 HTTPS；非空主机允许列表按名称精确匹配。当前允许列表为空时放行，不自动绑定 WSS 主机。
- `sha256` 恰好 64 个十六进制字符，`image_size` 为正 uint32 且不能超过目标分区。
- `security_version` 为非负 uint32，镜像内 secure_version 不低于它。
- `expires_at` 为非负 Unix 秒整数；设备时间大于 0 时，必须晚于设备当前时间。
- 镜像 `project_name` 必须为 `julia_fused_base`；它不同于 `product=julia-ai-device`。

`force_update=true` 只放宽目标版本高低判断，不关闭其他校验。

### 7.4 检查通知

```json
{
  "type": "ota_notify",
  "schema_version": 1,
  "job_id": "job-example-001"
}
```

`schema_version` 必须是数值 1，`job_id` 可省略。通知最多 512 字节，默认节流 5 秒；通过校验后唤醒检查任务，不直接执行下载。

## 8. HTTPS 下载与状态

### 8.1 下载行为

首次完整下载要求 HTTP 200；有正 Content-Length 时必须与清单长度一致，无 Content-Length 时仍按清单大小、完整响应和摘要校验。

存在恢复偏移时发送 `Range: bytes=<offset>-`。服务器返回 200 或 416 时，设备可放弃断点从零下载。206 的恢复路径依赖 ETag／Content-Range 处理。

**当前构建限制：** 生效配置没有 `CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS`，ESP-IDF 5.5.4 对应头文件／Kconfig 也未提供代码引用的开关／取头接口，206 分支会进入 `RANGE_MISMATCH`。当前不能向服务端承诺已具备可用的 Range 续传；保留断点数据不等于恢复路径已经验收。

下载后检查镜像头、芯片、项目名、版本、安全版本、SHA-256 与镜像有效性；只有满足提交条件才设置启动分区。双 OTA 分区、启动确认和回滚流程与网络是否在线分开处理。

### 8.2 状态示例

```json
{
  "type": "ota_status",
  "schema_version": 1,
  "event_id": "0123456789abcdef0123456789abcdef",
  "device_id": "esp-001122334455",
  "request_id": "12ab34cd",
  "artifact_id": "julia-example-0.1.1",
  "product": "julia-ai-device",
  "hardware_version": "1.0",
  "current_version": "0.1.0",
  "target_version": "0.1.1",
  "state": "accepted",
  "attempt": 1,
  "error_code": "NONE",
  "uptime_ms": 123456,
  "job_id": "job-example-001"
}
```

状态值：`accepted`、`downloading`、`verifying`、`rebooting`、`booted_pending_verify`、`succeeded`、`failed`、`rolled_back`、`deferred`。

下载状态可带 `bytes_downloaded`、`image_size`、`progress_percent`；默认按 5% 进度步长或 10 秒间隔节流。关键事件正常情况下先持久化到 NVS，收到对应 PUBACK 后删除；进度为尽力发送。

关键队列容量有界：队满时部分中间状态不再持久化，终态／重启事件可挤出最早记录。因此服务器应按 event_id 去重，并容忍重复及缺失的中间状态，不要求每次看到完整状态序列。

当前错误名称包括：`NONE`、`PRECONDITION_LOW_POWER`、`NETWORK_TIMEOUT`、`TLS_VERIFY_FAILED`、`HTTP_STATUS_INVALID`、`RANGE_MISMATCH`、`IMAGE_TOO_LARGE`、`IMAGE_HEADER_INVALID`、`HASH_MISMATCH`、`IMAGE_VALIDATE_FAILED`、`BOOT_SELF_TEST_FAILED`、`ROLLBACK_UNAVAILABLE`、`MANIFEST_INVALID`、`ARTIFACT_QUARANTINED`、`BOOT_PARTITION_SET_FAILED`、`NVS_WRITE_FAILED`、`UNKNOWN`。

`STORAGE_UNAVAILABLE` 枚举尚未映射为同名字符串，当前落为 `UNKNOWN`。`PRECONDITION_LOW_POWER` 也可能表示提交时空闲堆不足，不是可靠的电池电量测量结果。

## 9. 未接通的协议范围

音频素材模块有 `audio_check`／`audio_check_response` 结构和下载入口，但当前没有检查请求调度，也未注册 MQTT 响应分发；`native_audio_on_ready()` 仍为弱默认钩子。不能把向某个音频主题发布清单视为设备可执行的功能。

服务器端的 ASR／LLM／TTS 内部 API、情感标签、用户记忆、App 配置、设备会话轮次和命令应用回执，不属于本版已实现的设备协议。
