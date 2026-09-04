# 音频主机回归测试

这组测试直接编译设备使用的 `pcm_buffer.c` 和 `voice_playback.c`。播放测试只替换 RTOS 同步／任务、I2S 和时间接口，用确定性调度在写入中注入取消、断流和错误。

## 测试组织

```text
tests/host/
├─ CMakeLists.txt          独立主机工程，不加入固件 main 组件
├─ test_pcm_buffer.c      FIFO 边界、尾部、重置与数据顺序
├─ test_voice_playback.c  实际播放控制代码的确定性场景
├─ test_julia_fsm.c       行为状态迁移图与事件映射
├─ test_transport_disconnect_fsm.c WSS／MQTT断联进入 S7.1、重复事件忽略及提示结束回 S3
├─ test_wss_tx_writer.c   TLS 部分写、暂时错误重试与绝对截止时间
├─ test_voice_uplink_ring.c PSRAM上行ring顺序、回绕、容量与连接代次
├─ test_voice_uplink_pump.c 正常发送、积压追赶、时间预算与失败保留
├─ test_voice_session_recovery.c overflow请求、owner清理与新连接隔离
├─ test_display_ownership.c S6显示硬件所有权与旁路点亮回归检查
└─ stubs/                 仅供测试的 ESP 错误码、时间、内存与 RTOS 接口
```

九个测试程序分别注册为独立 CTest 目标。stub 不实现真实 FreeRTOS 调度、PSRAM硬件和 I2S DMA，禁止加入固件的全局头文件搜索路径。

## 执行方法

在具有原生 C99 编译器的环境执行：

```powershell
cmake -S tests/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

可用 GCC、Clang 或 TinyCC；Windows 使用 Ninja 时可显式指定 `-G Ninja`、`-DCMAKE_C_COMPILER=<编译器路径>` 和 `-DCMAKE_MAKE_PROGRAM=<ninja路径>`。编译工具只用于主机测试，不替代固件的 Xtensa 工具链。

主机测试保持独立的 `build-host/` 缓存，不复用固件的 `build/` 或 `build-ota-name/`。测试使用断言检查结果，测试源码保证断言不受 Release 的 NDEBUG 选项禁用。

## 覆盖范围

覆盖内容：

- 环形缓冲绕回、PCM16 对齐、满缓冲拒绝、END 保留尾部、取消清空及变长分包一致性。
- 不足预缓冲目标的短音频收到 END 后立即播放，并排空全部已接收数据。
- I2S 写入期间打断／重开，旧缓冲与旧完成事件不会作用于新播放。
- 预缓冲等待有界、1 秒无输入后仍可继续、15 秒空缓冲无输入时明确超时。
- 溢出终止、I2S 错误终止、异步自检结束。
- TLS 单次／部分写入，WANT_READ、WANT_WRITE、EAGAIN 的同区间重试，
  绝对截止时间以及永久错误退出。
- 上行ring的FIFO、回绕、满缓冲、消费确认、会话停止，以及旧 connection
  generation 数据不得进入新连接。
- 上行pump的正常／追赶批次、8ms运行预算、恢复统计和发送失败不提前消费。
- ring overflow 后 producer 只关闭入口，owner 才能清理；新连接只发送新代次PCM。
- S6 面板／背光只由 FSM 呈现控制；idle、motion、night 和 WSS 断链不能旁路点亮。

模拟写入不是物理扬声器输出；测试不能证明 DMA 排空时间、音质、多核最坏调度延迟或开机速度。这些项目仍按 [设备验收](../../docs/VALIDATION.md) 上板执行。
