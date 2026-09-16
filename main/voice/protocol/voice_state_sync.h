#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 会话开始/结束和所有报文发送都只允许在 WSS owner 任务里调用；就绪状态（is_ready()）可以
 * 在其它任务只读，session_id 则只允许 owner 读取（见下）。 */
void voice_state_sync_start(void);
void voice_state_sync_end(void);
/* WSS owner 每轮调用：重发未确认的握手/状态，并在 ACK 超过发送上限时自行结束会话。 */
void voice_state_sync_poll(void);
/* true = 本报文由本模块消费（含会话不符而丢弃的消息）；false = 与状态同步无关，
 * 调用方应继续交给后续命令处理器。 */
bool voice_state_sync_handle_text(const uint8_t *text, size_t len);
/* 云端已确认状态快照；关闭 CONFIG_JULIA_CLOUD_STATE_SYNC_ENABLE 时恒为 true，不构成上传门槛。 */
bool voice_state_sync_is_ready(void);
/* owner-only：只允许 WSS owner 读取。内容会在 start 时重新生成、在 end 时被清空（start 内部
 * 也先做一次 end 复位），返回的指针始终指向同一块静态缓冲，因此其它任务不得读取或缓存它，
 * 需要判断会话是否可用时只能读 is_ready()；空串表示当前没有活动会话。
 * 它只用于报文归属比对，不是授权凭据——云端仍需独立校验设备身份。 */
const char *voice_state_sync_session_id(void);
