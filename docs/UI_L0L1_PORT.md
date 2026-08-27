# L0/L1 UI 移植执行文档（julia-fused-base）
> 时间：周二晚间。目标：**周三-Fri（3 天）交付 L0/L1**（立绘显示 + RMS嘴型 + 眨眼/呼吸/微动）。
> L2（相位剪辑）、L3（转场/待机 .trn）**不做**。本文档基于已核实的代码事实，直接照做。

---

## 0. 现状（已核实）

- **已完成**（本次已复制到 `julia-fused-base`）：
  - `main/lvgl_port/lvgl_port.c/.h`
  - `main/ui/avatar_parts/{avatar_face, avatar_eyes, avatar_mouth}.c/.h`
  - `main/ui/avatar_micro_motion.c/.h`、`main/ui/avatar_micro_action.h`
  - `main/ui/julia_ui.c/.h`（⚠️ 需裁剪，见 §2）
  - `main/speech/julia_lipsync.c/.h`（RMS→嘴型核心）
  - `main/display/st77916_qspi.c`（LCD 驱动）
  - `main/ui/generated/`（`avatar_layers/*.bin+.c` 全资源 + `julia_rig_assets.c/.h` + `julia_ui_assets.c/.h/.bin` + `julia_blink_assets.c/.h`）
- **base 已有**：`components/{espressif__esp-sr, espressif__esp-dsp, julia_board_audio}`；语音链路已跑通。
- **base 没有**：lvgl 组件、`esp_lcd_st77916` 驱动、`julia_ui_showcase`/`avatar_anim_engine`/`avatar_clip_map`/`transition_*`/`idle_player`/`julia_display_theme`/`breathing_led`。

---

## 1. 复制 LVGL 组件与 LCD 依赖（未做，需执行）

```powershell
# LVGL 组件（fused 用 waveshare_demo 路径）
Copy-Item 'D:\Espressif\projects\julia-esp32s3-ai-terminal-fused\waveshare_demo\ESP-IDF\ESP32-S3-LCD-1.85-Test\components\lvgl__lvgl' `
          'D:\Espressif\projects\julia-fused-base\components\lvgl__lvgl' -Recurse -Force

# ST77916 LCD 驱动（fused 的 esp_lcd_st77916 组件目录）
Copy-Item 'D:\Espressif\projects\julia-esp32s3-ai-terminal-fused\waveshare_demo\ESP-IDF\ESP32-S3-LCD-1.85-Test\main\LCD_Driver\esp_lcd_st77916' `
          'D:\Espressif\projects\julia-fused-base\components\esp_lcd_st77916' -Recurse -Force -ErrorAction SilentlyContinue
# 若上面路径不存在：从 fused 的 main.c 摘 LCD 初始化代码（见 §3.1）
```

> ⚠️ LVGL 组件很大（含示例），如果复制慢可用 fused 的 `EXTRA_COMPONENT_DIRS` 方式：
> 在 base 顶层 `CMakeLists.txt` 的 `include()` 前加：
> ```cmake
> set(EXTRA_COMPONENT_DIRS
>     "${CMAKE_SOURCE_DIR}/components/lvgl__lvgl")
> ```

---

## 2. 裁剪 `main/ui/julia_ui.c`（关键，否则链接失败）

**必须删除的 include**（L2/L3 依赖，base 无对应文件）：
```c
#include "julia_ui_showcase.h"
#include "avatar_anim_engine.h"
#include "avatar_clip_map.h"
#include "transition_director.h"
#include "transition_player.h"
#include "idle_player.h"
#include "julia_display_theme.h"
#include "breathing_led.h"
```

**对应删除/替换的函数调用**（在 `julia_ui.c` 内搜索并处理）：
| 原调用 | 处理 |
|---|---|
| `julia_ui_showcase_allows_state_change()` | 改为 `return true;`（或删函数，改为空实现） |
| `transition_director_*` / `transition_player_*` / `transition_target_commit()` | 删除 `state_transition_apply()` 里的转场分支，改为**直接立绘切换**：`avatar_face_set_state(to_main)` + `julia_ui_set_idle_frame_mode(false)` 简化 |
| `idle_player_*` | 删除（或空实现） |
| `avatar_clip_map_*` / `avatar_anim_engine_*` | 删除 |
| `breathing_led_*` / `julia_display_theme_*` | 删除 |
| `led_transition_to` | 删除（无 LED 桥） |

**必须保留**（L0/L1 核心）：
- `julia_ui_init()` —— 建 LVGL 资源、`avatar_face_init`、立绘加载、`avatar_micro_motion_init()`
- `julia_ui_set_mouth_openness()` / `julia_ui_talking_start/stop()`
- `julia_ui_set_dialog_phase()` —— 只保留调 `avatar_micro_motion_set_dialog_phase` 部分，删 clip_map 部分
- `avatar_show_all()`、`julia_ui_get_avatar_slot()`、`julia_ui_current_state()`
- `apply_expression()`（有 `return;` 死代码，可不动）

---

## 3. 接线（base 侧）

### 3.1 `main/main.c`（native_ota_example.c 里的 app_main）
在初始化序列（`board_audio_init()` 后、网络前）加入：
```c
/* L0/L1：LCD + LVGL + 立绘 */
lcd_init_panel();                       /* st77916_qspi.c / 或从 fused main.c 摘 */
lvgl_port_init(s_panel_handle);
julia_ui_init();
avatar_show_all();
```
主循环（现有 `while(1)` 或独立任务，每 ~40ms）：
```c
update_avatar((uint32_t)(esp_timer_get_time() / 1000ULL));
```
> `s_panel_handle` 来自 `st77916_qspi.c` 或 fused `main.c` 的 LCD 初始化（`esp_lcd_new_panel_st77916`）。若摘 fused main.c：复制 `lcd_reset_via_exio/lcd_new_io/lcd_init_panel` 及 `vendor_specific_init_new[]`（main.c:107-425 段）+ 引脚宏（105-99）。

### 3.2 `main/voice/voice_service.c` → 下行 PCM 驱动嘴型（**L1 关键**）
`voice_service_on_binary()` 中，在 `board_audio_speaker_write()` 后追加：
```c
/* L1：下行音频 RMS → 嘴型开合（方案 B：不复制整个 lipsync 模块） */
extern void julia_ui_talking_start(void);
extern void julia_ui_set_mouth_openness(uint16_t openness_q8);
extern void julia_ui_talking_stop(void);
/* 在 begin/end 之间，逐帧计算 RMS：
 * rms = sqrt(mean(sample^2)); level = rms 分档 (0/30/65/95);
 * openness = level * 256; julia_ui_set_mouth_openness(openness);
 * 帧周期 = len / 16000；用 vTaskDelay 保持节奏（可选）。
 */
```
> 提示：`julia_lipsync.c` 里有现成的 `mouth_level_for_frame()`（RMS→档位）可抄逻辑；不引入 `julia_audio_play_start` 依赖（base 用 board_audio 直连）。

### 3.3 CMake（`main/CMakeLists.txt`）
- `SRCS` 增加：
  ```cmake
  "lvgl_port/lvgl_port.c"
  "ui/julia_ui.c"
  "ui/avatar_parts/avatar_face.c" "ui/avatar_parts/avatar_eyes.c" "ui/avatar_parts/avatar_mouth.c"
  "ui/avatar_micro_motion.c"
  "speech/julia_lipsync.c"
  "display/st77916_qspi.c"
  "ui/generated/julia_ui_assets.c" "ui/generated/julia_rig_assets.c"
  "ui/generated/avatar_layers/avatar_layer_assets.c" "ui/generated/avatar_layers/avatar_chroma_assets.c"
  "ui/generated/avatar_layers/avatar_face_base.c" "ui/generated/avatar_layers/avatar_face_doze.c"
  "ui/generated/julia_blink_assets.c"
  ```
- `PRIV_REQUIRES` 增加：`lvgl__lvgl esp_lcd esp_driver_spi esp_driver_i2s`
- `EMBED_FILES` 增加（立绘/嘴型 bin）：
  ```cmake
  "ui/generated/avatar_layers/eye_left_open.bin" "ui/generated/avatar_layers/eye_left_half.bin"
  "ui/generated/avatar_layers/eye_left_closed.bin" "ui/generated/avatar_layers/eye_right_open.bin"
  "ui/generated/avatar_layers/eye_right_half.bin" "ui/generated/avatar_layers/eye_right_closed.bin"
  "ui/generated/avatar_layers/avatar_pupil_left.bin" "ui/generated/avatar_layers/avatar_pupil_right.bin"
  "ui/generated/avatar_layers/mouth_closed.bin" "ui/generated/avatar_layers/mouth_half.bin"
  "ui/generated/avatar_layers/mouth_open.bin" "ui/generated/avatar_layers/hair_tip.bin"
  "ui/generated/julia_ui_assets.bin"
  ```

---

## 4. 禁止改动（防回归）
- ❌ 不动 `board_audio.c`/`wss_transport.c`/`mqtt_comm.c`（语音地基）；
- ❌ 不引入 L2/L3（`avatar_anim_engine`、`avatar_clip_map`、`transition_*`、`idle_player`、`julia_ui_showcase`、`breathing_led`、`julia_display_theme`）；
- ❌ 不改 fused 源工程；
- ❌ 不把 `julia_ui_set_state` 的完整转场搬过来（那是 L3）。

---

## 5. 三天进度表（对齐目标）

| 天 | 交付 | 验收 |
|---|---|---|
| 周三 | §1 LVGL/LCD 组件 + §2 裁剪 julia_ui.c + §3.3 CMake | `idf.py build` 通过 |
| 周四 | §3.1 接线（LCD/LVGL/init/update_avatar）+ §3.2 RMS→嘴型 | 烧录 → LCD 亮 + 立绘 + 眨眼/呼吸 + 说话动嘴 |
| 周五 | 联调：唤醒→说话→嘴动→回待机，串口日志 | 老师演示：设备说话会动、会眨眼 |

## 6. 常见坑（提前避）
1. **立绘不显示** → 检查 `EMBED_FILES` 是否声明了 `julia_ui_assets.bin` + `avatar_layers/*.bin`（§3.3 已列）；
2. **`julia_ui.c` 链接错误** → 一定是 L2/L3 include 或函数调用没删干净（§2 清单）；
3. **LVGL 初始化卡死** → 确认 `lvgl_port_init` 前 LCD panel 已建（§3.1 顺序）；
4. **嘴型不动** → 确认 `julia_ui_talking_start()` 先调（否则 `set_mouth_openness` 有 `s_talking` 守卫）；
5. **`update_avatar` 不动** → 确认 `avatar_micro_motion_init()` 在 `julia_ui_init` 里被调用了（fused 的 julia_ui.c 有）。
