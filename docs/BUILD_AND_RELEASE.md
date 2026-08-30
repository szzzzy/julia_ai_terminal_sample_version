# 构建与发布

文档版本：V1.0。返回 [项目入口](../README.md)。

## 1. 环境与依赖

当前工程基线为 ESP-IDF 5.5.4、ESP32-S3、16MiB Flash 和 Octal PSRAM。`dependencies.lock` 记录依赖，`components/` 包含 LVGL、ESP-SR、ESP-DSP 和板级音频。

根目录 [CMakeLists.txt](../CMakeLists.txt) 为 bootloader 子构建显式指定了 Windows Ninja 和 Xtensa 工具路径：

```text
D:/Espressif/tools/ninja/1.12.1/ninja.exe
D:/Espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin
```

在其他机器构建前，应调整这两处路径。`main/idf_component.yml` 通过 `${IDF_PATH}` 引用 SDK 内的 `protocol_examples_common`。不要依赖另一份工程的构建目录来补齐依赖。

## 2. 配置来源

| 文件 | 职责 |
| --- | --- |
| `sdkconfig` | 当前工程的实际选择；已有值不会简单被 defaults 覆盖 |
| `sdkconfig.defaults` | 生成配置时的项目默认值 |
| `main/Kconfig.projbuild` | 应用配置项、类型、帮助及默认值 |
| `CMakeLists.txt` | 工程名、应用版本、工具路径和镜像名一致性检查 |
| `partitions_16mb.csv` | Flash 分区定义 |

在已激活的 ESP-IDF 5.5.4 终端中运行：

```powershell
idf.py -B build menuconfig
idf.py -B build build
```

现有 `sdkconfig` 已选择 `esp32s3`；不要为例行构建运行会重置目标配置的操作。`sdkconfig.defaults.esp32h2` 中的 OpenThread 配置不代表本项目支持 ESP32-H2。

重点核对以下配置，不要在日志、文档或发布包中记录真实凭据：

| 配置 | 要求 |
| --- | --- |
| `CONFIG_EXAMPLE_WIFI_SSID`／`CONFIG_EXAMPLE_WIFI_PASSWORD` | 本机受控测试网络 |
| `CONFIG_COMM_MQTT_BROKER_URI` | MQTT 地址；`mqtt://` 为明文，`mqtts://` 才是 TLS |
| `CONFIG_COMM_DEVICE_AUTH_*` | MQTT 设备认证方式；默认 NONE 仅适合受控开发 |
| `CONFIG_WSS_SERVER_HOST`／`PORT`／`PATH` | WSS 主机、端口、路径，端口 9443、路径 `/voice` |
| `CONFIG_COMM_DEVICE_AUTH_TOKEN_VALUE`／`CONFIG_WSS_TOKEN` | WSS 优先使用前者，空值时使用后者，与 MQTT 认证模式分开理解 |
| `CONFIG_OTA_ALLOWED_URL_HOSTS` | OTA HTTPS 主机允许列表；空值会放行非空主机，不是默认白名单 |
| `CONFIG_JULIA_SERVER_WAKE_ENABLE` | 默认启用服务器唤醒；本地唤醒需要另行构建和验证模型分区 |

## 3. Windows 构建路径

在 `D:\Espressif\projects\julia-fused-base`、已激活 SDK 的 PowerShell 中，如果 CMake 无法定位工具，可显式指定：

```powershell
idf.py -B build `
  -D CMAKE_MAKE_PROGRAM=D:/Espressif/tools/ninja/1.12.1/ninja.exe `
  -D CMAKE_PROGRAM_PATH=D:/Espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin `
  -D CCACHE_ENABLE=0 build
```

SDK 环境应指向 `D:\Espressif\v5.5.4\esp-idf`，Python 虚拟环境应与该 SDK 匹配。其他安装路径按实机调整，不通过禁用镜像校验解决工具链错误。

Git 报告仓库所有权不一致时，先确认目录归属和可信性，再仅对确切目录配置信任；不要使用全目录通配信任。

编译数据库由实际 CMake 构建生成，路径为 `build/compile_commands.json`。`scripts/gen_compile_commands.ps1` 依赖另一工程的数据库且只枚举 `main` 根层源码，不适合作为本项目的权威编译数据库生成流程。

## 4. 镜像标识与版本

| 名称 | 当前值 | 使用位置 |
| --- | --- | --- |
| 工程名／镜像 `project_name` | `julia_fused_base` | 根 CMake、ESP 应用描述符、OTA 镜像准入 |
| `CONFIG_OTA_IMAGE_PROJECT_NAME` | `julia_fused_base` | 与工程名一致；不一致时 CMake 报错 |
| `CONFIG_OTA_PRODUCT_ID` | `julia-ai-device` | OTA 请求与响应中的 `product` |
| `CONFIG_OTA_HARDWARE_VERSION` | `1.0` | OTA 硬件兼容性校验 |
| `PROJECT_VER` | `0.1.0` | 应用版本、OTA 当前版本和镜像内版本 |

版本必须为三段数字 `major.minor.patch`。发布清单中的 `version` 必须与待发布 `.bin` 的应用版本完全一致；普通升级要求目标版本高于设备版本。

`force_update=true` 仅放宽版本高低比较，不绕过产品、硬件、镜像名、摘要或有效期等校验。镜像名与运行固件的准入名称不一致时，设备会拒绝镜像；需要串口烧录或经过验证的兼容发布策略。

## 5. 产物检查

在已激活 SDK 的终端，检查实际产物，而不是从文件名推断镜像属性：

```powershell
python -m esptool --chip esp32s3 image_info build/julia_fused_base.bin --version 2
Get-Item -LiteralPath build/julia_fused_base.bin | Select-Object Length
Get-FileHash -LiteralPath build/julia_fused_base.bin -Algorithm SHA256
```

要求 `Project name`、`App version`、芯片类型正确，镜像内部校验有效且不超过目标 OTA 分区。清单 `image_size` 使用文件实际字节数，`sha256` 使用整个 `.bin` 的 SHA-256，不使用 ELF 摘要或镜像内部 validation hash。

## 6. 分区与烧录

| 分区 | 大小 | 用途 |
| --- | --- | --- |
| `nvs` | 64KiB | 配置、OTA 报告和恢复信息 |
| `otadata` | 8KiB | bootloader OTA 选择数据 |
| `phy_init` | 4KiB | PHY 数据 |
| `ota_0`／`ota_1` | 各 7MiB | 应用双分区 |
| `model` | 512KiB | 本地唤醒模型存储槽；服务器唤醒配置不代表已烧入模型 |
| `audio_data` | 1MiB | 音频素材存储槽；不等同于 `/sdcard` 或已挂载的 `/spiffs` |

分区偏移以构建生成的 `partition-table.bin` 和 `flash_args` 为准，不手工套用另一工程地址。

确认板卡、供电和串口后，可执行以下烧录命令。示例 `COM3` 必须替换为实际设备端口：

```powershell
idf.py -B build -p COM3 flash monitor
```

`flash` 会写入设备，`monitor` 会连接串口。不要把 `erase-flash` 当作常规升级步骤；完整擦除会破坏 NVS 和其他已存数据。退出监视器通常使用 `Ctrl+]`。

## 7. OTA 发布流程

1. 确认目标产品、硬件和版本，构建发布配置并保存精确配置与源码标识。
2. 检查镜像名、版本、大小、SHA-256、安全版本和分区容量。
3. 在批准的 HTTPS 主机提供不可变版本 URL；为清单设置合理的未来过期时间。
4. 在设备发出 `ota_check` 后，按 [协议](PROTOCOL.md) 回显身份与 request_id；通知只促使设备检查，不直接携带固件。
5. 按 event_id 去重处理状态，观察启动确认或回滚结果；不能以 MQTT 发布成功代替安装成功。
6. 执行 [发布验收](VALIDATION.md)，保存故障注入、硬件运行和版本追踪记录。

## 8. 发布限制

- 当前 `CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS` 未在生效配置中定义，所用 SDK 的 HTTP client Kconfig／头文件也未提供代码引用的开关／取头接口。仅添加同名配置不能形成已验证的续传支持；206 恢复分支需要适配，当前可能返回 `RANGE_MISMATCH`。
- 电源与业务状态检查钩子默认通过；产品启动验收钩子也默认通过，且业务外设初始化发生在 OTA 启动流程之后。
- 当前未启用安全启动、Flash 加密和强制签名镜像；WSS 跳过服务器名称校验。生产启用安全功能前应制定密钥保管和恢复方案，eFuse 操作不得作为普通构建步骤执行。
- 开发 Wi-Fi／认证信息可能被编译进镜像；分发固件前必须审查配置与凭据。
- `build-ota-name` 是独立构建目录，不是唯一发布目录。本文统一以 `build` 示范，产物名称由实际工程名决定。
