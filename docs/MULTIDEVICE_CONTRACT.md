# Julia 多设备协议核对（2026-09-10，待云端确认）

本文件保留阶段核对历史。`0.1.4` 的最终两端修正规则见 [control_protocol=1](MULTIDEVICE_CONTROL_V1.md)；32 项耗尽、逐轮隔离、执行回执和 busy 恢复已按新契约修正，不再沿用下文的阶段限制。真实双端验收按用户要求暂不执行。

本文件先于实现变更编写。依据为当前固件和 CLOUD_STATE_SYNC_V2_PROMPT.md；仓库不包含云端实现，不能将以下待定项表述为已与云端一致。未完成确认前不得切换正式环境或烧录在用设备。

## 已确定的不变量

- device_id 沿用 native_ota_get_device_id：eFuse 基础 MAC 生成 `esp-` 加 12 位小写十六进制。重启、重连不变；不另造会变化的 ID。
- session_id 保留每次 WSS 连接随机生成的 16 字节、32 位十六进制字符串规则。interaction_id 由云端业务轮次分配；两者均不是凭证。
- WSS Bearer 必须在云端映射到唯一 device_id，并核对 session_sync 中的 ID。填写 ID 本身不能授权。
- session_sync 增加 device_id，保留 protocol_version=2、session_id、state_revision、state、sub_state、wake_required、companion_remaining_ms、reason。device_state 使用同样的身份字段。FSM 仍是状态权威。
- 初始 ACK 仍必须匹配当前 session_id、初始 state_revision、protocol_version=2、accepted=true。ACK 之前禁止业务音频。现有 2 秒间隔、最多 3 次发送，最后等待 2 秒的期限不变。
- MQTT 目标主题为 `voice/{device_id}/vcmd` 和 `voice/{device_id}/vstatus`，不同时订阅旧全局语音主题。broker 必须按凭证限制订阅/发布 ACL；client_id 只用于连接标识。
- 控制封装包含 type、device_id、session_id、interaction_id、request_id 及业务字段。QoS PUBACK 不是执行 ACK，更不是播放完成。
- 上行保持 16kHz PCM1，下行保持 SPKS 指定的 16/24kHz 裸 PCM16。不增加音频帧头或流控。SPKE 只结束输入，由原播放任务排空软件缓冲及 DMA。

同步示例（全部为虚构值）：

```json
{"type":"session_sync","device_id":"esp-001122334455","protocol_version":2,"session_id":"0<redacted>9abcdef0<redacted>9abcdef","state_revision":12,"state":"S3","sub_state":"","wake_required":true,"companion_remaining_ms":0,"reason":"EVT_VOICE_SESSION_RESET"}
```

现有 ACK 示例（不擅自要求云端新增 ACK 字段）：

```json
{"type":"session_sync_ack","protocol_version":2,"session_id":"0<redacted>9abcdef0<redacted>9abcdef","state_revision":12,"accepted":true}
```

MQTT 意图示例（本次严格模式已实现；须使用实际当前标识）：

```json
{"type":"intent_result","device_id":"esp-001122334455","session_id":"0<redacted>9abcdef0<redacted>9abcdef","interaction_id":"turn-001","request_id":"control-001","intent":"goodnight"}
```

## 必须核对的差异与缺失

| 项目 | 修改前实现/契约 | 需要云端确认 |
| --- | --- | --- |
| 凭证 | 编译配置；MQTT 可匿名，WSS 有旧 token 回退 | 逐设备发放方式、token→ID 绑定、MQTT 用户/token/证书与 ACL、轮换方式 |
| MQTT | 固定共享主题，纯文本 MIC_START/MIC_STOP/FILE_SEND，intent_result JSON | 业务 type 名及字段、会话外命令 interaction_id 表达方式 |
| 轮次 | interaction_id 仅由 wake_detected 设置；连续对话 MIC_START 为裸文本 | 每次实际话语/打断如何声明新 interaction_id；如何保证 MQTT 与 WSS 跨通道排序 |
| 音频 | SPKS/PCM/SPKE 均无轮次 ID | 云端须在写入当前 socket 前验证连接和轮次并取消旧任务；设备无法从裸 PCM 判断迟到轮次 |
| ACK | 只有 WSS 状态同步/require_wake ACK；无通用控制执行回执 | ACK type、结果字段、accepted 与实际完成区分、重发超时 |
| 去重 | require_wake 最近 8 项；业务控制无 request_id 缓存 | 去重有效期/容量、缓存淘汰后防重机制、同 ID 不同载荷冲突码；不能声称有界缓存永久 exactly-once |
| 过期 | MQTT 无发送时间/期限 | 有效期字段和时钟基准；不能用收到后计时识别 broker 已保存很久的消息 |
| 同步拒绝 | accepted=false 等待原有同步超时 | auth_failed、busy、sync_timeout、session_replaced 的准确载体/错误码和 retry_after 规则 |

现有 require_wake 的 applied、interaction_active、state_unavailable、stale_state、sync_required 及缓存语义保留。不得将这些状态机错误擅自映射为认证错误。网络故障保留原重连与快速离线路径；HTTP 401/403 可明确识别为认证拒绝并降低重试频率。业务 busy/替换恢复策略待明确，不猜测字符串。

## 云端源码复核补充

已只读检查 `/opt/julia-multidevice-20260910` 工作区（基线 03c94b8，存在未提交修改，不是固定发布版本）。`device_identity.py` 已有 token→device_id 和 MQTT 账号 ACL 映射；`mqtt_adapter.py` 将纯文本封装成 `type=command, command=<原文本>`，并添加五个身份字段以及 connection_id/epoch。固件无需用 cloud epoch 替换自己的 generation。

发现新增兼容要求：严格云端 `state_sync.py` 要求 session_sync/device_state/require_wake_ack 同时有字符串 interaction_id 和合法 request_id。固件在多设备模式下给状态快照增加空 interaction_id（状态是会话范围）和稳定重传 request_id，保留全部 v2 状态字段；state_ready 添加同样的封装。ACK 仍回显原 request_id。关闭码已确认：4401 认证失败、4408 同步超时、4001 被新连接替换；云端 busy 消息为 `type=busy, code=voice_capacity, retry_after_ms=1000`。重试策略尚未成为双方确认的发布契约。

源码仍未定义通用控制执行 ACK、业务消息有效期、每次实际话语的 interaction_id 更新；仍不能宣称本稿所有待定项已解决。云端 generation 会取消旧引擎投递，但其 interaction_id 仍沿用唤醒 ID。云端 validation/multidevice.log 当时显示 10 项测试 OK；这是读取到的云端记录，不是本次固件端到端测试。

严格模式实际握手示例：

```json
{"type":"session_sync","interaction_id":"","request_id":"session_sync-12","device_id":"esp-001122334455","protocol_version":2,"session_id":"0<redacted>9abcdef0<redacted>9abcdef","state_revision":12,"state":"S3","sub_state":"","wake_required":true,"companion_remaining_ms":0,"reason":"EVT_VOICE_SESSION_RESET"}
```

严格 ACK 需要回显 device_id 和 request_id，并满足原 v2 条件。状态快照 request_id 为消息类型加原 state_revision，重发保持不变。require_wake_ack 保留原请求 ID，state_ready 使用 `ready-<本地连接generation>`；这些 ID 都限定在当前 session 内，不能作为物理设备身份。

## 本次实现与明确的阶段限制

- `CONFIG_JULIA_MULTI_DEVICE_ENABLE` 默认关闭，防止尚在修改的云端与在用设备被隐式切换。开启需 v2；只登记本设备 vcmd，且该订阅变为 critical。未同时订阅全局语音主题，OTA 主题和行为不变。
- 严格 WSS 只使用 `CONFIG_COMM_DEVICE_AUTH_TOKEN_VALUE`；严格 MQTT 拒绝 NONE，用户名/密码模式只使用显式设备字段，不取旧全局字段。认证模式仍支持已有 TOKEN/证书分支，云端已检查到的 broker 实现支持账号密码映射，证书对接未验证。所有模式的空/控制字符 WSS token 在 TLS 前拒绝。HTTP 401/403、WS 4401 最少等待 60 秒重试；普通网络重连保持原配置。
- MQTT `intent_result` 保留 normal/goodnight/dismiss；`type=command` 的 command 支持原 MIC_START、MIC_STOP、FILE_SEND 文本。实际执行在 WSS owner 中校验；FILE_SEND 是会话范围，interaction_id 可为空，其余要求匹配已知当前 interaction_id。JSON 上限 768 字节、嵌套最多 8 层，拒绝重复身份字段、嵌入 NUL 和尾随第二对象。
- 去重保存当前 WSS session 最近接纳的最多 32 个不同 request_id，不淘汰，重连清空。相同 ID 即使内容不同也不重复执行；未定义冲突回执。第 33 个新请求返回本地诊断 `dedup_capacity` 并拒绝；不会为了腾槽主动断线。**这是等待云端明确去重生命周期期间的保守实现，不支持单会话无限轮业务控制，不能作为正式长期运行版本。**
- 错误归属/格式/重复/容量不足只写脱敏诊断，不产生未经约定的执行 ACK。控制队列满或同步未完成时拒绝入队，不缓存断线业务。消息有效期没有可依赖的字段，尚不能过滤“同 session、同 interaction 下首次到达但已超期”的消息。
- 每个新 device_state 同时镜像到本设备 vstatus，带身份封装。镜像 QoS0、不保留、离线跳过；outbox 已达 4096 字节时跳过，防止语音状态无限积压。网络断开竞态可能留下已入队快照，消费者必须按 session/revision 过滤。WSS 状态 ACK 仍为权威，MQTT 镜像不代表控制执行成功。
- `busy/voice_capacity`、4408、4001 当前仍走原超时或断链恢复。尚未新增云端 retry_after 调度或会话替换停连策略；两个设备误配同一凭证时仍可能互相替换，不能用此版本宣称通过该项验收。
- 时间点有固定诊断槽；成功入上行 ring 的采音帧记录最后时间，SPKS/首 PCM 在接收入口取时，首成功 I2S 写入与排空完成由播放任务取时。WSS owner 输出日志。completed 在错误终止时也记录结束时间，须结合已有播放错误日志判断成功；取消不伪造完成。连续对话仍可能沿用 wake interaction_id，不能据此声称已经具有逐话语隔离。

## 诊断与资源边界

时间点使用 esp_timer_get_time 的启动后单调微秒：最后有效采音、SPKS 接收、首 PCM 接收、首个成功 I2S 写入、播放排空完成。首个 I2S 写入是软件可测代理，不是声学出声的硬件测量。关联 session_id、interaction_id 及 playback generation；未有 interaction_id 时明确缺失，不伪造。不得直接与服务器墙钟相减。实时采音/播放任务仅更新固定槽，日志和遥测由非实时消费者输出。

## 迁移与回退

保存当前已验证镜像及私有配置、NVS 备份；不在仓库存放真实配置或凭证。现有 ID 来源不迁移、不擦写 eFuse/NVS。多设备配置必须显式启用且凭证缺失时拒绝联网，不能回退共享身份。先验证云端身份绑定、定向主题和协议，再启用设备；回退到旧镜像只允许回到隔离的单设备环境，不得接入多设备服务。不要执行 erase-flash。

## 双设备验收（尚未执行）

1. 分别配置 A/B 的凭证；核对稳定 ID 不同，重启仍一致；交叉 token/ID 必须被服务器拒绝，MQTT 交叉主题由 ACL 拒绝。
2. 同时唤醒对话，捕获各自 session/interaction/request、主题与状态，确认声音及控制只到目标设备。
3. B 播放长回答时令 A 断网、重连、退出；确认 B 不停播，A 的旧回调/PCM/控制不重放。
4. A 打断后故意延迟旧轮次控制和服务端 TTS 回调；旧结果不得送入新轮次。A 晚安/拒绝只影响 A，提示排空后进入原 S6/S5。
5. 重投同 request_id，注入非本设备、旧 session、旧 interaction、过期和同 ID 不同内容消息，核对执行次数及回执。
6. 网络抖动/恢复、短尾和长回答，检查 SPKE 后缓冲与 DMA 正常排空，核对 16/24kHz。
7. 连续至少 200 轮，记录最小堆、任务数、缓冲高水位、队列/去重占用；应维持固定上限，不能以单次成功代替稳定性验收。
8. 模拟认证拒绝、同步 ACK 丢失、busy、session_replaced，按最终契约检查重试间隔与恢复，保存脱敏日志。
