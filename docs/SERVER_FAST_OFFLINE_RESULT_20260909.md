# WSS 快速离线反馈实现与验证

固件不变。服务器配置 ping_interval=3、ping_timeout=6、close_timeout=1，沿用 websockets 12.0 匹配 PING/PONG 的保活机制。

状态接口的 wss 新增 transport_connected、session_ready、voice_online、last_disconnect。关闭中的 socket 不再计为在线；没有完成同步的活动会话时，engine.online=false，避免历史引擎快照误报在线。MQTT 在线状态独立保留在 hub 的 mqtt_online 中。

断开日志从实际发送/接收的关闭帧识别 ping_timeout、session_replaced、abnormal_close、normal_close，记录发现时刻、会话和关闭码；1006 不再被简单解释为设备重启。旧会话清理不会影响新会话的在线判定。

验证记录：

- 首轮快速离线测试 4 项通过：正常心跳持续 61 秒不误断；持续上行但不回 PONG 时 9.02 秒内标记离线；正常关闭和旧快照处理正确。
- 状态同步回归 22 项通过。
- 补充不同心跳相位、MQTT在线而WSS离线、网络、协议、音频下行、关闭及MQTT清理回归共21项通过。
- 测试运行于独立工作树和隔离端口，未向真实设备注入故障。

服务器测试日志位于 `/opt/julia/api_server/results/wss_fast_offline_tests.log`、`wss_fast_offline_state_tests.log`、`wss_fast_offline_regression.log`。

基线提交 `3cd300f`，备份标签 `backup/before-fast-offline-20260909_174726`；备份目录 `/opt/julia/backups/before-fast-offline-20260909_174726/`。实现分支 `codex/server-fast-offline`，部署采用快进合并，保留可回退记录，不向远端推送。

边界：6～9秒是静默失联目标，调度和状态页面刷新会增加可见延迟；明确关闭事件更快。MQTT仍按原60秒心跳、约90秒超时独立处理。需要真机分别在USB和电池供电下做最终断电验收，此改动不解决供电压降或固件内存不足。
