# 固件与云端多设备兼容性检查

这是修正前的审查记录（云端 a68e41b / 阶段固件）。后续已按 [control_protocol=1](MULTIDEVICE_CONTROL_V1.md) 修正两端；保留下面的问题和复现证据用于追踪，不代表 0.1.4 当前结果。

结论：**基础身份、主题与严格模式 v2 握手匹配，但业务链路尚未完全匹配，不建议进入双设备正式验收。** 本次仅检查，未修改固件/云端业务源码、未部署、未连接真机。

## 核对版本与证据

- 固件：本地 `b27969a` 基线上的上轮多设备修改工作区，按 `CONFIG_JULIA_MULTI_DEVICE_ENABLE=1` 检查。
- 云端：`/opt/julia-multidevice-20260910`，提交 `a68e41b5fff5a35be61c0295159d0cad89586ddf`，业务源码无未提交差异。RELEASE.json 标记未生产部署、未真机/付费 API 验证。
- 已下载五份业务源文件及协议文档到 `build/cloud-compat-20260910`；本地 SHA-256 与远端逐文件一致。没有下载配置或凭证。
- 离线探针 [check_compat.py](../build/cloud-compat-20260910/check_compat.py) 编译实际固件状态同步/控制校验 C；执行云端实际 DeviceSession、ScopedMqtt、send_vcmd、_publish_intent_result 函数。仅替换 socket、MQTT 发送、引擎对象和资源分配边界，无真实 ASR/LLM/TTS。
- 原始结果：[result.json](../build/cloud-compat-20260910/result.json)。探针断言用于复现当前缺陷，退出码 0 表示复现成功，不表示兼容性通过。
- 本次重跑固件 CTest **30/30 通过**。云端最终 `validation/acceptance.log` 记录 **80 项 OK**，`multidevice-final.log` 记录 **17 项 OK**；没有重新运行云端套件。较旧的 regression-final.log 存在失败，不能代替最终 acceptance.log。

## 阻断与缺口（按优先级）

### 1. [P1] 首次唤醒的晚安/拒绝 interaction_id 被云端覆盖为空

云端真实引擎在唤醒时设置 `_session_interaction_id`，但 DeviceSession.interaction_id 仍为空，直到 retire 才从引擎复制。`ScopedMqtt.send_vcmd` 使用会话上的空值，`MqttAdapter.send_vcmd` 又用它覆盖意图 JSON 原本正确的值。首次唤醒阶段 S3→S4 不触发 retire。

复现：引擎 `_session_interaction_id="wake_123"`，调用实际 `_publish_intent_result("goodnight")`，输出 `voice/esp-001122334455/vcmd` 中 `interaction_id=""`；实际固件校验返回 `stale_interaction`。同路径 dismiss 也受影响。

位置：[云端 ScopedMqtt](../build/cloud-compat-20260910/state_sync.py:435)、[云端覆盖字段](../build/cloud-compat-20260910/mqtt_adapter.py:203)、[固件校验](../main/voice/voice_control_guard.c:60)。

建议先修云端：使用经过当前 engine 所有权校验的交互 ID，保证唤醒消息和后续 MQTT 意图一致；不能让固件放宽为空即接受。

### 2. [P1] 云端禁止在 S2.3 发送固件用于打断的 MIC_START

固件 `voice_service_apply_mic_start` 明确支持 S2.3→EVT_INTERRUPT 并立即取消播放；云端 `DeviceSession.allowed` 对 MIC_START/MIC_STOP 只允许 S1、S4 或 S2.1，因此处于 S2.3 时实际 `deliver(engine,"MIC_START")` 返回 false。

这是双方命令准入规则不同。若 SPKE 后软件/硬件尾音尚未排空、状态仍为 S2.3，不能假定固定等待时间已使设备进入 S1；直接打断更需要该命令在播放态可达。

位置：[云端准入](../build/cloud-compat-20260910/state_sync.py:380)、[固件打断分支](../main/voice/voice_service.c:765)。建议区分 MIC_START 和 MIC_STOP 的状态准入，允许合法当前轮的播放打断；保留固件终止提示期间的原拒绝行为。

### 3. [P1] 同一连接第 33 条控制后永久拒绝新控制，且影响新唤醒

固件去重表为 32 项、不淘汰，只在 WSS 重建时清空；云端为新意图生成新 request_id，未实现该容量的协商/回收。探针前 32 条通过，第 33 条返回 `dedup_capacity`。normal 意图也占槽。wake_detected 也调用相同 guard，所以满表后新的唤醒同样会被拒绝。

位置：[容量](../main/voice/voice_control_guard.h:8)、[拒绝逻辑](../main/voice/voice_control_guard.c:63)、[唤醒共用校验](../main/voice/voice_service.c:877)。这是上轮保守阶段实现的已知限制，云端完成后仍未消除。

需要双方确定有界去重的安全回收条件；不能简单淘汰旧 ID 后声称不重复执行，也不应靠定期断线来腾空间。

### 4. [P1] busy 的两端恢复链路未接通，可能停留 S3 不再恢复语音

云端容量不足时发送 `busy/voice_capacity` 并把当前 revision 记为 blocked。相同 revision 后续 ensure_engine 直接返回，不再申请资源。固件没有 busy 处理分支，消息最终被忽略。若发生在 S3，固件没有 S4 听音/S2.2 回答超时触发新会话，新状态变化也并不必然发生。

复现：容量拒绝一次后，对同一 S3/revision 连续调用 10 次，资源申请次数仍为 1；不会仅因容量后来释放而重试。云端协议要求设备结束本轮并生成合法的新状态，固件尚未实现该行为。

位置：[云端 blocked gate/忙碌](../build/cloud-compat-20260910/state_sync.py:249)、[固件文本分派](../main/voice/voice_service.c:923)。应明确实际 FSM 事件与重新准入条件，不通过伪造 state_revision 绕过检查。

### 5. [P1] 同一个 wake ID 内，迟到旧轮 MQTT 仍可能被执行

双方仍把 interaction_id 作为唤醒期间继承的标识，没有每次实际话语/打断更新机制。云端下行虽有 connection_id/epoch，固件不保存或校验这些字段。云端 owner 检查可阻止尚未发布的旧任务；不能撤回已经送到 broker、之后才到设备的消息。

复现：当前会话/同一 wake ID 下先接受 epoch=2 的消息，再投递不同 request_id、epoch=1 的旧轮消息，固件仍返回 accepted。这个探针证明固件缺少轮次栅栏，不是一次真实 broker 延迟测试。

需要双方选择并定义逐话语 interaction_id 或等价轮次屏障，以及跨 WSS/MQTT 顺序；不能单独启用 epoch 比较，因为固件当前没有权威的 epoch 更新协议。裸音频仍依靠云端发送任务隔离。

### 6. [P2] 执行回执仍未闭合；vstatus 状态镜像不满足命令 ACK

云端新协议要求“同 request_id 的 vstatus 确认”和重复请求返回同执行结果。固件仅缓存 ID，没有结果；重复消息直接丢弃。现有 vstatus 是 device_state 镜像，request_id 为 `device_state-<revision>`，不是该命令 request_id，且不能表示 normal 命令执行或提示播放完成。

云端源码接收 vstatus 也只记录到 hub，未形成命令完成等待/结果校验；协议尚缺具体回执 type、状态字段、结果码。此项需两端共同补齐，不能只在文档中声明已支持。

位置：[固件实际分派](../main/voice/voice_service.c:1395)、[状态镜像](../main/voice/voice_state_sync.c:183)、[云端 vstatus 接收](../build/cloud-compat-20260910/mqtt_adapter.py:132)。

## 已匹配项

| 项目 | 结论与边界 |
| --- | --- |
| device_id | eFuse 生成的 esp-<12 hex> 符合云端字符限制；WSS token 查表绑定 ID、MQTT 用户密码映射身份的方式对齐。实际 A/B 凭证没有读取或核验 |
| 专属主题 | 严格固件 vcmd/vstatus 与云端主题、ACL 设计对齐 |
| session_sync/ACK | 实际固件 C 产生消息交给实际云端 handle，云端 ACK 再交回 C，双方 ready=true；保留 v2、session_id、state_revision 等字段 |
| device_state/ACK | 上述往返继续得到 device_state_ack；device_id/request_id 条件匹配 |
| state_ready/require_wake | 源码检查身份封装和原状态字段匹配；未做完整真机唤醒/休眠链路验收 |
| 音频编码 | 上行 PCM1 16kHz、SPKS 16/24kHz、裸 PCM 下行、SPKE 后排空设计一致；不能据此证明打断准入或真实 DMA 行为一致 |
| 认证拒绝 | 云端 4401 与固件冷却分支匹配；HTTP 401/403 也能识别 |

## 其他兼容边界

- 当前根 sdkconfig 仍是 `JULIA_MULTI_DEVICE_ENABLE` 关闭、MQTT AUTH_NONE；直接使用默认镜像不能接新严格服务。严格候选镜像开启多设备模式但凭证为空，是审阅产物，不是已配置设备镜像。
- 云端默认同步期限 5 秒，固件发送窗口约 6 秒；双方都有限退出，不视为字段不兼容，但服务端可能先关连接。
- 云端建议指数退避加抖动，固件普通重连仍固定配置间隔；4001/4408 未有专门恢复策略。属于待完善的恢复行为，不能宣称已按新建议实现。
- 双方均无业务消息有效期字段/校验，尚不能拒绝同会话同交互中“首次到达但已过期”的消息。

## 下一步顺序

先修复云端首次意图 ID 和 MIC_START 准入；双方闭合轮次、执行 ACK/去重回收与 busy 恢复；随后重跑本离线探针及各自回归，再执行隔离测试环境中的双设备验收。此次测试均未消耗外部推理 API、未改变生产状态。
