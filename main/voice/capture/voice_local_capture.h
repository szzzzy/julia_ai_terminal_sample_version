#pragma once
#include "esp_err.h"
#include "local_capture.h"
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#ifndef CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
#define CONFIG_JULIA_LOCAL_CAPTURE_ENABLE 0
#endif

/* 上行记录回调，在采音任务上下文同步执行：必须在返回前消费掉 buf，不得保留指针
 * （板级 PCM1 组帧缓冲会被下一帧复用）。generation 是采集时的连接代次；返回非 ESP_OK
 * 会让采集器置 failed，进而结束本次会话。 */
typedef esp_err_t (*voice_capture_send_t)(const uint8_t *, size_t, uint32_t generation);
/* 本地起音/结束事件回调，同样运行在采音任务上下文；只负责传达状态语义，是否据此改变语音状态由上层决定。 */
typedef void (*voice_capture_event_t)(lc_event_t, lc_mode_t);
/* 幂等：重复调用返回 ESP_OK 且不替换已注册的回调。工作内存（底噪窗、预录、判决历史）分配在
 * PSRAM；回调运行在板级采音任务上下文，必须快速返回，不能阻塞或直接触碰网络接口。 */
esp_err_t voice_local_capture_init(voice_capture_send_t send, voice_capture_event_t event);
/* 只由 WSS owner 在会话建立/结束时调用；文件传输结束、busy 冷却恢复等上行换代只重开
 * ring/pump 的代次，不经过本接口。generation 是本次连接的数据归属编号，换代即作废旧数据；
 * generation=0 表示停止协商（会话已结束）。poll() 内部并不判 generation=0，“0 之后不再发
 * capture_hello”依赖调用方停止轮询。 */
void voice_local_capture_connection(uint32_t generation);
/** WSS owner 每轮调用；返回 true 才是分段上传的放行闸门。未就绪时在此发 capture_hello，
 * 5 秒内收不到匹配的 capture_ready 就退避 60 秒并结束本次会话。 */
bool voice_local_capture_poll(void);
/* 返回 true = 本模块已消费该文本，调用方不得再当其它命令解析（含 session_id 不符而丢弃的消息）；
 * false = 与本模块无关，交给后续处理器。 */
bool voice_local_capture_text(const uint8_t *text, size_t len);
/* 采音任务每 20 ms 调用一次，输入必须是 656 字节 PCM1 帧；长度或 magic 不符时整帧丢弃。 */
void voice_local_capture_frame(const uint8_t *pcm1, size_t len);
/* 只设置期望模式；实际是否起音还要 capture_ready 已放行，未就绪时内部保持 LC_OFF。 */
void voice_local_capture_mode(lc_mode_t mode);
/* capture_ready 且代次未被替换时为 true，可在任意任务只读。 */
bool voice_local_capture_ready(void);
