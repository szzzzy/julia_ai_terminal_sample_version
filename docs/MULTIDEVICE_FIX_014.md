# Julia 0.1.4 两端修正与验证交付

已完成代码修正及当前条件下可执行的测试。按用户要求，真实双端联调与真机验收跳过。`0.1.4` 是本次候选构建版本：没有烧录设备、上传 OTA、部署服务或切换正式环境。

## 代码位置与版本

- 固件：当前工作区，应用版本由 0.1.3 增至 **0.1.4**；项目名和分区保持不变。
- 云端：独立目录 `/opt/julia-multidevice-contract-fix`，分支 `codex/multidevice-contract-fix`，基于 `a68e41b5fff5a35be61c0295159d0cad89586ddf` 的工作区修改。原 `/opt/julia` 和 `/opt/julia-multidevice-20260910` 未改动。
- 完整云端变更保存为 [可审阅补丁](patches/cloud-control-v1-a68e41b.patch)，包括新模块、测试与协议，共 9 个文件。已在干净基线副本执行 `git apply --check`；补丁 SHA-256：`d90a80afe0b153250019095cb2c219d24b62bb7ebf643a5f2ab56a804b2e25ab`。云端工作区尚未提交或推送。
- 两端协议以 [control_protocol=1](MULTIDEVICE_CONTROL_V1.md) 为准。历史兼容性报告保留问题复现，不再代表修正后的状态。

## 六项差异的修正

| 原问题 | 当前实现 |
| --- | --- |
| 晚安/拒绝 ID 被覆盖为空 | 云端将设备确认的交互 ID 写回当前引擎；ScopedMqtt 保留当前有效引擎的 ID，不以空的旧会话值覆盖 |
| 播放态 MIC_START 被拒绝 | 分离 MIC_START/MIC_STOP 准入；允许 S2.3 合法打断，屏蔽随后旧引擎 PCM。快速 S2.1 回执只取消旧音频任务，不取消促成迁移的 MIC_START |
| 第 33 条控制耗尽缓存 | 每轮 control_seq 高水位加最近 8 项结果缓存；淘汰结果不降低高水位，旧序号永不重新执行；新确认轮次安全回收 |
| busy 无恢复路径 | 云端冷却到期后允许新实时输入重新申请资源，不要求伪造 revision。固件通过真实 EVT_VOICE_BUSY 结束活动交互，等待后从空 ring 恢复；S3/S5/S6 保持原状态 |
| 迟到旧轮 MQTT 可通过 | WSS interaction_sync/ACK 屏障确认 interaction_seq 和交互 ID；每次实际话语使用新轮次，MQTT 到执行时再次校验；加入固件单调时钟有效期 |
| 执行 ACK 未闭合 | 本设备 vstatus 返回 control_ack 和原 request_id/序号；云端单独等待执行结果，PUBACK 不算成功；重复请求返回缓存原结果 |

额外检查了首个唤醒前会话范围 FILE_SEND、旧 busy 通知、终止提示期间的保护、ACK 丢失、满队列旧请求、MQTT PUBACK 等待取消后的资源回收。认证、同步超时、会话替换的重试冷却分开处理，普通网络快速断线检测保持原实现。

没有改动 PCM1/裸 PCM 编码、上行 16kHz、SPKS 16/24kHz、播放预缓冲、SPKE 排空与 DMA 收尾策略。control_ack 的 accepted 表示业务命令已应用（例如开始本地晚安提示），**不表示声音已经播放完毕**；完成继续由原播放/FSM 状态链路体现。

## 测试结果

| 验证 | 最终结果 | 边界 |
| --- | --- | --- |
| 云端完整 unittest 模拟回归 | **96/96 通过**，160.229 秒 | 使用 mock/本机回环接口，没有真实设备或外部付费推理 API |
| 固件主机 CTest，含跨端模拟 | **31/31 通过** | 当前 C 控制/解析/FSM/播放模块或函数；RTOS、扬声器、外设由替身提供 |
| 实际 C/Python 控制函数互通 | 通过 | A/B 两个独立固件模拟进程；握手/发送器/屏障/去重/执行 ACK 使用源码实现，硬件动作是 fixture |
| 固件普通模式构建 | 成功，0.1.4 | 原单设备配置，不用于新严格云端 |
| 固件严格模式构建 | 成功，0.1.4 | 配置凭证为空的审阅镜像，不是可直接联网的设备配置 |

跨端模拟覆盖：首次晚安正确执行；执行 ACK 丢一次后原消息重试、只执行一次；同轮 **50 条控制**、随后 **200 个新轮次**；旧轮、旧 session、错误设备消息拒绝；A busy 不改变模拟 B 的播放状态；结束时 pending control 为 0。底层 guard 测试还执行了 **10000+ 条控制**，检查缓存容量与序号回收。

完整云端回归中原有静默 PONG 断线测试仍通过，记录约 9.04 秒检测；这只是该模拟网络场景的结果，不是所有真实网络的硬时限。

证据：

- [固件 31 项结果](../build/control-v1-ctest.log)
- [云端 96 项结果](../build/cloud-contract-fix/final-regression.log)
- [跨端模拟结果](../build-host-multidevice/cross-control/result.json)
- [构建、补丁和源码摘要](control-v1-evidence.json)

## 镜像与配置

严格候选位于 [build-multidevice/julia_fused_base.bin](../build-multidevice/julia_fused_base.bin)，常规构建位于 `build/julia_fused_base.bin`。最终大小和 SHA-256 见证据 JSON。esptool 验证项目 `julia_fused_base`、版本 `0.1.4`、ESP-IDF `v5.5.4`，镜像 checksum/validation hash 有效，未超过 7 MiB 应用分区。

严格镜像的 WSS token、MQTT 设备用户名/密码和 Wi-Fi 凭证均为空；已检查生成配置。缺失凭证会按设计拒绝服务，不回退共享身份。常规镜像可能含原有私有编译配置，仅留本机，不能直接公开分发。

[配置示例](multidevice.sdkconfig.example) 不含真实凭证；[生成严格审阅配置的脚本](../scripts/prepare_multidevice_build.py) 只生成独立构建配置，不操作设备。实际接入时仍需在私有配置中为每台设备填写独立凭证，并使云端账号/token 绑定 eFuse 派生 ID。

## 复现

在原 ESP-IDF/主机编译环境中，为 CTest 额外指定云端 api_server 源码目录：

```powershell
cmake -S tests/host -B build-host-multidevice `
  -DJULIA_CLOUD_SOURCE_DIR=D:/Espressif/projects/julia-fused-base/build/cloud-contract-fix/api_server
cmake --build build-host-multidevice --clean-first
ctest --test-dir build-host-multidevice --output-on-failure
```

其余 CMake 编译器、cJSON/IDF 和 Python 路径沿用 [前次构建记录](MULTIDEVICE_VALIDATION_20260910.md)。不指定 JULIA_CLOUD_SOURCE_DIR 时是 30 项，不会自动连接云端服务。

云端独立修复目录的命令：

```sh
cd /opt/julia-multidevice-contract-fix/api_server
PYTHONPATH=server:tests /opt/julia/api_server/.venv/bin/python -m unittest discover -s tests -v
```

构建命令与路径见前次记录；本次仅对确切 SDK/仓库路径设置进程级 Git 信任、使用既有 ROM ELF 目录，解决了前次 `HEAD-HASH-NOTFOUND` 元数据问题，没有修改全局 Git 配置或下载替换依赖。

## 未验证与回退

- 已按用户要求跳过真实双端：真实采音/播放、DMA 出声时刻、供电、网络抖动、长期 FreeRTOS/PSRAM 运行，以及真实 ASR/LLM/TTS 效果均未验证。模拟中的播放标志和固定缓存检查不能代替这些结论。
- 新严格固件和云端必须配套 control_protocol=1；旧严格候选 a68e41b 不能与新严格固件混用，双方会拒绝不支持的扩展。旧单设备模式仍仅用于独立隔离环境。
- 原独立 MQTT 管理 CLI 不能直接绕过活动 DeviceSession 发新严格控制；需要通过拥有当前 WSS 会话的控制入口。
- 去重仅保留 8 条详细结果，更老请求按序号拒绝为 stale_request，不提供无限期历史结果；发送端不得改序号复用旧 ID。
- 未更改 NVS/eFuse/分区格式。代码回退可从本地差异和云端补丁恢复；实际设备回退须在日后批准的维护窗口使用已验证旧镜像及私有配置，不执行 erase-flash。新严格模式不能自动降级到共享身份。

当前无需真实设备即可审阅代码和重跑上述测试；没有待用户批准的烧录或上线操作。
