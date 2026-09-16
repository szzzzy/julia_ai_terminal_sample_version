# 当前云端与固件协议核对

2026-09-15。经 SSH 只读访问固件配置指向的服务器 `8.133.215.254`。

## 结论与边界

当前固件 capture-v1 与服务器当前源码、实际启动配置在本次检查的握手、音频报文、
轮次同步、唤醒和播放入口上匹配。未发现需要继续修改的协议冲突。
检查时 9443 端口没有已建立的设备连接；这不代表已完成真实麦克风、模型与扬声器的端到端验收。
本次未修改服务器、重启服务或刷机。

## 运行对象

- 进程 PID：79448；启动时间：2026-09-15 16:56:01（服务器显示时间）。
- 工作目录：`/opt/julia-runtime/control-v1-20260910/release/api_server`。
- 实际配置参数：`/opt/julia-runtime/control-v1-20260910/config.json`。
  仓库内 `api_server/server/config.json` 不是本次进程指定的配置。
- WSS：9443、`/voice`、TLS、`require_device_identity=true`、`allow_legacy=false`。
- 四个核心协议源码修改时间均早于进程启动；当前有未提交修改，不能仅以提交 `ce1cce1` 代表当前版本。
- 下载的只读源码快照：`build/cloud-live-audit-20260915`；未下载配置或凭据。

## 双端检查

| 项目 | 结果 |
| --- | --- |
| session_sync 后 capture_hello/ready；session、version、stream_generation | 匹配 |
| capture_start/end/abort、wake/dialog、floor_dbfs、utterance_id、总帧数 | 匹配 |
| PCM2，16 kHz，20 ms，320 样本，16 字节头，656 字节报文 | 匹配 |
| 连续帧序号、小端字段、字节和校验、8 秒/15 秒段上限 | 匹配 |
| capture_dialog 在云端内部建立 interaction_sync，不下发旧 MIC 命令 | 匹配 |
| 固件普通轮次同步后直接放行；重复请求返回缓存 ACK | 匹配 |
| Think 先于 start/end 到达，或 end 后才到达 Think | 双端离线互通通过 |
| wake_detected 与独立唤醒轮次、SPKS 24000/SPKE 播放入口 | 双端离线互通通过 |

## 云端与旧总结的差别

- 当前已强制 capture-v1；未协商的上行被拒绝，PCM1 不再是可用语音兼容路径。
- 已删除 `engine/board_serial_asr_test.py`；实时 ASR 编排直接使用完整固件段。
- 当前普通对话路径保留一条 ASR 会话，最后不足 600 ms 的尾块在同一会话完成，
  整 600 ms 边界也会显式结束；没有旧的裁剪重识别和人声复核调用。
- 普通对话判决当前按识别是否为空返回 speech/empty；没有旧 noise 判决分支。
  固件仍能接收 noise，但本轮云端路径不产生它。
- 600 ms 流式 ASR 分块及文件式 TTS 等机制仍存在；不应将协议匹配理解为所有性能工作都已完成。

## 验证

在本地运行刚下载的云端源码，推理使用测试替身，未调用线上模型：

- `test_capture_v1.py`：13/13，包括解析固件生成的真实 wire 文件。
- `test_capture_only.py`：4/4，包括整句一次 ASR 会话与完整尾块。
- `test_voice_state_wait.py`：8/8，包括迟到状态、取消、超时和网络故障分类。
- `build/cloud-live-audit-20260915/cross_capture_audit.py`：通过。
  实际云端 DeviceSession/SessionControl 与当前固件 C 处理器交换消息；硬件和传输边界为替身。
  覆盖重复 ACK、无 MIC_START 播放、迟到旧命令、Think 两种先后顺序及唤醒播放。

固件产物为此前本任务构建的 `build/julia_fused_base.bin`，尚未确认设备已刷入此版本。
