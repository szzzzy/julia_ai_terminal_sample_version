# Julia 固件注释任务 — 审核发现的疑似问题汇总（仅记录，未修改）

> 来源：15 个并行子代理在补注释时发现的代码级观察。以下所有条目均未改动代码，
> 仅供后续人工复核。按影响面排序，标注 [高]/[中]/[低] 为主观严重度（未实际验证）。

## 构建/链接层面（最优先复核）

1. **[高] julia_ui.c 未纳入 CMakeLists 且包含 6 个不存在的头文件**
   `main/ui/julia_ui.c` include 了 `julia_ui_showcase.h / avatar_anim_engine.h /
   avatar_clip_map.h / transition_director.h / transition_player.h / idle_player.h`
   （base 工程无这些文件），并未被 `main/CMakeLists.txt` 的 srcs 收录——即当前
   不参与编译，属 L2/L3 裁剪遗留。若未来加入构建会立刻失败。
   （另：`ui/julia_display_theme.c`、`ui/avatar_parts/avatar_face.c`、`display/st77916_qspi.c`、
   `julia_voice.c` 均未入 srcs；st77916_qspi.c 还缺少其 .h 文件。）

2. **[中] julia_voice.c 为"参考实现"，未编译**：引用多个不存在的头
   （julia_audio.h / julia_ai_client.h / julia_speech_cloud.h / julia_local_tts.h /
   julia_network.h / julia_home.h / julia_system.h）；内部 `#if 0` 有死代码 base_prompt。

3. **[中] julia_display.h 悬空声明**：`julia_display_set_backlight(bool)` 只有声明、
   无实现（当前无人调用，未触发链接错误）。

## 语音链路

4. **[高] vstatus 回执未实现**：PROTOCOL.md §2.3 要求设备发布
   `mic_started/mic_stopped/file_send_queued/error ...`，但整个 main/ 未发现任何
   vstatus 发布；`voice_service_on_mqtt_command` 只入队、从不 publish。

5. **[中] MQTT vcmd 与 WSS 下行两入口执行不一致**：vcmd 的 MIC_START/MIC_STOP/
   FILE_SEND 仅入队（若从未建 WSS 会话则永不生效、队满仅 ESP_LOGW）；WSS 下行
   文本帧则直接执行 apply_mic_state()。需确认协议期望。

6. **[中] FILE_SEND 中途失败不触发重连**（wss_transport/voice_service 组合）：
   `voice_service_push_file` 在 ferror / total!=size 时返回 ESP_FAIL，但返回值被
   `(void)` 丢弃；`s_session_failed` 只在帧发送失败时置位 → 已发部分帧却不断链、
   不发 END，与 PROTOCOL"绝不发截断的 END"相悖。

7. **[低] voice_service_push_file 用函数级 static 缓冲区**：单实例串行使用安全；
   多任务并发会踩踏（无锁）。

## OTA

8. **[中] STORAGE_UNAVAILABLE 未映射**：`native_ota_failure_reason_name()` 的
   switch 缺 `NATIVE_OTA_FAILURE_STORAGE_UNAVAILABLE` → 上报字符串为 "UNKNOWN"，
   与 PROTOCOL.md §3.4 不一致（ota_types.h 已注明新增需追加并同步）。

9. **[中] ota_control_plane 的 expires_at=0 被判过期**：`now>0 && expires_at<=now`
   把缺省/0 值当作已过期，恒拒绝 upgrade；需与服务器确认 expires_at 必填且 >0。

10. **[低] ota_control_plane 的 request_id 单槽命中后不清空**：重复 QoS1 响应靠
    ota_engine 的 s_ota_in_progress 去重；周期检查与 ota_notify 叠加时，后发请求
    覆盖单槽，先发响应可能被 stale 丢弃（可能丢一次升级提示）。

11. **[低] ota_report 队列满时丢弃非终态事件（accepted/verifying/初始 downloading）**
    ：只发不持久化，掉电即丢——符合 best-effort 设计但服务器依赖中间态则受影响。

12. **[低] 断点续传强依赖 CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS**：关闭时
    ETag 恒 NULL、Content-Range 解析失败 → 206 分支落 RANGE_MISMATCH 终端失败，
    续传功能实际上不可用。

13. **[低] ota_cooldown_seconds 初段对称**：count=0/1 均返回 BASE，count≥2 才翻倍；
    前两次冷却时长相同（疑似有意）。此外冷却后二次 NVS 写失败会遗留 COOLING_DOWN
    记录并重复降温（边界鲁棒性）。

14. **[低] ota_report_event 无条件覆盖 s_store.context**：异常时序下可能覆盖先前
    任务的待验收上下文（正常路径 REBOOTING 后即重启，难触发）。

15. **[低] ota_report_store_load 遇损坏 blob 直接清空镜像继续运行**：无备份/再试。

16. **[低] ota_control_plane 的 type 同时接受 "ota_check_response" 与 "ota"**：
    比协议文档更宽松（需确认是否应严格化）。

## 状态机/情境

17. **[中] julia_context 与 julia_time 两条独立 RTC/SNTP 路径**：两处都可能
    init/写同一块 PCF85063，无共享互斥；julia_context_init 还独立调用
    esp_netif_sntp_init()（无 sync_cb），存在重复初始化与并发访问隐患。

18. **[低] julia_context 久别重逢只触发一次**：s_return_care_checked 置位后本周期
    不复位，用户再次长离隔（同开机周期）不再触发 EVT_ROUTINE_BREAK。

19. **[低] julia_fsm_transition 死代码**：计算 unused `summary[16]` + snprintf。

20. **[低] julia_context 日期范围不一致**：valid_time 允许 ≤2099，julia_time 允许
    ≤2069。

## 记忆/例行

21. **[高] julia_memory_init / julia_routine_init 全工程无调用点**：
    - 事件日志（s_event_queue 为 NULL）→ julia_memory_append 恒 INVALID_ARG，
      事件日志静默失效；
    - 例行偏差检测（s_lock 为 NULL）→ julia_routine_* 全部 no-op；
    - 画像/摘要/会话日志路径不依赖 init，仍可用。
    疑似启动流程接线遗漏，需人工确认。

22. **[低] julia_routine_on_activity 把 SENSOR（运动）计入"连续活跃"**：is_deviation
    可能把纯运动误判为用户持续在场。

23. **[低] julia_memory_append 中 type==0 时 summary 截 61 字节**（其它 64），来源不明。

24. **[低] contains_sensitive 用 strstr 子串匹配**：误报/漏报属启发式（非严格 PII）。

## 硬件

25. **[中] PCF85063_Set_Alarm 越界**：`uint8_t buf[5]` + `I2C_Write(...,buf,6)` 越界
    读第 6 字节；星期闹钟 0x80 未被写入。

26. **[中] QMI8658 setAccLPF/setGyroLPF 用 `!MASK`（逻辑非=0）清掩码**：
    `ctrl5 &= 0` 清空整个 CTRL5，应为 `~`。仅 init 路径调用一次，影响有限。

27. **[低] I2C 双驱动争用**：tca9554/pcf85063_shared 用 ESP-IDF i2c_master；
    QMI8658/Waveshare PCF85063 走外部 I2C_Driver（I2C_Driver.h 不在仓库，引脚未知）
    ——若共用 IO10/IO11 会冲突。

28. **[低] julia_led_init / breathing_led_update / julia_sd_init 均无调用点**：
    LED、呼吸灯、julia_sd 功能实际未接线（LED 首次 output 即置 output_failed）。

29. **[低] sd_card_start 无自动重试/拔卡检测**：与 main.c "挂载失败会自动重试"注释不符
    （真实为单次失败即返回）。另外 sd_card.c 与 julia_sd.c 两套实现都挂 /sdcard，
    voice_service 的 SD 锁只有 weak 默认无强符号实现（并发读写风险）。

30. **[低] QMI8658.c 重复声明 float accelScales**（tentative + 定义）。

31. **[低] board_rtc_set_time / PCF85063_Set_All 整时间写入不停振**、无写后回读校验。

## 显示/UI

32. **[中] julia_ui.c apply_expression 的 return; 后全是死代码**（几何脸分支被 GPT
    立绘取代；start_mouth_anim 等仅被死代码引用）——已标注 DEAD，勿补全勿删。

33. **[中] julia_display_theme.c 与 app/julia_idle_display.c 策略重叠**：当前运行时
    用后者（主题等未入构建）；两套背光/doze 策略并存，未来接入需明确唯一主人。

34. **[低] 三处 ST77916 厂商初始化序列取值/参数长度（0x21/0x11/0x29 的 data_bytes
    ）不一致**：当前实际编译路径用 julia_display.c 那组；属维护重复。

35. **[低] avatar_parts（chroma_assets）与微动引擎（layer_assets）两套眼睛/嘴资源
    可能对同一 lv_obj_t 互相覆盖 src**；`update_avatar` 与 state_worker_task 间
    存在无锁共享读取（单任务串行实际安全）。

36. **[低] DISPLAY_STABILITY_HOTFIX=1 导致 resume_all 仍暂停**（显示稳定性热修复）；
    转场仅做眼睛透明度过关。UI 层 "50ms" 旧注释与实现 40ms 不一致。

37. **[低] LV_COLOR_16_SWAP 推断**：依据 avatar_face_doze.c 的 #error 守卫推断
    面板按大端接收，建议在 sdkconfig/lv_conf 中最终确认。

## 网络

38. **[低] mqtt_comm 的 s_suback_deadline_ms 跨任务共享无锁**；s_reconnect_requested
    仅 volatile+通知同步；PUBACK 与断线竞态下可能对作废 msg_id 匹配残留槽位。

39. **[低] http_downloader_run 非可重入（文件级 static 缓冲区）**：两个任务并发
    调用会互相踩踏；当前调用方通常串行。

40. **[低] network_lifecycle 的 ip_ready 回调在生命周期任务中同步执行**：任一回调
    阻塞过久会连带推迟 Wi-Fi 重连调度（头文件注释已写明"不应无限阻塞"）。

## 其他（低价值观察）

41. julia_voice 追话计数无锁（s_followup_ready_ms/until_ms 跨任务读写）；
    s_dialog_rounds 只在唤醒词触发时归零（口径需确认）；
    julia_voice_init 失败无统一回滚。

42. julia_lipsync 累积状态非线程安全（单任务调用实际安全）；FUSED/VOICE_ONLY 模式
    节拍依赖 julia_audio_play_start 阻塞写扬声器（若该 sink 非阻塞嘴型会快于真实时间）。

43. voice_uri 结尾斜杠会被保留（"SD:/x/" → "/sdcard/x/"），打开的是目录路径；
    前缀匹配区分大小写。

44. julia_fsm 的 data 参数（所有处理器 (void)data）为预留未用。

45. mqtt_comm 实际使用 CONFIG_COMM_MQTT_BROKER_URI（默认 mqtt://172.20.10.2:1883）；
    "JULIA_SERVER_ADDR 空值拒绝启动"的校验在 Kconfig/别处，不在 mqtt_comm.c。
