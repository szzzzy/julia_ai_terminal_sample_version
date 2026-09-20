# S6 中 WSS 先断线：复查记录

> 2026-09-18 后续实现与验证见 [S6 WSS 恢复实现记录](S6_WSS_RECOVERY_IMPLEMENTATION_20260918.md)：已补齐恢复缺口和播放积压诊断，现场根因尚未证实；下文保留原始排查阶段结论。

## 现场条件更新

2026-09-18 用户确认：此前 MQTT 重启失败重试修复已经刷入，问题仍发生；服务器日志首先显示 WSS 断线，报告 `code 1006`、`ping timeout`，后续没有重新握手。因此不能用旧 MQTT start 失败漏洞解释本次现象。尚未取得日志原文和同时段 MQTT 状态。

这组现场信息优先指向 WSS owner 停止推进，或持续网络不可达；没有证据支持“持续重新握手但被服务端拒绝”。服务器没有收到握手不等于设备从未尝试连接，仍需设备端日志区分。

本轮串口枚举返回 `no ports found`，未取得设备任务栈或实时日志。仅检查源码和执行主机测试，没有刷机、复位或修改产品代码。

## 确认的行为

1. 当前 `sdkconfig` 启用 `CONFIG_JULIA_LOCAL_CAPTURE_ENABLE`。`julia_quiet_power.c` 在 S6 保留网络与采音；`wss_quiet_blocks()` 在该配置恒为 false。未发现按 S6 状态禁用 WSS 重连的分支。
2. `interaction_event_allowed()` 要求服务 ONLINE、MQTT ready、WSS ready、状态同步 ready 全部满足。只要 WSS 不就绪，S6 的运动和语音唤醒就会被拒绝；这一外观不能证明 CPU 死机。
3. 正常 WSS 循环在下行静默 15 秒后发送 PING，再等待 10 秒下行响应；结束会话后通常等待 5 秒再连接。鉴权和协议策略可能延长退避，这些数字不是整个恢复流程的硬上界。

## 优先排查的恢复薄弱点

`wss_session_task()` 同时负责 TLS 建连、收发、保活、上层回调及重连。`wss_run_session()` 在调用 `on_session_end()` 返回后才打印 `WSS session ended ... reconnecting`，然后才能进入重连等待。

断线清理链为：

```text
wss_run_session
  -> TLS destroy
  -> voice_service_on_session_end
     -> 清理 capture / state sync / ring / playback
     -> post_fsm_event(EVT_WSS_DISCONNECTED)
        -> julia_fsm_runtime_post_sync
           -> xQueueSend(..., portMAX_DELAY)
           -> xSemaphoreTake(..., portMAX_DELAY)
  -> "WSS session ended ... reconnecting"
  -> 退避和下一次 wss_connect
```

建立新会话的 `EVT_VOICE_SESSION_RESET`、接收同步确认后的 `EVT_WSS_CONNECTED` 同样同步等待 FSM。只要 FSM 不再消费或其处理流程阻塞，WSS owner 就可能无限等待：保活和自动重连也随之停止。

这是确认存在的无超时依赖，不是已经复现的死锁。本轮检查没有证明哪个锁实际卡住，也没有证明用户现场走到了上述等待。

当前任务看门狗配置检查两个 idle task，没有发现 WSS owner 独立心跳订阅；`CORE_TASK_STALLED` 只是预留故障类型。业务任务阻塞而 idle 继续运行时，没有这类恢复兜底。

不能简单把 semaphore 等待改成超时后直接返回：现有消息持有调用栈上的 semaphore 和 applied 指针，FSM 延迟消费会访问已经失效的存储。若修复，需要先实现安全的请求生命周期和过期事件处理。

## 用日志区分下一步

| 证据 | 排查方向 |
| --- | --- |
| 服务端 ping timeout，没有后续连接请求 | 检查设备 WSS owner 是否停在回调、TLS 或 FSM 等待；也要排除网络请求无法到达服务器 |
| 设备已有 receive failed/session probe，但缺少 session ended/reconnecting | 检查 TLS destroy 和 on_session_end 清理路径 |
| 已出现 connected & authenticated，随后没有 session_sync | 检查 on_session_start 及其同步 FSM 调用 |
| 反复出现 TLS connect failed / upgrade rejected | 已在重试，转查网络、TLS、鉴权或服务器拒绝原因 |
| 反复出现 cloud state ACK timeout | socket 建连成功，但业务同步失败 |
| MQTT 同时掉线 | 优先补查共同 Wi-Fi/IP、资源与调度问题；WSS 先被服务器观察到不代表其必然是根因 |

需要对齐首次 WSS 断线前后约一分钟的服务端原始日志（close code/reason、ping timeout、重新握手记录），以及同一设备 MQTT 在线状态。故障设备若仍在现场，复位前保存串口和任务栈比继续重启更有区分力。

## 本轮验证

在 `build-host-state-recovery` 执行 `recovery_fsm`、`recovery_wifi_profiles`、`recovery_voice`、`mqtt_restart`，4/4 通过。这些测试会提取当前生产函数重新编译，验证逻辑恢复路径；不模拟真实 FreeRTOS 多任务死锁、无线网络和长时间硬件运行，不能据此排除现场问题。
