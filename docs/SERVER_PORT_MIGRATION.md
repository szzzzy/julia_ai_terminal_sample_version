# 开发服务器端口迁移（2026-09-16）

当前根目录 `sdkconfig` 和默认配置使用新版开发服务器：

| 链路 | 开发版 | 甲方旧测试服务器 |
| --- | --- | --- |
| WSS | `wss://8.133.215.254:19443/voice` | `wss://8.133.215.254:9443/voice` |
| HTTPS | `https://8.133.215.254:18443` | `https://8.133.215.254:8443` |
| MQTT TLS | `mqtts://8.133.215.254:11883` | `mqtts://8.133.215.254:1883` |

CA、TLS 校验选项、设备身份、凭据、主题、capture-v1、PCM2、动态底噪、收音起止与状态机均沿用原配置和实现。

## 配置与构建隔离

已有 `sdkconfig` 优先于 defaults，不能只编辑 defaults。脚本只修改五个服务器配置键，保留其余配置和凭据，不打印配置内容：

```powershell
python scripts/prepare_server_profile.py --profile development --input sdkconfig --output sdkconfig
idf.py -B build build

python scripts/prepare_server_profile.py --profile legacy --input sdkconfig --output build-server-legacy/sdkconfig
idf.py -B build-server-legacy -D SDKCONFIG=build-server-legacy/sdkconfig build
```

`sdkconfig.server-legacy.defaults` 是不含凭据的旧端口覆盖配置；适用于新建配置时放在 defaults 列表最后。已有配置应使用上述脚本。私有配置放在已忽略的 `build-server-legacy/`，不要提交或发布。两个目录的镜像应分别标注开发版和旧端口版。

旧端口配置只保留连接环境，不将当前 capture-v1 固件变为历史协议固件；甲方已交付的旧版二进制和历史源码应继续保留，不用当前开发版覆盖。

## 连接、下载与 NVS 检查

- WSS 首连、HTTP Upgrade Host 和断线重连均读取 `CONFIG_WSS_SERVER_HOST/PORT/PATH`。MQTT 首连、自动重连及客户端重建均使用 `CONFIG_COMM_MQTT_BROKER_URI`。没有从 NVS 读取服务器地址的覆盖路径。
- HTTPS 没有独立的固定基础 URL 配置，OTA 和音频 URL 来自服务器清单。OTA 使用 `ota_engine.c` 的 HTTP 客户端，音频使用公共 `http_downloader.c`；两个入口均处理旧端口，重试保持同一有效 URL。
- 开发版只将精确 authority `https://8.133.215.254:8443` 映射为 `https://8.133.215.254:18443`。路径、查询参数、其它主机和端口均保持原值；旧端口版设置 `CONFIG_JULIA_LEGACY_SERVER_PORTS=y`，不做映射。服务器仍应直接下发新端口清单，尤其签名绑定完整 URL 时须由服务器重新签发，不能修改签名校验规则。
- `ota_resume/record` 和 `audio_resume/record` 保存下载 URL，用于断点与制品匹配，不覆盖 WSS/MQTT 配置。迁移在建立 HTTPS 连接前定向执行，所以即便旧清单和旧断点仍存在，开发版也使用 18443；无需擦除 NVS。持久化记录格式与内容不变，保留制品身份、Range、ETag、大小及 SHA-256 校验。若新清单 URL 与旧记录不同，现有逻辑仅丢弃对应下载断点并从零下载，不清除其它用户设置。
- 音频资源下载代码参与编译，但当前产品主流程尚未接通 MQTT 音频清单触发。设备音频下载验证需要现有测试入口调用 `audio_engine_start()`；不能把 WSS 对话播放成功视为资源下载已通过。

## 设备端验证

烧录开发版应用并保留 NVS，不运行 `erase-flash`。记录测试前后非敏感配置；串口使用现有 INFO 日志，不开启会转储 HTTP 头、MQTT 密码或完整载荷的调试日志。服务器侧仅记录设备 ID、端口和结果，认证密钥、Authorization 及 URL 签名参数必须脱敏。

1. 启动联网，确认 `WSS connected & authenticated (wss://8.133.215.254:19443/voice)`，认证、状态同步、capture-v1 协商与 `capture_ready` 均成功。确认 PCM2 上传正常。
2. 确认 `MQTT_EVENT_CONNECTED` 和关键主题对应的 `MQTT_EVENT_SUBSCRIBED`/SUBACK，服务器确认 TLS 会话落在 11883，主题和设备身份保持不变。
3. 下发合法 OTA 清单及音频测试清单，分别验证原生 18443 URL 和遗留 8443 URL 最终连接 18443，HTTP 200/206、长度及 SHA-256 校验成功。中断下载后重启并再次下发同一清单，检查断点续传或安全回退从零下载。保留其它 NVS 设置。OTA 测试使用适配本设备且版本合法的测试镜像。
4. 执行唤醒、完整对话、结束收音，再关闭并恢复 Wi-Fi/服务端连接；确认重新认证、capture_ready、MQTT 重新订阅均恢复。服务器连接记录须只有 19443、11883，下载须为 18443，不回落旧端口。核对动态底噪、收音起止与状态机行为未变。
5. 旧版服务器用隔离的旧端口构建验证 9443/1883（TLS）/8443。不得把开发版音频协议兼容性等同于历史旧固件的兼容性。

编译和主机回归不能替代以上设备及服务端验收。

## 本次验证结果

- ESP-IDF 5.5.4 开发版完整编译通过，产物 `build/julia_fused_base.bin`，大小 `0x263fd0`，应用分区剩余 66%。
- SHA-256：`864837abd9dddb04bb3be5f5a04ce34bd11bee91ec3d3f0aafa3931acc6938ee`。
- 已核对生成的 `build/config/sdkconfig.h`：WSS 为 19443，MQTT 为 TLS 11883，旧端口开关关闭。
- `download_protocol` 主机测试通过：精确旧地址迁移、其它地址不变、查询参数保留、旧版不迁移、容量边界及原有 URL/响应头校验。
- 配置脚本往返切换及重复执行通过，全部非连接配置保持一致，检查未输出凭据。
- 未刷机；WSS 认证、capture_ready、MQTT TLS/SUBACK、HTTPS 实际下载和断线恢复仍待设备端验证。
