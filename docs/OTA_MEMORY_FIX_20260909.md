# OTA TLS 内存修复（2026-09-09）

设备 0.1.1 在 WSS 已连接时启动 OTA，`mbedtls_ssl_setup` 返回 `-0x7F00`，随后报告 `NETWORK_TIMEOUT`。ESP-IDF 5.5.4 中该值为 SSL 内存分配失败，且 ESP-TLS 在此路径保存其正数幅值。

## 修改

- sdkconfig 和 sdkconfig.defaults 选择 `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`，让 TLS 使用板载 8 MiB PSRAM。保留原有 TLS 记录缓冲长度和证书校验。
- OTA 建连前和失败时记录内部 RAM/PSRAM 可用量和最大连续块。
- 在错误枚举末尾追加 `OUT_OF_MEMORY`，保持 NVS 历史编号；识别正负两种 SSL 分配错误和 ESP_ERR_NO_MEM。
- 保留底层操作错误，避免保存重试记录成功后日志错误地打印 ESP_OK。
- 服务器 `/opt/julia/api_server/server/mqtt_adapter.py` 通知加入 `schema_version: 1`。原文件备份在 `results/ota_memory_fix_20260909/mqtt_adapter.py.before`。服务已于 16:39:51 启动，1883/9443/8443 监听正常，状态接口返回 200。

## 验证与产物

- ESP-IDF 固件构建通过，镜像 checksum 和 validation hash 有效。
- TLS 错误注入测试通过：正负 0x7F00、ESP_ERR_NO_MEM、证书/握手失败、普通网络失败。
- 服务端通知载荷测试和 5 项协议回归测试通过，未向真实设备发送测试通知。
- 修复镜像：`build/julia_fused_base.bin`，版本 0.1.2，2,259,072 字节。
- 文件 SHA-256：`48102befeb7e16e0e0d883c2763dfc9d33eb925025a0eeb6c20aad54fc158daa`。

## 尚待实机验证

本轮没有烧录设备，也没有替换云端正在发布的旧 0.1.2 文件。先通过 USB 烧录此修复版；确认 WSS 和 TLS 内存日志后，另行构建并发布更高版本（例如 0.1.3）测试 OTA，避免同版本被正常跳过。验收需要看到下载、校验、重启及新固件启动确认，编译通过不等于 OTA 实机通过。
