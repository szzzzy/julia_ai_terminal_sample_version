# 服务端 MQTT 断联清理修复

2026-09-09，先备份后部署。此改动不需要更新固件。

备份：服务器 `/opt/julia/backups/mqtt-liveness-before-20260909_171630/api_server.tar.gz`，包含程序、配置、证书和发布固件，排除虚拟环境、results 和运行历史日志。SHA-256：`f3457bae6e0cf2e01967eed16c7202133cc90a244186292dc836834951f98fce`。

修复文件：`server/mqtt_pure.py`、`server/broker.py`、`server/run_server.py`、`server/session_hub.py`。原生回归测试：`tests/test_mqtt_liveness.py`。

- CONNECT 后按 1.5 × keepalive 限制无完整报文的等待时间；设备 keepalive=60 时约 90 秒判离线。keepalive=0 按协议禁用此超时。
- 同一 client_id 新连接关闭旧连接；旧连接结束不会把新连接置为离线，也不会向新连接发送旧报文的应答。
- 所有退出路径关闭 writer，清理 clean session；持久会话保留订阅但不再标记在线。
- 服务停止先回收连接，再等待监听器关闭，避免等待残留 socket 而卡住。
- 状态接口 hub 中增加 `mqtt_online` 字段（未知为 null），并记录连接/离线事件及原因。历史设备条目仍保留，不等同于在线设备。

隔离测试共 10 项通过，包括静默失联、心跳续期、同 client_id 接管、clean/persistent 会话、半包超时、keepalive=0、服务停止和原有 5 项协议回归。测试记录位于服务器 `results/mqtt_liveness_fix_20260909/tests.log`。

部署前源文件与备份逐一比对，防止覆盖并行修改。首次部署检查发现旧服务已经停止，未执行停止操作；随后部署并启动修复版。启动后发现缺少原先进程环境中的 DASHSCOPE_API_KEY，已从服务器既有 `.env` 恢复该变量（不输出密钥），正常停止修复版后重新启动，最终 PID 32302。今后手动启动也应加载现有 `.env` 中所需环境变量。

物理断电后的最终验收：先让真实设备连接，再断电；WSS 按独立保活机制断开，MQTT 最迟约在最后完整报文后 90 秒置为离线。短暂仍显示 TCP ESTAB 属于检测窗口，超时后不应继续残留。此次软件验证不替代该实机步骤。
