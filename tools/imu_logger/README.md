# 单动作 IMU 记录工具

目标只有：输入动作名称 → 电池供电做动作 → 保存 CSV → 自动出图。
设备和电脑连接同一可信局域网；电脑充当临时接收端，不使用现有云服务、SD 卡或文件系统。

## 已实现的流程

1. 电脑输入 `walk_01` 等动作名并回车。
2. 设备响一声，屏幕显示准备，等待 3 秒。
3. 屏幕显示 `RECORDING 8s`，连续采集 8 秒六轴数据到 RAM。
4. 停止采集，再响两声；两段提示音均不在记录时间内。
5. 设备上传本次 CSV，电脑落盘后才确认成功，然后生成 SVG 曲线和 JSON 摘要。
6. 屏幕显示 `SAVED / READY`，可以开始下一条记录。

同一时间只保留一条记录。上传失败显示 `UPLOAD FAILED`，使用 `:retry` 重传；成功前拒绝下一次采集，不自动覆盖。重传使用同一记录 ID，电脑不会重复生成 CSV。断电或复位会丢失尚未上传的数据。

## 构建实验固件

在已激活的 ESP-IDF PowerShell、项目根目录执行。使用独立配置和构建目录，保留产品的 `sdkconfig` 与 `build/`：

```powershell
# 第一次创建；已有实验配置时不要重复覆盖。
Copy-Item sdkconfig sdkconfig.imu-test
idf.py -B build-imu-test -D SDKCONFIG=sdkconfig.imu-test menuconfig
```

在 `Example Configuration` 启用 `Standalone IMU experiment firmware (no product voice/FSM)`。
Wi-Fi 参数沿用复制过来的配置。随后：

```powershell
idf.py -B build-imu-test -D SDKCONFIG=sdkconfig.imu-test build
# 手动选择实际设备端口；这一步才会烧录。
idf.py -B build-imu-test -D SDKCONFIG=sdkconfig.imu-test -p COM8 flash monitor
```

也可在没有既存配置时使用 `-D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.imu-test.defaults"` 创建实验配置。已经存在的配置优先于 defaults，因此需要确认开关实际为 `y`。

不要用根目录 `FLASH_NEW_FIRMWARE.ps1` 烧录本实验，它指向产品的 `build/`。
本工作站已有专用烧录入口，检查实验开关和现有目标设备 MAC 后才烧录：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\imu_logger\flash_imu_test.ps1 -CheckOnly
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\imu_logger\flash_imu_test.ps1 -Port COM8
```

第一条只检查、不连接设备；第二条手动执行才会烧录。`ExecutionPolicy Bypass` 只作用于该次 PowerShell 进程，不修改系统策略。

测试结束要恢复产品功能时，再使用根目录的产品烧录脚本。两个镜像共用板上固件分区，烧入实验固件后不会同时运行产品功能。
测试模式保留启动验收、电源、显示、提示音与 Wi-Fi，但不启动产品 FSM、IMU 唤醒、MQTT/WSS 对话或 OTA 服务。默认产品配置不启用它。

## 电脑端使用

从屏幕或串口 `IMU: <IP>:8080` 获取设备 IP。设备由电池供电后可以脱开 USB。
Python 3.9+，**无第三方依赖**：

```powershell
python tools/imu_logger/imu_capture.py --device 192.168.1.50
```

电脑监听 TCP 8765；设备监听 TCP 8080。允许 Python 在专用网络接收入站连接，两端必须能互相访问，不能处于热点的客户端隔离模式。此测试接口没有认证，仅用于可信局域网，不做公网映射。

输入动作名并回车开始，例如：

```text
walk_01     自然走动/挂着摆动
pickup_01   正常拿起/挪动
shake_01    明显主动摇晃
tap_01      轻碰/桌面振动
```

动作名只允许英文字母、数字、下划线、短横线，最长 48 字符。每类先做 5 次，人工改成 `_02` 等即可。
命令：`:status` 查看设备阶段；`:retry` 重传失败记录；`:quit` 退出电脑程序。上传失败后重启电脑脚本时，应使用原来的电脑 IP 和监听端口。

默认输出到 `imu_records/`：

```text
<记录ID>_walk_01.csv            原始计数、物理单位、时间戳及元数据
<记录ID>_walk_01.svg            浏览器直接打开：六轴 + accel_delta + gyro_norm
<记录ID>_walk_01.summary.json   实际采样率、最大间隔、峰值与质量提示
```

CSV 已保存但绘图失败不影响数据安全，可以单独重画：

```powershell
python tools/imu_logger/imu_capture.py --plot imu_records/<文件名>.csv
```

## 数据的含义和边界

- 实验配置固定为 ODR code 6、加速度 ±16 g、角速度 ±1024 °/s、LPF 关闭。QMI8658C Rev A 数据手册的六轴模式该档约 112.1 Hz；不同芯片版本可能略有差异，**不把轮询频率当成实际采样率**。该手册不支持 ±2048 °/s，不能继续盲目增大量程。
- 初版记录使用 ±8 g / ±512 °/s；CSV 中保留各自量程。电脑程序按元数据校验换算，旧记录仍可重画，不能按新量程重新解释旧原始计数。换量程只影响实验固件，产品驱动及唤醒阈值不变。
- 用 2 ms 定时通知读取传感器，RAM 只接收新的 24 位样本计数；读取前后计数变化的快照会丢弃。样本计数的跳变用于统计漏样，不保证轮询方式零漏样。
- `t_us` 是一次 I2C 快照读取开始/结束的 MCU 时间中点，相对本次采集开始；不是传感器内部精确采样时刻。`sensor_counter` 是传感器样本计数，不是微秒。
- 保存六轴 int16 原始计数及换算值。摘要显示读失败、计数跳跃、接近量程上限、缓冲满和实际速率不足；不要把带警告的记录当作完整有效数据。
- 最多 1200 个样本；满后停止，不覆盖。8 秒正常预期约 900 个新样本，以实测为准。
- `accel_delta` 为相邻采样三轴加速度差绝对值之和，单位 mg；`gyro_norm` 为三轴角速度模长，单位 °/s。
- 图中 750 mg / 90 °/s 仅为当前产品阈值参考。尤其 `accel_delta` 随采样间隔变化，**不能从高频曲线直接断言原来低频的 750 mg 阈值是否正确**。
- 采集期间不逐点打印、不上传 CSV；Wi-Fi 仍保持连接，实际调度影响由时间戳和漏样计数体现。量程/LPF 与产品模式不同，这一轮用于观察动作，不代表产品误触率验证。

寄存器依据：[QST QMI8658C Rev A](https://www.qstcorp.com/upload/pdf/202210/13-52-27%20QMI8658C%20Datasheet%20Rev%20A%20%281%29.pdf)，CTRL2/3/5 和 TIMESTAMP 寄存器。产品运动参数仍在 `sdkconfig`，实验配置不会覆盖它们。

## 离线试参数，不用反复烧录

`tune_motion.py` 默认读取项目 `sdkconfig` 中的五个 IMU 参数，回放 `imu_records/latest/*.csv`。
以 walk 开头的动作标为不应触发，包含 hand 或 shake 的动作标为应触发；未知动作名会报错，避免静默猜测。

```powershell
# 进入交互模式，直接输入五个值
python tools/imu_logger/tune_motion.py
# 不提示输入，直接回放当前 sdkconfig 一次
python tools/imu_logger/tune_motion.py --once
# 只覆盖角速度和确认帧数，其余沿用 sdkconfig
python tools/imu_logger/tune_motion.py --gyro-dps 500 --confirm-frames 10
# 所有参数均可单独调整
python tools/imu_logger/tune_motion.py --sample-ms 10 --confirm-frames 10 --cooldown-ms 10000 --accel-mg 750 --gyro-dps 500
# 小范围扫描角速度/加速度门槛及确认帧数，输出全部组合
python tools/imu_logger/tune_motion.py --scan
# 只研究角速度分支：off 仅是分析开关，不是可写进 sdkconfig 的值
python tools/imu_logger/tune_motion.py --accel-mg off --gyro-dps 800 --confirm-frames 1
```

默认运行后，在提示符按这个顺序输入：

```text
采样间隔(ms) 连续次数 冷却时间(ms) 加速度阈值(mg) 角速度阈值(dps)
10 10 10000 750 500
```

回车立即计算，之后可以继续输入下一组。直接回车沿用当前五个值，`-` 保留对应值（例如 `- - - - 600` 只改角速度），输入 `q` 退出。输错数字会提示重新输入，不会退出或改写配置。`--interactive` 可以在提供命令行初始参数时继续交互。

结果默认在 `imu_records/tuning/replay.json` 和 `scan.csv`。可用 `--output` 分开保存不同试验，`--records` 选择其他记录文件夹，`--config` 选择配置文件。脚本不会修改配置、原始数据或固件。

终端输出解释：

- `expected`：期望是否触发；`trigger phases`：默认检查三个不同轮询起点，`3/3` 表示三次都触发，`0/3` 表示都不触发，其他结果表示对采样起点敏感。
- `first time(s)`：记录开始后的首次触发时间，不是动作开始后的延迟，因为尚未标注动作起点。
- `A/G hits`：第一个轮询起点下，首次触发时最后 N 个点中加速度/角速度各自达标多少次；二者可能重叠，默认不允许交替凑数。
- 默认新规则要求 **A 或 G 独立连续达到 N 次**。N=10 时，成功窗口必须出现 `10/x` 或 `x/10`；`6/9` 不再通过。这只是离线候选规则，尚未修改产品固件。
- `False-trigger records`：至少一种轮询起点误触发的 walk 记录数；`missed records`：至少一种起点漏掉的主动动作记录数。

默认回放将 A、G 分别连续计数，各自不达标就清零，任一路达到确认次数即可触发。加 `--combined-or` 可以对照原固件规则：每个点只要 A 或 G 达标，就累计到同一个连续计数器，允许两路交替凑数。输出 JSON 和扫描 CSV 都标注使用的规则。

两种模式均保留首次基线、冷却和数据缺口处理。按设定周期取当时最近的已知样本，然后**重新计算相邻加速度差**；不会直接抽取已经算好的 delta。轮询快于记录更新时，重复值仍会参与判断，和当前产品轮询寄存器的方式一致。遇到记录计数缺口会重建基线。

`COOLDOWN_MS` 只影响已经触发后的重复检测，不会阻止第一次误触发。默认在第一次触发后停止，模拟进入 S4 离开运动监测；加 `--continuous` 可以假设一直留在监测状态来检查冷却，但这不模拟真实 FSM。

这是记录数据上的理想周期轮询模型，不模拟 I2C 耗时、RTOS 抖动、播放抑制、开机稳定等待或改变传感器 ODR/滤波器的效果。产品原有 ±64°/s 量程不能使用 500/800°/s 这样的高门槛；正式部署须匹配实验硬件配置。扫描通过只表示这批记录能分开，不是独立样本验证或零误触保证。不要把轮询周期拉长来代替连续确认时间：它会同时改变采样信息和 accel_delta。
