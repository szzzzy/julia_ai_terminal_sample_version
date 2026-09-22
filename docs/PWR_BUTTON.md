# PWR 电池关机

当前板卡支持软件检测长按后切断电池供电。依据本机 `D:/Espressif/ESP32-S3-LCD-1.85.pdf` 和
[厂商原理图](https://files.waveshare.com/wiki/ESP32-S3-LCD-1.85/ESP32-S3-LCD-1.85.pdf)：
GPIO6 为 Key_BAT（R1 上拉，PWR 经 D1 拉低），GPIO7 为 BAT_Control（通过 T1 控制 Q1），
GPIO8 为 BAT_ADC。按键还会经 D4 维持电池供电，所以松手后才能可靠断电。
[厂商 FAQ](https://docs.waveshare.net/ESP32-S3-LCD-1.85/FAQ/) 也说明了软件长按关机方式。

固件在供电保持成功后启动独立按键任务，不依赖网络和 FSM 状态：

- 开机先等待一次稳定松手，持续按住开机键不会立即触发关机。
- 新一次按压持续至少 3 秒，松手后拉低 GPIO7；短按无动作。
- 按下和松开均消抖 50ms，按键任务每 10ms（至少一个 RTOS tick）采样一次。
- `JULIA_PWR_KEY_ENABLE` 默认开启，`JULIA_PWR_KEY_GPIO` 默认 6，
  `JULIA_PWR_OFF_HOLD_MS` 默认 3000ms，可在 menuconfig 调整。

这是电池电源切断，不是 S6 睡眠，不播放提示、不保存会话，也不等待 OTA 完成。
USB 连接时仍有外部供电，设备继续运行并记录日志，电池保持维持关闭；拔掉 USB 后会断电，
再按 PWR 可重新开机。没有增加 USB 下的假关机或深睡唤醒流程。

2026-09-21：`ctest --test-dir build-host -R '^power_key$' --output-on-failure` 通过，
ESP-IDF 5.5.4 `idf.py --no-ccache build` 完整编译通过（本机 ccache 启动失败，关闭缓存后成功）。
产物为 `build/julia_fused_base.bin`，大小 0x255760，应用分区剩余 67%。
host 测试覆盖开机按住、首次松手、短按、按下/释放抖动、门限前与门限上的释放、
持续长按和单次触发。需上板确认电池独供下断电及再次开机，并分别检查 USB 独供和 USB+电池；
软件测试不能证明电源轨已实际关闭。未执行烧录。
