# 云端修改提示词：Julia 会话与状态同步协议 v2

以下内容可直接交给云端代码助手执行。对应固件已实现，云端请按此契约修改，不要仅参考旧的示意消息。

---

请修改当前 Julia 语音服务器，为 ESP32 设备实现 WSS 状态同步协议 v2，完成代码、测试和部署说明。保留已有认证、语音识别、推理、TTS、MQTT、OTA 功能。先检查现有代码中的会话生命周期和免唤醒计时逻辑，然后完成以下改造。

## 1. 目标与职责

- 设备 FSM 是设备行为状态的唯一权威。云端通过状态快照跟随设备，通过明确请求提前结束陪伴，不能自己静默改变状态并假定设备已经同步。
- 新 WSS 连接必须重建会话。旧任务、旧轮次、旧播放、旧唤醒资格、旧回调不能向新连接投递结果。
- 所有新增消息使用同一条已认证的 WSS 连接上的 JSON 文本帧。状态同步不要转发到 MQTT。
- MQTT/WSS 连通与行为状态分开处理。S3/S5/S6 是等待唤醒，不等于设备掉线。

## 2. 初始握手

设备在 WSS 认证成功后主动发送，初始状态可能是 S3/S5/S6/S7/S8，不要假定总是 S3：

```json
{"type":"session_sync","protocol_version":2,"session_id":"0123456789abcdef0123456789abcdef","state_revision":12,"state":"S3","sub_state":"","wake_required":true,"companion_remaining_ms":0,"reason":"EVT_VOICE_SESSION_RESET"}
```

云端在当前已认证连接上绑定 `session_id`，清理旧业务上下文，应用这个状态，然后原样回显初始版本号：

```json
{"type":"session_sync_ack","protocol_version":2,"session_id":"0123456789abcdef0123456789abcdef","state_revision":12,"accepted":true}
```

- ACK 中 `protocol_version=2`、`state_revision` 和布尔值 `accepted=true` 都是必需的。
- ACK 发出前不得发 MIC_START/MIC_STOP/SPKS/SPKE、唤醒确认或音频。设备也不会在收到有效 ACK 前上传 PCM。
- 因此服务器接收循环必须能在没有任何 PCM 到达时先处理 JSON 握手，不能等待首包音频才启动接收/认证后的逻辑。
- 同一连接上重复的 `session_sync` 是重传：回相同 ACK，不重复清理已经建立的业务上下文。新的物理连接即使还属于同一设备，也要独立处理。
- 会话 ID 是每次连接随机生成的 32 个十六进制字符；它只用于关联，不能代替原有设备认证。

## 3. 状态快照与确认

握手完成后，设备会再发一次最新快照，并在 FSM 状态改变时发送：

```json
{"type":"device_state","protocol_version":2,"session_id":"0123456789abcdef0123456789abcdef","state_revision":18,"state":"S1","sub_state":"","wake_required":false,"companion_remaining_ms":300000,"reason":"EVT_SILENCE_TIMEOUT"}
```

云端应用后回复：

```json
{"type":"device_state_ack","session_id":"0123456789abcdef0123456789abcdef","state_revision":18}
```

字段规则：

- `state`：`S0`～`S8`，只含主状态。
- `sub_state`：S2 为 `S2.1`/`S2.2`/`S2.3`，S7 为 `S7.1`/`S7.2`，其余为空字符串。
- `state_revision`：设备启动期间递增的 uint32。每次连接可能从任意非零版本开始；仅在同一 `session_id` 内比较，不要要求从 1 开始。
- `reason`：诊断事件名，不用它替代 `state` 判断。
- `companion_remaining_ms`：S1 在生成快照时的剩余时间，其他状态为 0。重传时数值不变，不能因重传重复重置云端计时。
- 设备允许合并尚未发送的中间状态；版本号不一定连续。收到更高版本时，以完整快照收敛，不要求先收到全部历史版本。
- 相同版本是重传：幂等应用并再 ACK。低版本不能覆盖高版本，但仍可 ACK 其版本以结束对方重传。ACK 必须回显收到的版本，不要伪造未来版本。
- 初始握手后可能再收到同版本的 `device_state`，仍须按上述规则 ACK。

状态对应云端行为：

| 设备状态 | 云端行为 |
|---|---|
| S1 | 陪伴免唤醒窗口，可通过 MIC_START 发起下一轮 |
| S2.1 | 当前用户话语进行中，结束时发送 MIC_STOP |
| S2.2 | 等待当前轮回答，回答使用 SPKS/PCM/SPKE |
| S2.3 | 当前回答播放中 |
| S3/S5/S6 | 仅做唤醒检测，确认后先发送 wake_detected |
| S4 | 唤醒交互阶段，遵循 state_ready 握手与 MIC_START/MIC_STOP |
| S0/S7/S8 | 暂停新的业务交互；仍收取并 ACK 状态快照，不发送唤醒/播放命令 |

S0/S7/S8 中 `wake_required=true` 不代表设备此刻允许被唤醒，必须结合主状态判断。设备的背景 PCM 上传不代表它处于陪伴或听取正式用户话语。

## 4. 云端请求恢复等待唤醒

正常情况下由设备的 S1 计时驱动退出。云端若需要提前结束陪伴，发送下面的请求，等待成功 ACK 后再切换为等待唤醒：

```json
{"type":"require_wake","session_id":"0123456789abcdef0123456789abcdef","request_id":"idle-001","state_revision":18,"reason":"companion_expired"}
```

- `state_revision` 必须是云端当前已知的最新设备版本。它是执行前置条件，防止请求迟到后误打断新一轮。
- `request_id` 长度 1～63，匹配 `[A-Za-z0-9_.:-]{1,63}`；重试用同一 ID、同一原始版本，不要更改已发请求的内容。
- `reason` 可带诊断原因；当前设备不靠它决定迁移。

成功响应：

```json
{"type":"require_wake_ack","session_id":"0123456789abcdef0123456789abcdef","request_id":"idle-001","accepted":true,"state":"S3","sub_state":"","state_revision":19,"wake_required":true,"reason":"applied"}
```

处理规则：

| 条件 | 返回及行为 |
|---|---|
| 当前 S1 且版本匹配 | 进入 S3，accepted=true |
| 当前 S3/S5/S6 且版本匹配 | 保持原状态，accepted=true，不点亮睡眠设备 |
| 当前 S2/S4 且版本匹配 | accepted=false，reason=interaction_active |
| 当前 S0/S7/S8 | accepted=false，reason=state_unavailable |
| 版本过期或在 FSM 执行前发生变化 | accepted=false，reason=stale_state |
| 初始同步尚未完成 | accepted=false，reason=sync_required |
| session_id 不属于当前连接、缺失或格式错误 | 忽略，不执行 |
| 缺失/非法 request_id 或 state_revision | 忽略，不执行 |

设备缓存当前连接最近 8 个请求的原始结果，重复请求返回同一响应，不重新迁移。旧缓存被淘汰后仍由版本前置条件防止跨轮次修改。重连会清空缓存，所以云端不能把旧请求重投到新会话。

收到 `stale_state` 时，先更新设备快照并重新判断需求；若确实仍需操作，再生成新 `request_id`。收到 `interaction_active` 时保留当前对话，不要通过循环重试强制打断。重复 ACK 可能携带旧版本，不得回滚更新的状态镜像。

## 5. 原有语音协议的兼容

- 保留 `wake_detected` → `state_ready`：进入已同步会话后，云端识别到唤醒词发送 `{"type":"wake_detected","interaction_id":"wake_123"}`，等设备回 `{"type":"state_ready","interaction_id":"wake_123","state":"S4"}` 再发唤醒回应。
- 本次不更改 MIC_START/MIC_STOP/SPKS/SPKE 或 PCM 二进制格式。不要给现有 PCM 增加帧头。
- 设备处于 S1 时也接受新的 wake_detected，以修复云端先结束免唤醒窗口时的偏差。
- **结束交互必须保留特殊语义链路**：识别到“别打扰我”等结束意图时，通过原 MQTT 语音命令 topic 发送 `{"type":"intent_result","intent":"dismiss","interaction_id":"当前交互ID"}`；“晚安”发送 `intent:"goodnight"`。设备在 S4 播放本地提示，结束后分别进入 S5/S6，云端不要再为该轮发送普通 SPKS/PCM/SPKE。
- **终止提示期间禁止重新启动话语**：发出 dismiss/goodnight 后，等待设备 S5/S6 快照，期间关闭本轮 VAD 触发或丢弃终止提示的回声，不发送 MIC_START/MIC_STOP。固件 0.1.1 会忽略终止提示期间的 MIC_START，保证退出动作不被取消。正常回答期间仍允许用户打断。
- **v2 中 MIC_START 不代替唤醒确认**：S3/S5/S6 收到单独 MIC_START 会被忽略。云端必须先发送 wake_detected，等待 state_ready，再发送实际话语的 MIC_START。S1 的连续对话仍直接走 S2，无需每轮经过 S4。
- 原有裸音频协议尚无逐轮 ID：云端仍须取消旧 ASR/LLM/TTS 任务并校验回调所属连接/轮次，不能把旧 SPKS、PCM 或 SPKE 发到新一轮。
- 固件 S2.2 等待回答默认 30 秒；S4/S2.1 等待话语完成默认 60 秒。超时会关闭会话，重新进行 session_sync。

## 6. 重试、上线顺序与测试

设备对 session_sync/device_state 最多发送 3 次，间隔 2 秒，第三次发出后再等待 2 秒仍未得到匹配 ACK 就结束连接并后台重连。成功 ACK 不需要反向 ACK。新状态替换旧待确认快照时使用新的版本与重试窗口。云端建议 require_wake 也采用有限重试，超时视为结果未知，不能假定设备已待机。

服务器应先部署 v2 支持，再烧录开启 v2 的设备。固件 `CONFIG_JULIA_CLOUD_STATE_SYNC_ENABLE=y` 默认开启；旧服务器不回 ACK 时不会放行语音，不能把这种情况误判为设备麦克风故障。若服务端同时兼容旧设备，请分清两种连接模式：首次 session_sync 表示 v2，不要给已声明 v2 的连接做静默降级。

请添加并运行下列测试：

1. 新连接在无 PCM 的情况下完成 session_sync/ACK，然后才开始唤醒检测和上传。
2. S1 正常到期后应用 S3 快照，要求下一次唤醒。
3. S1 收到 require_wake，设备成功回 S3；重复请求不重复操作。
4. S2/S4/S8 收到 require_wake 被拒绝，不丢当前业务或 OTA 状态。
5. S5/S6 重连及 require_wake 后保持原状态，不被错误唤醒。
6. 旧 session_id、旧 state_revision、重复/延迟 ACK 均不能污染当前状态。
7. ACK 丢失后设备重传；服务器幂等确认，不重复取消业务，也不延长陪伴时间。
8. 状态在请求排队期间变化，require_wake 被拒绝为 stale_state。
9. 握手与状态 ACK 持续缺失时设备按约 6 秒期限断开，并重新建立全新会话。
10. MQTT 暂不可用但 WSS 正常时，服务器仍独立处理 WSS 状态同步；网络恢复后不能继承旧免唤醒资格。

交付请说明修改文件、会话任务取消策略、测试结果、兼容旧客户端的策略，以及服务器日志中如何查看 session_id/state_revision/request_id。不要记录设备凭据或音频内容。
