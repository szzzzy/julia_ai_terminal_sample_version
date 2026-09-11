# Julia 真实设备接入诊断（2026-09-11）

## 结论

目标设备 esp-28848591972c 实际启动的是仓库普通模式旧镜像，而非私有配套镜像。当前 WSS 4401 和 MQTT EOF 有一致的配置原因：旧 WSS token 不在新版设备认证表内，旧镜像使用明文 MQTT 连接 TLS 1883。无需重新改造多设备架构、修改凭证、证书或服务端口。

## 本次现场证据

- COM8 USB 序列号与 esptool 读取的 MAC 均为 28:84:85:91:97:2c。
- 09:27–09:30 现场检查：PID 55717 运行新版 main.py，命令行配置为 `/opt/julia-runtime/control-v1-20260910/config.json`，cwd 为其 `release/api_server`，同一进程监听 9443、1883、8443。该 PID 仅是本次快照，后续操作必须重新发现进程。
- 被动串口监听再次得到 MQTT transport_read EOF / esp-tls 0x8008，以及 WSS CLOSE 0x1131（4401）。同期云端日志记录 WSS 打开后快速关闭，没有进入设备认证成功日志。
- 本地与服务器凭证包逐字段匹配。配置中的 WSS token 映射到目标设备，MQTT 密码与凭证包匹配。
- 在线 WSS 探针使用有效凭证和故意不合规的 control_protocol=0，收到 `control_protocol_required`，证明当前在线进程接受配套凭证。已检查源码：该拒绝发生在 activate 之前，未取得会话所有权。
- 使用现有 CA、公网 IP 对 1883 完成证书验证及 TLS 握手（TLSv1.3），没有发送 MQTT CONNECT 或接管设备会话。
- 设备分区表：ota_0 为 0x20000，ota_1 为 0x720000。otadata 第一条 seq=1、state=2，第二条擦除状态；ota_1 无 ESP 应用头。
- ota_0 的前 256 字节只匹配普通镜像；esptool verify_flash 对普通镜像全长进行芯片端 MD5 摘要比较，退出码为 0，确认匹配。普通镜像 SHA-256：`171ad32ad3bf50c1b3667e8de07abf7fc0f36e43a4dcd47cc715c598cbcbdfd3`。
- 正常复位后的启动日志明确显示 `Loaded app from partition at offset 0x20000`，ELF SHA 前缀 `92d8b3ff6`，与读回应用头一致。版本仍为 0.1.4，版本号不能区分镜像。
- 普通镜像配置未启用 JULIA_MULTI_DEVICE_ENABLE；MQTT URI 为 `mqtt://8.133.215.254:1883`；其 WSS token 不在当前云端配置的设备认证表内。
- 私有配套镜像重新检查为 2548976 字节，SHA-256 为 `b7b29b67348dea8fe630f1d9063d76daa907db6fa0200f21e7e622afea686814`，flash_args 的应用偏移也是 0x20000。

## 校验限制与操作记录

首次整段 read_flash 在约 0.96 MB 处发生串口传输损坏，不能据此判断 Flash 本身损坏。改用小块读回和芯片端摘要校验后完成身份判定。诊断程序最初以错误的成功文案判断生成了 verify_success=false；实际 verify_flash 退出码为 0。已核对 esptool 源码，其成功文案为 `verify OK (digest matched)`，摘要不匹配会抛出 FatalError；已修正诊断程序，改记录退出码和工具原文。

本次仅读取分区元数据、应用标识并计算校验值，曾短暂复位，最后已恢复正常启动。未烧录、擦除或主动修改 Flash/NVS/eFuse，未修改在线服务。原始临时 Flash 文件自动清理，报告不含凭证。

## 最小修复方案（尚未执行）

1. 保持当前新版云端不变。将指定私有配套应用镜像写入当前启动分区 0x20000；现有分区布局和有效启动记录已满足要求，不需要重写分区表、otadata 或擦除 NVS。
2. 写前再次核对 COM8 的 MAC、镜像 SHA-256，写后执行 verify_flash，并保留完整退出码与脱敏结果。
3. 正常复位后确认启动镜像 ELF 标识与配套镜像一致，串口获得 IP、MQTT_EVENT_CONNECTED，以及 WSS 初始同步 ACK；在同期云端日志确认目标设备接入。只有这些真实设备证据通过才能认定修复完成。

尚不能确定此前用户执行烧录脚本在哪一步未完成，或其后是否有其他镜像覆盖；没有完整成功输出，不能把此处归因于用户选错入口。但本次 Flash 校验已证明当前配套镜像不在运行分区，备用分区也没有有效应用。
