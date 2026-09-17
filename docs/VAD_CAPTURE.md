# 本地收音 VAD 接入（2026-09-17）

本次按用户明确选择：**能量、FFT、VAD一起参与起音与收尾**。VAD补充而非替换FFT。
只修改本地采集判定及构建接线，没有修改云端、IMU、业务状态图、传输协议，未烧录、提交或推送。
当前工作区另有启动初始化等未提交改动，本次保留。

## 实现

复用 vendored ESP-SR **1.9.4** 的传统 WebRTC VAD，采样率16kHz、PCM16、每帧320点/20ms。
只链接 `libesp_audio_processor.a` 所需目标文件，不初始化AFE、WakeNet、模型分区或神经网络运行时，不增加新任务。

`lc_vad.c` 使用 `vad_create/vad_destroy` 管理实例。该版本 `vad_create` 返回底层WebRTC实例，适配器使用已核对的 `WebRtcVad_Init/set_mode/Process` 符号进行无分配重置并保留错误返回。
之所以不直接调用 `vad_process`，是该版本包装器将任意非零结果（包括−1错误）转成人声；本实现将运行错误作为失败处理。升级ESP-SR版本时必须重新核对这部分ABI。

实例只在采集存储初始化时分配一次；采音线程独占运行和重置，逐帧不分配、不打印日志、不修改PCM。VAD接收**所有有效监听帧**，包括能量低或FFT不通过的帧，维持噪声统计。
LC_OFF不处理；重新进入监听、模式切换、连接换代或超过100ms的采音间隙后，在采音线程下一帧前重置。自然段间继续保留噪声适应状态。

- 起音：原能量门槛（wake底噪+3dB、dialog底噪+9dB）以及启用的FFT、VAD共同通过，才进入原起音累计窗；wake仍需1帧，dialog仍需300ms窗内6帧。
- 段内：能量>冻结底噪+3dB且启用的分类器通过才抵扣静音。普通对话通过帧抵扣40ms，唤醒抵扣80ms；不通过帧累计20ms。
- 结束：默认仍为wake累计500ms、dialog累计700ms；没有因本次接入改成“单帧否决”或“每个通过帧把静音全清零”。
- 单段上限仍为wake400帧、dialog750帧。预录仍为25帧/500ms。
- 所有段内PCM先发送再作正常尾部判断；尾部不作回溯裁剪，PCM2帧数、索引和顺序保持。

## 参数和回退

| 配置 | 当前默认 | 说明 |
|---|---:|---|
| `CONFIG_JULIA_CAPTURE_VAD_ENABLE` | y | 新增VAD总开关 |
| `CONFIG_JULIA_CAPTURE_FFT_GATE` | y（sdkconfig.defaults） | 保留FFT条件，可独立关闭做对照 |
| `CONFIG_JULIA_CAPTURE_VAD_MODE` | 1 | 范围0–3，越大越严格；候选起点，未实机标定 |
| `CONFIG_JULIA_CAPTURE_VAD_WAKE_TAIL_MS` | 500 | 静音累计门限 |
| `CONFIG_JULIA_CAPTURE_VAD_DIALOG_TAIL_MS` | 700 | 静音累计门限 |

尾部参数向上取整到20ms。VAD关闭时完全使用原有固定500/700ms门限及原能量/FFT逻辑。
这里只指定外层累计时间，**不能保证实际停说到结束恰好500/700ms**：VAD内部尾部保持、误判、采音/网络积压也会影响延迟。

初始化失败会明确打印错误并保留原有能量/FFT路径，不阻塞整机启动。运行期间检测/重置失败沿用采集失败路径结束当前连接，不把错误伪装成人声或静默丢音。
启动成功打印 `gate=energy+fft(1)+webrtc mode=1 frame_ms=20 tail_wake=500 tail_dialog=700 init_us=...`，用于确认实际运行配置及初始化耗时。

## 验证范围

最终清理重建后 **45/45 CTest通过**；VAD关闭/开启两种产品固件均构建成功，最终产物开启FFT+VAD，IMU实验模式关闭。
开启版大小 `0x26c720`，关闭对照 `0x26a2c0`，本工作区两种构建相差9,312字节（约9.1KiB）。此差值是Flash产物大小，不是运行RAM或启动耗时。
最终产物 `build/julia_fused_base.bin`；测试、两种构建日志和配置/哈希摘要在 `build/vad-validation/`。

主机新增可控后端测试：能量拒绝、FFT拒绝、VAD拒绝、三项均通过、低能帧仍喂VAD；预录/段内逐字节与索引检查；段尾、短暂停顿、400/750帧上限、再次起音、模式切换、关闭暂停、检测器重置及运行错误、发送失败。
capture-v1 owner测试增加VAD启用和初始化失败回退两种路径，检查PCM2、冻结底噪、判决回填与连接换代。

**主机使用可控VAD后端测试适配器与集成，不代表运行了Xtensa预编译VAD，也不证明人声检测准确率。** ESP32-S3产品构建和链接映射用于确认实际传统VAD已接入。
此前365条人声的97.72%是FFT参考结果，不能作为本次三门组合通过率。

开关对照方式：先保持FFT开启，只切VAD总开关。分别记录实际停说→端侧END、端侧END→服务器END、句尾漏音、短句/轻声误拒绝，以及MIC积压。
本次未自动烧录，因此目标板启动增量、每帧P50/P95/最大耗时、误截和实际收尾改善仍待实机测量。

## 文件

- `components/julia_board_audio/lc_vad.c`、`include/lc_vad.h`：VAD适配。
- `local_capture.c`、`include/local_capture.h`：分类器接口、起音/维持接线。
- `main/voice/capture/voice_local_capture.c`：一次初始化、采音所有权、换代/间隙重置、启动日志。
- 两个组件CMake、`main/Kconfig.projbuild`、`sdkconfig.defaults`：最小链接和配置。
- `tests/host/test_vad_capture.c`、`vad_test_backend.c`、`local_capture_stubs/esp_vad.h`、owner测试与CMake：主机回归。
