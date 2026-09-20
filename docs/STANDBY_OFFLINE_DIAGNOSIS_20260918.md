# 长时间待机后离线、摇晃无响应：阶段性定位

> 2026-09-18 后续实现与验证见 [S6 WSS 恢复实现记录](S6_WSS_RECOVERY_IMPLEMENTATION_20260918.md)：已补齐恢复缺口和播放积压诊断，现场根因尚未证实；下文保留原始排查阶段结论。

时间：2026-09-18，检查时本机约 10:39（UTC+8）。依据当前工作区；不能据此证明设备运行的就是当前构建。

> 后续修复：本文第 2 节 MQTT 重建失败不再重试的问题已在工作区修复，详见文末。前面的探针输出保留为修复前证据。

## 现场描述

用户反馈：有供电，待机约 1 小时后服务器看不到设备连接，摇晃没有反应，按 RST 恢复。发生于本次检查之前的一段时间，没有精确的断线时刻，尚未区分 MQTT 在线状态与 WSS socket 状态。

本次没有刷机、复位、修改产品代码或改服务器。普通和提升权限后的串口枚举均为 `no ports found`，因此没有取得本次故障的串口记录、任务栈或设备构建号。本机 SSH 别名配置没有当前文档所示语音服务器的连接入口，本次也没有获得新的服务端日志。以下内容是源码与主机故障注入结论，不能替代现场根因证据。

## 1. 已确认：睡眠后的离线状态会表现得像“卡死”

- `sdkconfig` 中 S3 待机期限为 300 秒，到期进入 S6。
- [julia_fsm_runtime.c:449](D:/Espressif/projects/julia-fused-base/main/behavior/julia_fsm_runtime.c:449) 的睡眠呈现关闭背光和面板；头像更新任务也暂停。
- [julia_quiet_power.c:49](D:/Espressif/projects/julia-fused-base/main/behavior/julia_quiet_power.c:49) 在当前本地采集配置下持续保留网络；这不是正常关网休眠。
- [interaction_event_allowed](D:/Espressif/projects/julia-fused-base/main/behavior/julia_fsm_runtime.c:711) 要求业务服务 ONLINE，且 MQTT、WSS、语音状态同步都 ready，才允许 `EVT_MOTION_WAKE` 和 `EVT_WAKEUP`。
- S6 下断联事件没有对应的主状态迁移，仍保持 S6；offline 标记不能在关闭的面板上提供反馈。网络恢复后不会重放已经拒绝的摇晃事件，需要新的唤醒输入。

提取实际准入函数、链接实际 FSM，模拟进入 S6 后分别失去 MQTT/WSS，再恢复连接：

```text
S6 mqtt_offline: motion_allowed=0 voice_allowed=0 disconnect_transition=0 state=S6_SLEEP
S6 wss_offline: motion_allowed=0
links_restored: motion_transitions_to=S4_INTERACTION
```

因此，“黑屏 + 摇晃无响应 + 服务器离线”不一定代表 CPU 停止运行；该表现可以由现有状态策略直接产生。这解释了外观，尚未解释最初掉线和不能恢复的原因。

## 2. 已确认的恢复漏洞：MQTT 重建失败后不再重试

位置：[mqtt_comm.c:1109](D:/Espressif/projects/julia-fused-base/main/network/mqtt_comm.c:1109)，`mqtt_ota_check_task()` 的 `s_reconnect_requested` 分支。

执行顺序为：清除重建请求 → stop 客户端 → start 客户端 → 无论 start 成败都令 `wait_ticks = portMAX_DELAY`。失败时只打印日志，没有恢复重试请求，也没有把 `s_power_stopped` 设置成需要启动的状态。随后 ready 位未置位、SUBACK 截止时间为 0，会继续无限等待。

本地 ESP-IDF 5.5.4 的 `esp_mqtt_client_start()` 确实可能在 MQTT task 创建失败时返回 `ESP_FAIL`；成功停止后的客户端没有正在运行的 MQTT task，不能依靠其内部自动重连补救。

主机探针从生产函数提取“已停止客户端恢复”“重建请求”“未 ready 等待”三个相邻分支，注入 stop 成功、start 失败，并额外给予后续循环执行机会：

```text
mqtt_restart_failure: start_calls=1 running=0 retry_flag=0 stopped_flag=0 wait_forever=1
```

即使额外唤醒 owner，也不会再次 start。这个漏洞可以使 MQTT 长期离线；结合第 1 节，S6 的摇晃与语音唤醒都被阻止，RST 重走初始化可以恢复。

**适用限制：**尚无现场 `Failed to restart MQTT client` / `Error create mqtt task` 日志，不能认定本次就是此路径。它本身也不直接停止独立 WSS task；如果确认 MQTT 与 WSS 同时消失，需要继续检查共同的 Wi-Fi/IP、资源或任务阻塞问题。

修复方向：stop 成功后明确记录“客户端已停止”；start 失败时使用有界退避继续启动，成功后才进入等待 SUBACK 的阶段。stop 失败也必须单独处理，不能继续假定客户端已停止。增加 fail-once/succeed-next 和连续失败的回归测试。

## 3. 关于“约 1 小时”

当前本地配置的关键时间：

| 机制 | 当前值 |
| --- | --- |
| S3/S5 自动进入睡眠 | 300 秒 |
| WSS 保活 / PONG 窗口 | 15 秒 / 10 秒 |
| WSS 常规重连等待 | 5 秒 |
| MQTT keepalive / 常规重连 | 60 秒 / 10 秒 |
| OTA 正常检查间隔 | 21600 秒，即 6 小时 |
| OTA 下载冷却上限 | 3600 秒，仅失败升级路径 |

没有证据表明正常待机流程设置了“一小时后停止联网”。当前固件显式关闭自动 light sleep；睡眠状态主要关闭屏幕，并不应关闭联网。不能因为约一小时就直接归因计时溢出、自动深睡眠或 OTA。路由器租约/服务端会话期限也尚无日志证据。

WSS 正常 owner 循环会持续重连；Wi-Fi 也有超时与退避。所以若现场长期没有任何重连尝试，优先查 owner 是否仍推进、Wi-Fi/IP 状态是否失真、初始化重试是否卡住。前次审查的 OTA 约 50 分钟空读问题仅在实际升级下载中成立，目前没有进入 S8 的证据。

## 4. 历史日志能证明什么

- 9 月 9 日的静置日志记录过上行 overflow、TLS tx_stall，并随后恢复连接；这是不同日期、不同配置的历史样本。
- 9 月 17 日 `build-host-state-recovery/s4-live-diagnosis.log` 记录的是 S4 监听 60 秒超时，随后进入 S7、返回 S3、WSS 重新同步成功。电池周期日志一直存在。
- 这些记录不能作为 9 月 18 日本次故障的根因，且尚未拿到本次最后一次正常日志与断线日志。

## 5. 下一次现场最有区分力的证据

在 RST 前被动保存串口，避免打开端口时通过 DTR/RTS 自动复位。现有固件每 10 秒输出 `JULIA_BATTERY: monitor`，可先判断至少有任务在运行：

1. 电池日志持续，且反复 Wi-Fi/TLS 重连失败：重点查网络与服务端拒绝原因。
2. 电池日志持续，出现 MQTT start 失败后再无启动：对应第 2 节漏洞。
3. 电池日志持续，但 WSS/FSM 再无进展：需要任务状态/栈回溯判断同步等待；看门狗只监控 idle 不能排除业务任务阻塞。
4. 全部日志停止：还不能单凭这一点区分电源、USB、串口或系统停滞，需结合任务/供电现场。

服务端同时保留 WSS close reason、最后接收时间、认证重试，以及 MQTT disconnected/reconnected 时间；设备 ID 与日志时间必须对应。修复前不要仅延长待机时间或屏蔽睡眠，那会改变现象而保留恢复缺陷。

## 可重复验证

[probe_standby.py](D:/Espressif/projects/julia-fused-base/docs/reviews/2026-09-17/probe_standby.py) 使用当前生产源码，生成物写入已忽略的 `build-review-20260917`：

```powershell
& D:/Espressif/python_env/idf5.5_py3.13_env/Scripts/python.exe docs/reviews/2026-09-17/probe_standby.py
```

这是定位反例，断言确认的是当前问题存在，不是修复验收。没有执行一小时硬件复现。

## 后续修复与验证（2026-09-18）

用户授权后修复 `mqtt_ota_check_task()`：

- 重建时检查 stop 的返回值；失败保留重建请求，200 ms 后重试，不继续 start。
- stop 成功记录客户端已停止，下一轮复用暂停恢复的启动流程。
- start 失败保留已停止状态，1000 ms 后继续启动；成功才清除该状态。
- 暂停请求仍优先处理；启动成功不等于业务就绪，继续等待既有 SUBACK 放行。

新增 [test_mqtt_restart.py](D:/Espressif/projects/julia-fused-base/tests/host/test_mqtt_restart.py)，提取完整生产 owner 循环，仅模拟 RTOS/客户端等外部边界。覆盖首次启动失败后成功、连续启动失败、连续停止失败、停止失败后成功、重试中暂停/恢复，以及未订阅就绪时不发布 OTA 检查。该测试在旧实现上失败、修复后通过。

验证：主机 CTest 46/46 通过；ESP32-S3 固件构建通过，`build/julia_fused_base.bin` 大小 `0x27c270`。首次构建遇到本机 ccache 路径/临时目录权限问题，补齐当前进程 PATH 并将 ccache 临时目录指定到工作区后完成构建，未修改产品配置。构建包含当前工作区已有的其他未提交改动。

前文 `probe_standby.py` 是修复前的反例，其 MQTT 断言不再作为通过标准；修复后以 `mqtt_restart` CTest 为准。未刷机或复位设备，也未进行一小时实机复现；本次修复不等于已证实用户现场故障根因。
