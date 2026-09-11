# Julia 多设备阶段交付与验证（2026-09-10）

完成范围按本次追加指示“先改确定的部分”。这是一份可审阅的阶段版本，**不是完整多设备正式验收通过**。未烧录设备、未修改云端源码、未部署或切换服务。

## 代码与协议

固件基线 `b27969a`，修改保留在工作区供审阅；应用版本仍为 `0.1.3`，不能据此直接发布普通 OTA（正式发布前需要独立递增版本和验收）。

| 模块 | 本次变化 |
| --- | --- |
| voice_state_sync | 复用原 eFuse ID；保留随机 session 规则、v2 快照、FSM/revision 和 ACK 期限；严格模式补齐云端身份封装及 ACK request 匹配 |
| mqtt_comm / voice_service | 严格模式只订阅设备 vcmd；实际执行时校验设备、会话、交互；保留旧业务执行函数；设备 vstatus 镜像、有限 outbox 准入 |
| voice_control_guard | 768 字节、8 层 JSON 上限；身份字段校验、去重、容量拒绝；固定 32 项，不淘汰以防旧请求重执行 |
| wss_transport | 空/非法 token 在 TLS 前拒绝；严格模式禁用旧 token 回退；HTTP 401/403、WS 4401 最少 60 秒重试；不输出 HTTP 响应头 |
| voice_playback / voice_service | 增加固定槽时间点与 WSS owner 日志；未修改预缓冲、PCM 格式、采样率、SPKE 排空、I2S drain、FSM 业务转换 |
| 配置 | 默认不开启严格模式；独立构建配置清空 Wi-Fi/认证字符串，不修改在用设备配置 |

协议、确切消息示例、已读云端代码与差异见 [MULTIDEVICE_CONTRACT.md](MULTIDEVICE_CONTRACT.md)。私有设备配置参考 [multidevice.sdkconfig.example](multidevice.sdkconfig.example)，样例凭证全部为空，直接使用时按设计拒绝服务。

## 自动化结果

最终独立目录 `build-host-multidevice`：**30/30 CTest 通过，0 失败**，最后运行 2.96 秒。使用本机 TinyCC、ESP-IDF 内置 cJSON/http_parser 和 Python 3.13 环境，不访问在线设备或业务接口。

- `voice_control_guard`：模拟 A/B 状态独立、旧 session/interaction、重复请求、容量满后不淘汰、不随 10000 次拒绝增长、缺失/重复字段、嵌套深度及 NUL。
- `multidevice_routing`：提取当前 MQTT 接收和 owner 分派函数，验证入队无立即副作用、排队后换 session/interaction 被拒绝、晚安重复仅执行一次、跨设备拒绝、会话范围文件命令、队列压力。还编译实际凭证选择函数，验证 NONE/TOKEN/用户名密码缺失均拒绝，旧配置不能兜底。
- `voice_state_sync` / `voice_state_sync_strict`：ID 稳定、重连 session 改变、旧 ACK/版本/request/设备拒绝、同步有界重试、require_wake 幂等及 S6 保持。
- 原播放、PCM 缓冲、上行 ring/pump、连接恢复、TLS 部分写/FSM/网络故障等回归全部通过。播放新增检查首 I2S 时间、完成时间、失败未标记开播及旧 generation 查询拒绝。

结果文件：`build/host-multidevice-tests.log`、`build/host-multidevice-build.log`、`build-host-multidevice/Testing/Temporary/LastTest.log`。

复现（已安装 SDK 与原生 C 编译器的终端）：

```powershell
cmake -S tests/host -B build-host-multidevice -G Ninja `
  -DCMAKE_C_COMPILER=D:/Espressif/projects/julia-fused-base/build-ota-name/host-tools/tcc/tcc.exe `
  -DCMAKE_MAKE_PROGRAM=D:/Espressif/tools/ninja/1.12.1/ninja.exe `
  -DIDF_PATH=D:/Espressif/v5.5.4/esp-idf `
  -DPython3_EXECUTABLE=D:/Espressif/python_env/idf5.5_py3.13_env/Scripts/python.exe
cmake --build build-host-multidevice --clean-first
ctest --test-dir build-host-multidevice --output-on-failure
```

最初复用旧 build-host 时发现 TinyCC 未追踪头文件依赖，出现旧对象/缺失 stub 错误；已补齐 stub 并从独立目录完整编译，以上结果来自最终源码，不计旧目录失败输出为通过。

## 固件构建

ESP32-S3，SDK 路径 `D:/Espressif/v5.5.4/esp-idf`，Xtensa GCC 14.2.0。两种配置均构建成功并通过分区大小检查。

| 配置 | 文件 | 字节 | SHA-256 |
| --- | --- | ---: | --- |
| 原配置/严格模式关闭 | `build/julia_fused_base.bin` | 2542288 | `71865098a90bdb83ae72d117fac6b9e5ca2339a31c127d1e4e2deb3a3cbf0a1a` |
| 严格模式/空凭证审阅镜像 | `build-multidevice/julia_fused_base.bin` | 2543392 | `5bd2156867f826f84e89a9639be128d0b5905c503aaa2d6ba2afdd52a9f51da0` |

严格模式已核对生成头文件的 MULTI_DEVICE_ENABLE、CLOUD_STATE_SYNC_ENABLE 和 USERNAME_PASSWORD 均为 1。7 MiB 应用分区尚余约 65%。esptool 核对项目名 `julia_fused_base`、版本 `0.1.3`、ESP32-S3、checksum 和 validation hash 有效。因构建账户的 SDK Git ownership 检查，镜像 ESP-IDF 描述显示 `HEAD-HASH-NOTFOUND`；构建源码路径已记录，此元数据应在正式发布环境重新生成。原配置镜像可能含原有私有编译配置，仅留本机，不作为可公开分享文件。

构建日志：`build/build-final.log`、`build/build-multidevice-final.log`；完整严格构建日志 `build/multidevice-strict-build.log`。首次新目录未自动定位 Ninja，已按仓库文档显式配置工具路径后构建成功，没有下载或替换依赖。

严格模式复现（先激活既有 ESP-IDF 环境）：

```powershell
python scripts/prepare_multidevice_build.py
idf.py -B build-multidevice `
  -DSDKCONFIG=D:/Espressif/projects/julia-fused-base/build/multidevice.sdkconfig `
  -DCMAKE_MAKE_PROGRAM=D:/Espressif/tools/ninja/1.12.1/ninja.exe `
  -DCMAKE_PROGRAM_PATH=D:/Espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin `
  -DCCACHE_ENABLE=0 build
```

## 未验证、兼容限制与下一步

1. **硬件未验证**：A/B 同时真实对话、拔网/电源、A 晚安而 B 持续播放、真实插话、长回答尾音、DMA 实际出声时间、200 轮堆/任务稳定性。模拟测试不等价于这些验收。
2. **协议未闭合**：云端未定义控制执行 ACK、首次到达但已过期的 MQTT 消息判定、每次话语/打断的新 interaction_id。普通音频仍裸传，云端负责取消旧发送任务。当前沿用 wake ID，不能过滤同 wake ID 内所有旧轮控制。
3. **去重阶段上限**：每个 WSS session 32 个已接纳控制，满后拒绝新控制，不自动清空或强制重连。长期业务使用前必须双方确定可安全回收的窗口或确认机制。
4. **恢复待对齐**：busy/retry_after、同步超时和 session_replaced 尚未新增约定的调度策略；重复凭证导致的会话争夺尚未解决。已实现认证拒绝冷却，普通网络重连保持原机制。
5. **状态与 ACK 区别**：vstatus 只是尽力发送的状态镜像。没有虚构通用执行 ACK；不能以镜像/PUBACK 当作命令成功或扬声器完成。
6. **云端快照非固定发布**：只读检查了 `/opt/julia-multidevice-20260910` 未提交工作区，读取到 10 项云端测试 OK。未启动其服务，也未运行固件到该云端的端到端联调。云端继续修改后须重新核对。

双设备逐项操作清单见协议说明末尾。确认上述待定项并给出更新后的可审阅版本和验证结果后，再申请烧录在用设备或切换测试环境。

回退：保持默认严格开关关闭即可构建原主题模式；该模式只可用于原隔离单设备环境。身份算法和 NVS 格式未变，无迁移写入。若未来已经烧录，应先批准维护窗口，再恢复预先保存的已验证旧镜像与私有配置；不执行 erase-flash、不擦 NVS/eFuse。不要把本次同版本阶段镜像当作自动 OTA 升级或正式回退包。
