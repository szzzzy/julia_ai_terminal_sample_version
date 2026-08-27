# main 源码目录

`main` 按功能域组织应用源码。新增代码应优先放入对应目录，避免再次堆积在组件根目录。

| 目录 | 职责 |
| --- | --- |
| `app/` | 干净的 `app_main` 入口、业务模块初始化和跨模块编排 |
| `audio/` | 音频素材检查、下载、校验和服务协调 |
| `display/` | LCD 面板与显示硬件驱动 |
| `hardware/` | LED、IO 扩展器等板级外设 |
| `lvgl_port/` | LVGL 与 ESP-IDF 显示/任务适配层 |
| `network/` | Wi-Fi 生命周期、MQTT 和通用 HTTP 下载 |
| `ota/` | 固件下载引擎、启动验收、控制、状态、校验和上报 |
| `storage/` | SD 卡等持久化存储适配 |
| `ui/` | 界面、Avatar 部件及生成资源 |
| `voice/` | 语音服务、WSS 传输、唤醒和嘴型同步 |

源码仍属于同一个 ESP-IDF `main` 组件；文件移动后需在 `CMakeLists.txt` 的 `srcs` 和 `INCLUDE_DIRS` 中登记对应路径。
