/**
 * @file    julia_voice.h
 * @brief   语音顶层协调器（本地 AFE/WakeNet 语音管线）的对 <外> 接口。
 *
 * 职责（本组件负责什么）：
 * - 持有且只持有一个 julia_fsm_t 实例（s_fsm），并把 FSM 的 on_enter 回调
 *   接到 voice_on_enter() 上，从而在状态机迁移时同步驱动 UI；
 * - 维护"对话相位"（julia_dialog_phase_t，见 julia_ui.h）与 FSM 子状态之间的
 *   映射（LISTENING/THINKING/SPEAKING/IDLE），相位由本模块显式推进；
 * - 把 mic 采集（julia_audio*）、本地 AFE 推理（ESP-SR）、云 ASR/LLM/TTS
 *   （julia_speech_cloud* / julia_ai* / julia_local_tts*）串成一条
 *   唤醒->采集->识别->生成->播报 的完整对话会话；
 * - 提供诊断/烤机注入接口 julia_voice_inject_event()，复用真实 FSM 事件与
 *   相位事件路径；
 * - 向 context/memory 等模块暴露查询/控制接口：is_busy / interrupt /
 *   last_audio_activity_ms / get_state。
 *
 * 不负责（边界）：
 * - 不做 WSS/MQTT 上的语音数据收发（那是 voice_service 的职责）；
 * - 不做嘴型与扬声器播放（julia_lipsync / board_audio）；
 * - 不做本地唤醒检测本身——本模块是"唤醒后"的全流程，唤醒词检测由
 *   wake_detector 完成。本接口中的"唤醒"事件是唤醒检测器上报的结果。
 *
 * 线程模型：
 * - 本模块在内部创建 voice_feed / voice_detect / voice_session 三个任务，
 *   所有内部状态由这些任务读写；对 <外> 的查询接口通过 s_fsm_lock 互斥，
 *   注入接口在调用方任务上下文中同步执行。
 */
#pragma once
#include "esp_err.h"
#include "julia_fsm.h"

/**
 * @brief 初始化本地语音管线：开麦、建 CPU 频率锁、初始化 Qwen 客户端、
 *        创建 AFE/WakeNet（唤醒/回声/端点检测）并拉起 feed/detect 任务。
 *
 * @return ESP_OK 管线就绪，可开始检测唤醒词。
 * @return ESP_ERR_INVALID_STATE DashScope 密钥为空，无法进行云端识别/生成。
 * @return ESP_ERR_NOT_FOUND "model" 分区或指定名称的 WakeNet 模型不可用。
 * @return ESP_ERR_NO_MEM AFE 实例、互斥量或任一任务创建失败。
 *
 * @note 幂等约定见实现：本函数设计的调用时机是 app 启动早期、网络就绪前；
 *       重复调用会重复开麦与建锁，调用方不应重复调用。
 */
esp_err_t julia_voice_init(void);

/**
 * @brief 把 FSM 事件送进状态机（加锁后调用 julia_fsm_handle_event）。
 *
 * 状态迁移若触发子状态变化，会在锁内回调 voice_on_enter()，
 * 后者同步更新 UI 状态与对话相位。
 *
 * @param[in] event FSM 事件（见 julia_fsm.h 的 fsm_event_t）。
 * @return true  状态机发生了迁移（或全局事件被接受）；
 * @return false 事件被当前状态忽略 / FSM 互斥量获取超时。
 *
 * @note 可在任意任务上下文调用；内部用 s_fsm_lock 串行化对 s_fsm 的访问。
 */
bool julia_voice_handle_event(fsm_event_t event);

/**
 * @brief 返回当前 FSM 子状态（读 s_fsm.sub_state，加锁）。
 *
 * @return 当前子状态；锁获取失败时返回未加锁读到的旧值（尽力而为）。
 */
julia_sub_state_t julia_voice_get_state(void);

/**
 * @brief 查询是否正处于一次语音会话中（录到音并正在识别/生成/播报）。
 *
 * @return true 会话进行中；false 空闲。
 *
 * @note 读取原子标志 s_session_busy，不阻塞。context/memory 等模块用它
 *       判断是否应打断或避让当前语音处理。
 */
bool julia_voice_is_busy(void);

/**
 * @brief 请求打断当前 TTS 播放（置位 s_interrupt_requested）。
 *
 * 播放循环会及时停止写扬声器并结束本次会话；随后 pipeline 复位并重新
 * 打开 WakeNet，允许用新的唤醒词继续对话。
 *
 * @note 该标志由 detect 任务在读到唤醒词且会话忙时置位，也可由其他任务
 *       直接调用；它只是请求，实际打断由播放循环异步响应。
 */
void julia_voice_interrupt(void);

/**
 * @brief 返回最近一次检测到语音活动的时刻（ms，esp_timer 时基）。
 *
 * @return 最近语音活动时间戳；0 表示尚无记录。供 memory/routine 等模块
 *         判断用户是否长时间未发声。
 */
int64_t julia_voice_last_audio_activity_ms(void);

/**
 * @brief 供诊断/烤机注入的虚构事件，按"一次完整对话"的先后顺序取值。
 *
 * 每个值都映射到真实 FSM 事件 + 一次对话相位推进，从而在没有真实
 * mic/云端的情况下，也能按真实事件路径看到状态机与 UI 相位的变化。
 */
typedef enum {
    JULIA_VOICE_INJECT_WAKE = 0,     /**< 等价唤醒词命中-> 进入 LISTENING。 */
    JULIA_VOICE_INJECT_ASR_DONE,     /**< ASR 出文本后 -> 进入 THINKING。 */
    JULIA_VOICE_INJECT_LLM_RESPONSE, /**< LLM 产生回答 -> 进入 S4.x 多轮。 */
    JULIA_VOICE_INJECT_TTS_READY,    /**< TTS 就绪 -> 进入 SPEAKING。 */
    JULIA_VOICE_INJECT_TTS_DONE,     /**< 播放结束 -> 回到 IDLE 并复位。 */
} julia_voice_injected_event_t;

/**
 * @brief 诊断/烤机事件注入接口：复用真实 FSM 与对话相位事件路径。
 *
 * 输入一个虚构阶段事件，本函数在调用方任务上下文中同步执行对应的
 * FSM/UI 事件序列（见实现 switch）。这样无需真实 mic/ASR/LLM/TTS 即可
 * 驱动完整的对话状态转换，用于可视化验证与回归测试。
 *
 * @param[in] event 要注入的虚构阶段事件。
 * @param[in] text  可供日志显示的文本（如 ASR/LLM 结果），可为 NULL。
 * @return ESP_OK 事件被接受并执行；ESP_ERR_INVALID_ARG 事件值非法。
 *
 * @note 仅用于诊断/烤机；生产路径不会调用。它没有真实音频，因此不会
 *       触发实际播放，只驱动状态与相位。
 */
esp_err_t julia_voice_inject_event(julia_voice_injected_event_t event, const char *text);
