# 语音状态回执时序修复

基线：def92ee；备份标签 backup/before-voice-state-wait-20260909。服务器备份目录 `/opt/julia/backups/voice-state-wait-20260909/` 保存 Git bundle 和故障日志。

故障：2026-09-09 18:03:10，MIC_STOP 后设备仍处于 S2.1，空识别轮次提前发 SPKS，被状态规则拒绝。发送层把 False 当作无客户端连接并终止引擎；设备随后才上报 S2.2。

修复：

- 记录成功发送 MIC_STOP 时的引擎所有者、会话轮次及设备 revision。SPKS 等待同一轮次的新 revision 且处于允许播报状态。
- 采用事件唤醒和绝对截止时间；默认3秒，可用服务器 voice.state_wait_timeout_s 调整（上限10秒）。重复/旧revision和其他session回执不能放行。
- 暂时状态不满足就等待；状态拒绝或旧任务失效按轮次取消处理；真正的 socket 异常仍保留错误分类。
- 超时取消当前轮次与API任务、保留WSS会话并阻止旧音频重放，等待设备进入新的收音/待机/陪伴状态再创建新轮次。不会强制改变设备状态，也不会盲目补发。
- 状态接口增加 engine.available 和 last_delivery_issue；等待设备恢复时标记 awaiting_device_state，连接在线不再掩盖语音轮次不可用。

验证：47项状态同步、音频下行、API关闭、MQTT和协议回归通过；另9项定向测试通过，覆盖实际1.6秒回执延迟、超时、迟到回执、打断/断开、真socket错误、工作线程取消和状态页。测试使用隔离环境，不替代真机连续对话验收。

固件未修改，保留服务器WSS 3/6秒快速断联和MQTT清理。部署使用独立 codex/voice-state-wait 分支及快进合并；保留原有环境变量，缺失时从既有.env加载API密钥，不输出密钥、不推送远端。
