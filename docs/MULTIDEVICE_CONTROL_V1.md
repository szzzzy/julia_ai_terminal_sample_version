# 两端修正契约：control_protocol=1

本次同时实现于严格固件和独立云端修复工作区，不改变 protocol_version=2、物理 device_id、随机 session_id、FSM 状态版本或 PCM 格式。旧单设备入口保持独立；严格 session_sync 增加 control_protocol=1，严格云端不对不支持此扩展的设备静默降级。

## 轮次确认

云端在每次 wake_detected 和实际 MIC_START 前，发送 WSS `interaction_sync`，携带原五项身份字段及从 1 递增的 interaction_seq。interaction_id 是该轮唯一 ID；不是引擎 epoch。固件接受恰好下一序号后使旧控制失效，回 `interaction_sync_ack`（accepted、code、interaction_seq、device_time_ms）。重复同序号同 ID 同 request 返回同一 ACK，不重复清理；旧序号或不同内容拒绝。此步骤不伪造 FSM 状态，真正状态迁移仍由后续 wake_detected/MIC_START 驱动。

云端最多 3 次、间隔 2 秒等待匹配 ACK，再发送该轮业务；失败关闭本连接，重新同步。终止提示期间固件拒绝开启新轮次，保留晚安/拒绝语义。连接任务和发送锁共同保证屏障之前的旧音频不能越过新轮次；裸 PCM 不增加帧头。

同一个 sequence/interaction_id 改 request_id 或 purpose 返回 request_id_conflict。state_ready 回显 interaction_seq；这仍是唤醒状态回执，不是播放完成。

## MQTT 控制和执行结果

控制除原身份字段外必须携带 interaction_seq、control_seq 和 expires_at_ms。control_seq 在每个 interaction_seq 内从 1 递增，云端串行发送并等待上一项的最终执行回执；最多 8 项排队，禁止跨轮重发。

首次唤醒前的会话范围 FILE_SEND 使用 interaction_seq=0、interaction_id=""，仍要求已完成 session_sync 和有效的设备时钟锚点；它不授予语音交互资格。MIC_START/意图控制仍须完成语音轮次确认。

固件在 WSS owner 实际执行时检查身份、轮次、序号和有效期。结果为 `type=control_ack`，回显五个身份字段与 interaction_seq/control_seq，含 accepted 布尔、code、state_revision。通过本设备 vstatus 发布；MQTT PUBACK 不代替此回执。accepted 表示业务命令已经应用（例如已启动本地终止提示），不表示声音已经播完；真实完成仍由既有播放排空/FSM 快照体现。

固件保存最近 8 项执行结果和业务指纹。重复序号/ID/内容返回原结果；同序号或仍在缓存内的同 ID 改内容返回 request_id_conflict。淘汰记录仍受 control_seq 高水位保护：旧序号返回 stale_request，永远不重新执行。新轮次确认后回收缓存，旧轮消息先被轮次校验拒绝。不承诺无限期保存历史结果，也不靠强制重连腾槽。

超时最多重发原封装 3 次、间隔 2 秒；不改 request_id、序号、内容或有效期。超时结果未知，云端取消该轮而非猜测成功。格式错误/错误设备或会话只脱敏诊断；可关联的拒绝结果使用 expired、stale_interaction、out_of_order、unsupported_operation、state_unavailable、terminal_reply、request_id_conflict、stale_request 等 code。

结果指纹使用 cJSON 紧凑序列化后的完整封装，属性顺序也需保持不变。发送端一次序列化后原样重发。缓存回收后，以 (session_id, interaction_seq, control_seq) 的高水位拒绝旧请求，云端始终生成新的 UUID request_id；不允许通过改序号复用旧 request_id。旧的独立 MQTT 管理 CLI 不能绕过活动 WSS 轮次发严格控制；需通过 DeviceSession 调度。

## 时钟与有效期

device_time_ms/expires_at_ms 使用固件 esp_timer 启动后单调毫秒，不使用 UTC。云端以最近匹配轮次 ACK 的 device_time_ms 加自身单调经过时长推算保守期限；网络传输延迟会缩短可用时间，不能用服务器墙钟直接减固件时间。单条有效期上限 10 秒；已缓存的重复请求先返回原结果，不能把已经执行的请求重新判为未执行。没有本连接的时钟锚点时不发控制。

## busy 与恢复

云端引擎容量不足返回 busy/voice_capacity 和有界 retry_after_ms（1～30 秒退避）。冷却期间只丢弃当时收到的 PCM，不积压重放；到期可用新的实时输入重新申请资源，即使 S3 revision 未变。其它业务取消原因仍保留原轮次失效门槛。

固件收到匹配 busy 时，取消该轮控制及对话声音，丢弃未发送采音，暂停至冷却到期；活动 S1/S2/S4 通过新增真实 EVT_VOICE_BUSY 返回 S3。S3/S5/S6 保持原状态，尤其不点亮休眠设备。到期从空上行缓冲恢复实时采音，不恢复旧轮次或伪造 state_revision。忙碌诊断与网络断开分开处理。

busy 必须匹配当前 interaction_seq/interaction_id；迟到旧轮 busy 无效，终止提示期间不取消本地晚安/拒绝。同步/屏障/执行 ACK 超时后重连最少等待 30 秒；4001 会话替换、4404 路径错误和认证拒绝最少 60 秒。普通网络重连仍保留原间隔和快速离线检测。

## 不变项与发布门槛

保留 16kHz PCM1 上行、SPKS 16/24kHz 下行、播放预缓冲、SPKE 软件/DMA 排空、快速断线检测及唯一任务所有权。先完成两端自动化和固件构建，再审阅双设备步骤；本次不烧录在用设备、不部署或切换环境。
