# capture-v1 云端配套修改（待部署）

> 最新状态：内容已同步到原云端目录，提交 `ce1cce1`，回退标签 `backup/pre-capture-v1-20260915`。尚未重启服务，后续由用户验证。见 [同步记录](../../docs/CAPTURE_CLOUD_SYNC_20260915.md)。下文保留本地制作阶段的说明。

本目录保存新接收器、补丁制作脚本、协议测试与线上参考源码指纹。线上服务尚未修改；部署必须等用户指令。

设计与完整链路见 [本地收音说明](../../docs/LOCAL_CAPTURE.md)。已有云端 API、人声复核、ASR、噪声判决和回答链路继续使用。未协商新协议的连接保留旧 PCM1 算法；新协议连接停止下发 MIC_START/MIC_STOP，以固件 start/end 为边界。

## 本地制作

```powershell
python cloud/capture-v1/prepare_patch.py build/local-capture-reference build/local-capture-work docs/patches/cloud-capture-v1.patch
```

参考和输出目录必须不同。脚本验证四个基线文件 SHA-256，只写输出副本和补丁，不修改参考或部署目录。参考源码未纳入 Git；它是本次只读提取的实际线上版本。

输出补丁为 [cloud-capture-v1.patch](../../docs/patches/cloud-capture-v1.patch)，涉及：

- `server/local_capture.py`：有界段接收、连续帧校验、明确段尾、异常超时。
- `server/wss_adapter.py`：能力协商、PCM2 路由和控制消息校验。
- `server/state_sync.py`：设备先进入 Think 时仍接收有效段；保留结果归属屏障；反馈带 session。
- `server/real_engine.py`：复用人声/识别/噪声/回答逻辑，返回段判决；新路径停止下发 MIC 起止。
- `engine/board_serial_asr_test.py`：协商后改用设备定义的段；旧调用保留原算法。

## 本地测试

安装到项目 build 下的依赖：NumPy、websockets。执行时设置 `PYTHONPATH` 指向该依赖目录，设置 `PYTHONUTF8=1`；后者用于原云端测试中未显式指定编码的读取操作。

```powershell
$env:PYTHONPATH = "$PWD/build/local-capture-deps"
$env:PYTHONUTF8 = '1'
python cloud/capture-v1/test_local_capture.py -v
python tests/host/test_capture_oracle.py --cloud build/local-capture-reference/engine/board_serial_asr_test.py --driver build/capture_oracle_driver.exe
python cloud/capture-v1/run_regressions.py build/local-capture-work
```

`run_regressions.py` 需要已有云端 tests 目录放在输出副本中。它显式使用 websockets 的 legacy API，匹配线上适配器使用的 `create_protocol` 接口；测试不修改部署依赖。

真正部署前仍需核对服务器最新源码指纹与配置、备份并执行 `git apply --check`，再按用户指令安排重启和设备联调。本目录没有自动部署或自动重启脚本。
