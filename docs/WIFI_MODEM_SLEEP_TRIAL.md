# Wi-Fi Modem sleep 实验配置

当前实验采用全局 `WIFI_PS_MIN_MODEM`，包含 S2/S4；最小活动时间为
15 ms，最大 AP 空闲保活时间为 60 s。音频分包、缓冲、CPU 频率、发射功率
及 MQTT/WSS 心跳保持原配置。

`main/network/network_lifecycle.c` 在驱动初始化时设置模式；`sdkconfig`
与 `sdkconfig.defaults` 同步设置两个计时参数。启动后日志应显示：

```text
Wi-Fi power save=MIN_MODEM min_active=15ms keepalive=60s
```

MIN_MODEM 仅允许通信空闲时关闭 RF/PHY，不停止麦克风采集和音频上传。
收发会刷新最小活动计时，持续流量下未必有明显节电。60 s 参数仅控制
长时间没有发送数据时的 AP 补充保活，不代表音频发送间隔或睡眠时长。

实机验证使用相同供电、亮度、音量及网络条件，对比原 NONE 版本与实验版：

- S5 待机平均电流、完整唤醒耗时及唤醒成功率。
- S2 回答首音、连续播放、打断耗时，以及上行积压和播放欠载。
- 弱信号下的卡顿、断线及连接恢复时间。

若需完整回退本次实验，将模式恢复为 `WIFI_PS_NONE`，日志同步更新，
并将两个配置文件中的最小活动时间、最大保活时间恢复为 50 ms、10 s，重新编译。
编译通过不代表实机功耗及交互指标通过。
