/**
 * @file    julia_voice.c
 * @brief   未参与当前构建的旧本地 ASR/LLM/TTS 语音栈参考实现。
 *
 * 当前运行路径是 board_audio + voice_service + wss_transport；main/CMakeLists.txt
 * 未包含本文件。这里描述的 Qwen、本地会话 FSM 和流式 TTS 不能作为当前能力声明。
 *
 * 一条完整会话的数据流（本模块串起来的"链路"）：
 *
 *   麦克风 I2S --feed_task--> ESP-SR AFE --detect_task 拉取结果-->
 *      判唤醒词 / 语音能量 -> 采集用户语音（静音端点）-> session_task
 *   session_task:
 *      ASR（云端/本地）-> 文本；若命中本地命令/遗忘指令直接响应；
 *      LLM（Qwen）流式生成 -> 句子切分 -> 逐句 TTS -> 扬声器+口型；
 *      任一环节失败按"三级输出链"降级（云端->本地TTS->固化提示音）。
 *
 * 职责边界：
 * - 负责"语音会话"这一层：采集、识别、生成、播放与失败降级；
 * - 不负责 WSS/MQTT 上的音频/命令收发（voice_service）；
 * - 不负责唤醒词检测（wake_detector）——但会把唤醒结果（EVT_USER_CALL）
 *   接入本模块的 FSM，并响应会话忙时的打断请求。
 *
 * 线程模型：
 * - voice_feed（core0,p6）：仅从 mic 读块喂 AFE；
 * - voice_detect（core1,p5）：AFE fetch，判唤醒/语音/语音起止，采集到
 *   s_recording 后创建 session_task；
 * - voice_session（core1,p5，动态创建）：ASR/LLM/TTS/播放的整个会话，
 *   结束自删；是唯一会写 NVS 的任务（因此栈必须在内部 SRAM，见 detect_task）；
 * - stream_playback_task：仅当 TTS 采用流式播报时创建，消费
 *   synthesize_and_queue_segment 入队的 PCM 段。
 *
 * 共享状态串行化：
 * - s_fsm（含 sub_state/main_state）由 s_fsm_lock 互斥量保护；
 * - s_session_busy / s_interrupt_requested / s_dialog_rounds /
 *   s_last_audio_activity_ms 等为 volatile 原子标志/计数器，任务间越过锁
 *   直接读写（靠 volatile + 单字访问，未用原子指令）；
 * - s_recording / s_recorded_samples 在 detect 与 session 之间通过"先还原
 *   到本地变量再清零"的一次性交接传递，避免两个任务同时持有。
 *
 * 结构说明：本文件是 fused 工程较早的"本地全流程语音"，在 fused-base 里
 * wake_detector.c（本地唤醒）与 voice_service.c（WSS 语音面）已把它拆掉。
 * 因此本目录未把本文件纳入 main/CMakeLists.txt 的 srcs（不参与编译），
 * 保留作为轨道/参考实现。
 * NOTE：需结合调用方确认：julia_audio.h / julia_ai_client.h / julia_speech_cloud.h /
 * julia_local_tts.h / julia_network.h / julia_home.h / julia_system.h 均不在本目录内，
 * 本文件对它们的函数契约（如 receive_chunk 的结束约定、TTS 缓冲所有权）为据调用点推断。
 */
#include "julia_voice.h"

#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_wn_iface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "julia_ai_client.h"
#include "julia_audio.h"
#include "wake_word_config.h"
#include "wake_reply.h"
#include "julia_fsm.h"
#include "julia_home.h"
#include "julia_network.h"
#include "julia_memory.h"
#include "julia_routine.h"
#include "julia_speech_cloud.h"
#include "julia_local_tts.h"
#include "julia_lipsync.h"
#include "julia_system.h"
#include "julia_ui.h"
#include "avatar_micro_action.h"
#include "model_path.h"
#include "sdkconfig.h"

#define TAG "JULIA_VOICE"

/*
 * 语音采集 / 端点检测的参数（单位默认 ms / 采样点）。
 * 这些值决定"何时算说了话、何时算说完话"：
 * - NO_SPEECH_TIMEOUT_MS：开麦后一直没检出语音，等待这么长就放弃本次采集；
 * - END_SILENCE_MS：检测到语音后，连续静音这么长就判定一句说完；
 * - MIN_CAPTURE_MS：即使语音很短，也至少要采集这么长，避免被噪声误触发截断；
 * - SPEECH_PEAK/RMS_THRESHOLD：VAD 之外的能量闸门；另有 SPEECH_CONFIRM_FRAMES
 *   帧持续命中才确认为语音，滤掉孤立的环境尖峰。
 */
/* 单次对话可采集的最大样本数：6 秒 @ 采样率（16k => 96000 样本）。 */
#define MAX_RECORD_SAMPLES (JULIA_AUDIO_SAMPLE_RATE * 6)
/* 开麦后未检出任何语音的最大容忍时间。 */
#define NO_SPEECH_TIMEOUT_MS 5000
/* 说话人停顿多少毫秒视为一句结束（端点剪枝）。 */
#define END_SILENCE_MS 750
/* 最短有效采集时长，防止过快触发剪掉开头。 */
#define MIN_CAPTURE_MS 1200
/* 语音峰值的确认阈值（时域峰值门限）。 */
#define SPEECH_PEAK_THRESHOLD 650
/* 语音均方根（RMS）的确认阈值。 */
#define SPEECH_RMS_THRESHOLD  260
/* 连续多少帧命中能量闸门才确认"有语音"，抑制瞬时毛刺。 */
#define SPEECH_CONFIRM_FRAMES  3
/* 网络不可用回退提示音（固化 PCM 文件，SPIFFS/SD）。 */
#define NETWORK_PROMPT_PATH "/sdcard/julia/netmsg.pcm"
/* 追话窗口（跟接话音）：FOLLOWUP_WINDOW_MS 为 0 表示当前禁用追话。 */
#define FOLLOWUP_WINDOW_MS 0
/* 追话窗口开始前，需距上一次回答多久（冷却时间）。 */
#define FOLLOWUP_COOLDOWN_MS 700
/* 单次语音会话总时长上限，超时强制结束（防云端挂死）。 */
#define VOICE_SESSION_MAX_MS 60000
/* 一次唤醒后允许的连续对话轮数上限，防止无限追话/循环。 */
#define MAX_DIALOG_ROUNDS 5

/*
 * 模块级状态（单例）。多数为 volatile 布尔/计数器，供 detect/session 任务
 * 跨任务共享；s_fsm 与 s_fsm_lock 成对使用，详见文件头"共享状态串行化"。
 */
static const esp_afe_sr_iface_t *s_afe;        /* AFE 接口句柄（ESP-SR 静态表）。 */
static esp_afe_sr_data_t *s_afe_data;          /* AFE 运行时实例（含 WakeNet/VAD 模型）。 */
static srmodel_list_t *s_models;               /* 模型分区加载后的模型列表（用于过滤唤醒模型）。 */
static julia_fsm_t s_fsm;                      /* FSM 实例；由 s_fsm_lock 保护。 */
static volatile bool s_session_busy;           /* 是否正在一次语音会话中。 */
static volatile bool s_interrupt_requested;    /* 请求打断当前 TTS 播放。 */
static int16_t *s_recording;                   /* detect 采集缓冲区，交接给 session_task。 */
static size_t s_recorded_samples;              /* s_recording 上已写入的样本数。 */
static esp_pm_lock_handle_t s_cpu_lock;        /* CPU 频率上限锁，保证 SR 推理稳定。 */
static SemaphoreHandle_t s_fsm_lock;           /* 保护 s_fsm / 派生状态的互斥量。 */
static volatile int64_t s_last_audio_activity_ms; /* 最近语音活动时刻（esp_timer ms）。 */
static volatile int64_t s_followup_until_ms;   /* 追话窗口截止时刻。 */
static volatile uint8_t s_dialog_rounds;       /* 本次唤醒以来的对话轮数。 */
static volatile int64_t s_followup_ready_ms;   /* 追话窗口最早允许开始的时刻（冷却后）。 */
static volatile int64_t s_wake_resume_ms;      /* 唤醒重新生效的延迟：防止回声/残留误唤醒。 */

/*
 * 会话失败阶段枚举：明确记录"卡在哪一环"，供降级/回执/日志区分。
 * 取值顺序与链路执行顺序保持一致（ASR -> LLM -> TTS -> 播放）。
 */
typedef enum {
    VOICE_FAILURE_NONE = 0,     /**< 无失败。 */
    VOICE_FAILURE_ASR = 1,      /**< 语音识别失败或超时。 */
    VOICE_FAILURE_AI = 2,       /**< LLM 生成失败（无回答/无网络）。 */
    VOICE_FAILURE_TTS = 3,      /**< 语音合成失败。 */
    VOICE_FAILURE_PLAYBACK = 4, /**< 扬声器播放失败。 */
} voice_failure_t;

/**
 * @brief 兜底的离线提示音：所有输出链都失败时，用短促低频音告知用户。
 *
 * @param[in] failure 失败阶段，仅用于日志；声音反馈本身不区分失败类型。
 */
static void play_failure_code(voice_failure_t failure)
{
    ESP_LOGW(TAG, "offline voice feedback: failure=%d", failure);
    /* 220Hz 短音：无语义，仅用于"我失败了"这一可感知的回执。 */
    julia_audio_play_tone(220, 70, 18);
}

/**
 * @brief 识别并就地执行"本地命令"，命中后返回 true，否则返回 false。
 *
 * 这些是无需联网/无需 LLM 的关键词命令（status / time / standby，含中文）。
 * 命中时直接给出一条本地 TTS 配音（失败则降级为提示音），用于在断网或
 * 需要快速反馈时也能得到响应，避免每次都要走一遍云链路。
 *
 * @param[in] text ASR 输出的用户文本，可为 NULL（视作未命中）。
 * @return true  命令被识别并已处理（调用方无需再进入 LLM 流程）；
 * @return false 未命中任何本地命令，调用方继续走 LLM。
 *
 * @note 仅在 session_task（ASR 成功之后、LLM 之前）调用，上下文为会话任务。
 */
static bool handle_local_command(const char *text)
{
    if (!text) return false;
    if (strstr(text, "status") || strstr(text, "状态")) {
        ESP_LOGI(TAG, "Local command: status");
        /* TTS 失败降级为两声上行音，保证仍有听觉反馈。 */
        if (julia_local_tts_speak("系统运行正常") != ESP_OK) {
            julia_audio_play_tone(660, 90, 45); julia_audio_play_tone(880, 90, 45);
        }
        return true;
    }
    if (strstr(text, "time") || strstr(text, "时间")) {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        ESP_LOGI(TAG, "Local command: time %02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        if (julia_local_tts_speak("时间已同步") != ESP_OK) julia_audio_play_tone(880, 120, 45);
        return true;
    }
    if (strstr(text, "standby") || strstr(text, "待机")) {
        ESP_LOGI(TAG, "Local command: standby");
        if (julia_local_tts_speak("进入待机模式") != ESP_OK) julia_audio_play_tone(440, 180, 40);
        return true;
    }
    return false;
}

/**
 * @brief 用关键词在用户文本中做轻量情感判读，命中则返回可附加到 system
 *        prompt 的共情提示，并写出情感标签。
 *
 * 纯本地关键词匹配，无云端/无模型开销。命中"低落/疲惫/积极"三类时：
 * - 返回一段中文提示串（用于增强 LLM 的共情语气）；
 * - 写入 *label 为对应 julia_emotion_t，供 memory 记录带情感的事件。
 *
 * @param[in]  text  用户的 ASR 文本（非 NULL）。
 * @param[out] label 接收 JULIA_MEMORY_EMOTION_* 值；未命中时保持不变。
 * @return 非 NULL 表示命中了某种情感，返回提示串；
 *         返回 NULL 表示未识别出情感（调用方按普通对话处理）。
 *
 * @note 仅在 session_task 中调用；不阻塞，纯字符串匹配。
 */
static const char *detect_emotion(const char *text, uint8_t *label)
{
    static const char *sad[] = {"难过", "伤心", "不开心", "想哭", "孤独", "焦虑", "压力"};
    static const char *tired[] = {"累了", "好累", "疲惫", "困了", "没精神"};
    static const char *happy[] = {"开心", "高兴", "太好了", "喜欢", "兴奋"};
    for (size_t i = 0; i < sizeof(sad) / sizeof(sad[0]); ++i)
        if (strstr(text, sad[i])) { *label = JULIA_MEMORY_EMOTION_SAD; return "用户情绪低落，请先共情，再简短回应。"; }
    for (size_t i = 0; i < sizeof(tired) / sizeof(tired[0]); ++i)
        if (strstr(text, tired[i])) { *label = JULIA_MEMORY_EMOTION_TIRED; return "用户感到疲惫，请温和关心并避免冗长回答。"; }
    for (size_t i = 0; i < sizeof(happy) / sizeof(happy[0]); ++i)
        if (strstr(text, happy[i])) { *label = JULIA_MEMORY_EMOTION_HAPPY; return "用户情绪积极，请自然分享这份愉快。"; }
    return NULL;
}

/**
 * @brief 加锁后把 FSM 事件交入状态机，返回是否发生了迁移。
 *
 * 封装 julia_fsm_handle_event()：先对 s_fsm_lock 做 1s 上锁（超时则放弃，
 * 视为未迁移，避免阻塞调用方），迁移过程中回调 voice_on_enter() 同步驱动
 * UI 状态与对话相位。任务之间（detect/session 与外部查询）经由这把锁
 * 串行化对 s_fsm 的访问。
 *
 * @param[in] event 要送入状态机的事件。
 * @return true  事件被接受并发生状态迁移（或为全局事件）；
 * @return false 上锁超时，或当前子状态忽略该事件。
 *
 * @note 可由任意任务上下文调用；不要在持有其他锁时调用（避免锁序反转）。
 */
bool julia_voice_handle_event(fsm_event_t event)
{
    if (!s_fsm_lock || xSemaphoreTake(s_fsm_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    bool changed = julia_fsm_handle_event(&s_fsm, event, NULL);
    xSemaphoreGive(s_fsm_lock);
    return changed;
}

/**
 * @brief 加锁读取当前 FSM 子状态；上锁失败时返回未加锁读到的旧值（尽力而为）。
 *
 * 与 julia_fsm_handle_event 共用 s_fsm_lock，但用更短的 100ms 超时，因为这是
 * 高频查询路径（context/UI 等模块轮询），不应长时间阻塞调用者。
 */
julia_sub_state_t julia_voice_get_state(void)
{
    if (!s_fsm_lock || xSemaphoreTake(s_fsm_lock, pdMS_TO_TICKS(100)) != pdTRUE) return s_fsm.sub_state;
    julia_sub_state_t state = s_fsm.sub_state;
    xSemaphoreGive(s_fsm_lock);
    return state;
}

/**
 * @brief 查询是否正在一次语音会话中（读原子标志，不阻塞、不需要锁）。
 *
 * @return true 会话进行中（采集后正在识别/生成/播放）；false 空闲。
 */
bool julia_voice_is_busy(void) { return s_session_busy; }

/**
 * @brief 请求打断当前播放：仅置位 s_interrupt_requested 并记日志。
 *
 * 真正的打断由播放循环在下一次检查时发现该标志而执行（停止写扬声器、
 * 结束本次会话、开回 WakeNet）。这是异步、非阻塞的安全打断接口，可在
 * 任意任务上下文中调用。
 */
void julia_voice_interrupt(void)
{
    s_interrupt_requested = true;
    ESP_LOGI(TAG, "Voice interruption requested");
}

/**
 * @brief 返回最近一次检测到语音活动的绝对时刻（esp_timer ms）。
 *
 * @return 时间戳；0 表示尚无记录。用于 memory/routine 判断用户"是否久未说话"。
 */
int64_t julia_voice_last_audio_activity_ms(void) { return s_last_audio_activity_ms; }

/**
 * @brief 诊断/烤机事件注入：按虚构阶段依次驱动真实 FSM 事件与对话相位。
 *
 * 每个注入值映射为若干真实调用：触发一次 activity、送入对应的 FSM 事件、
 * 并显式推进对话相位（LISTENING/THINKING/SPEAKING/IDLE）。它不产生真实
 * 音频，仅用于在无 mic/无云端的场合可视化验证状态机与 UI 相位联动。
 *
 * @param[in] event 要注入的虚构阶段事件。
 * @param[in] text  日志用文本（ASR/LLM 结果等），可为 NULL。
 * @return ESP_OK 注入成功；ESP_ERR_INVALID_ARG 事件值非法。
 *
 * @note 仅在诊断/烤机路径调用；不校验会话是否真的存在对应阶段。
 */
esp_err_t julia_voice_inject_event(julia_voice_injected_event_t event, const char *text)
{
    switch (event) {
    case JULIA_VOICE_INJECT_WAKE:
        julia_routine_on_activity(JULIA_ACTIVITY_WAKE);
        julia_voice_handle_event(EVT_USER_CALL);
        julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_LISTENING);
        break;
    case JULIA_VOICE_INJECT_ASR_DONE:
        ESP_LOGI(TAG, "Injected ASR: %s", text ? text : "");
        julia_voice_handle_event(EVT_START_DIALOG);
        julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_THINKING);
        break;
    case JULIA_VOICE_INJECT_LLM_RESPONSE:
        ESP_LOGI(TAG, "Injected LLM: %s", text ? text : "");
        julia_voice_handle_event(EVT_MULTI_TURN_DETECTED);
        break;
    case JULIA_VOICE_INJECT_TTS_READY:
        julia_voice_handle_event(EVT_MULTI_TURN_DETECTED);
        julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_SPEAKING);
        break;
    case JULIA_VOICE_INJECT_TTS_DONE:
        julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_IDLE);
        julia_voice_handle_event(EVT_SILENCE_TIMEOUT);
        if (julia_voice_get_state() != JULIA_SUB_STATE_S1_1_NEAR_STANDBY)
            julia_voice_handle_event(EVT_WAKEUP);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/**
 * @brief FSM 的 on_enter 回调：子状态变化时同步 UI 状态，并映射对话相位。
 *
 * 由 julia_fsm_transition() 在迁移完成后调用（本文件的调用路径是
 * julia_voice_handle_event() 之内、s_fsm_lock 持有的期间），因此它必须是
 * 轻量的 UI 更新，不做长耗时操作。
 *
 * FSM 子状态 -> 对话相位映射（本函数仅显式处理两个相位敏感的入口状态）：
 * - JULIA_SUB_STATE_S3_3_USER_CALL（收到用户呼叫/唤醒）-> LISTENING；
 * - JULIA_SUB_STATE_S1_1_NEAR_STANDBY（回到近场待机）-> IDLE；
 * 其余子状态不在此处改相位，相位由会话流程在相应阶段显式推进。
 *
 * @param[in] fsm    FSM 实例（未使用）。
 * @param[in] state  新进入的子状态。
 * @param[in] event  触发迁移的事件（未使用）。
 */
static void voice_on_enter(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t event)
{
    (void)fsm; (void)event;
    julia_ui_set_state(state);
    if (state == JULIA_SUB_STATE_S3_3_USER_CALL) julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_LISTENING);
    else if (state == JULIA_SUB_STATE_S1_1_NEAR_STANDBY) julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_IDLE);
}

/**
 * @brief 把状态机拉回近场待机（S1.1 / IDLE）。
 *
 * 会话结束或取消时的统一收尾：先发 EVT_SILENCE_TIMEOUT 使其向下迁移，
 * 若仍不在 S1.1（如停留在对话/其他状态）再补发 EVT_WAKEUP 强制回到待机。
 *
 * 注意：用 s_fsm.sub_state 直接判断当前状态，而没有加锁读取；仅在本模块
 * 串行上下文（同一任务已持有/本意可控）里调用，是安全的设计选择。
 */
static void return_to_standby(void)
{
    julia_voice_handle_event(EVT_SILENCE_TIMEOUT);
    if (s_fsm.sub_state != JULIA_SUB_STATE_S1_1_NEAR_STANDBY)
        julia_voice_handle_event(EVT_WAKEUP);
}

/**
 * @brief 对 LLM 流式文本切出"一个可立即 TTS 的句子"的字节数。
 *
 * 目的：边收 LLM 边合成播放，降低首字节延迟。返回 0 表示当前还没有一个
 * 完整句子可切（继续累积）。切分优先级：
 *   1) 英文句点/叹号/问号/分号；
 *   2) 中文的 。！？；
 *   3) 超过约 24 个汉字（72 字节）后遇到逗号（，/ ,）作为自然 TTS 边界；
 *   4) 超过 900 字节后按安全边界硬切（避免单条 TTS 过长）。
 *
 * 注意中文 UTF-8 多字节与字节定位：中文标点是 3 字节，返回值是其占用的
 * 字节数；硬切（case 4）会在字节级回溯，避免把多字节字符拦腰截断。
 *
 * @param[in] text 自前一次切分处起累积的待切文本（非 NULL、以 NUL 结尾）。
 * @param[in] final 是否为最后一段；为 true 时即使没有标点也返回剩余全部。
 * @return 本次可切出的字节数（>0）；0 表示尚未成句、继续累积。
 *
 * @note 仅处理句子边界，不校验文本正确性；由调用方 memmove 消费掉这一长度。
 */
static size_t sentence_segment_bytes(const char *text, bool final)
{
    size_t length = strlen(text);
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '.' || text[i] == '!' || text[i] == '?' || text[i] == ';')
            return i + 1;
        if (i + 2 < length && (memcmp(text + i, "。", 3) == 0 ||
                               memcmp(text + i, "！", 3) == 0 ||
                               memcmp(text + i, "？", 3) == 0 ||
                               memcmp(text + i, "；", 3) == 0))
            return i + 3;
    }
    /* Long first sentences may stream for several seconds before a full stop.
     * A comma after roughly 24 Chinese characters is a natural TTS boundary. */
    if (length >= 72) {
        for (size_t i = 0; i < length; ++i) {
            if (text[i] == ',') return i + 1;
            if (i + 2 < length && memcmp(text + i, "，", 3) == 0) return i + 3;
        }
    }
    if (length >= 900) {
        size_t cut = 900;
        while (cut > 0 && ((uint8_t)text[cut] & 0xc0U) == 0x80U) --cut;
        return cut;
    }
    return final ? length : 0;
}

/*
 * 流式播报（边 TTS 边放）用的小结构：
 * - stream_pcm_item_t：队列里的一段 PCM（或其"结束哨兵"），由生产者（会话任务）
 *   malloc 并入队，消费者（stream_playback_task）播放后 free。
 * - stream_playback_t：一次流式播放的共享句柄，跨生产者/消费者传递状态。
 */
typedef struct {
    int16_t *pcm;    /* 待播放的 PCM 缓冲（由消费者释放）。 */
    size_t samples;  /* PCM 样本数。 */
    bool end;        /* 队列结束哨兵：置位后消费者退出循环。 */
} stream_pcm_item_t;

typedef struct {
    QueueHandle_t queue;        /* PCM 段队列（有界，容量 2）。 */
    SemaphoreHandle_t done;     /* 播放任务结束信号（二值信号量）。 */
    esp_err_t result;           /* 播放结果，由消费者写、生产者读。 */
    bool played_any;            /* 是否已播放过至少一段（判断是否真的出声）。 */
    int64_t session_started_ms; /* 会话起点，用于记录首帧延迟。 */
} stream_playback_t;

/**
 * @brief 流式播报任务：消费句段队列，边播放边驱动口型，播完自删。
 *
 * 由 synthesize_and_queue_segment() 在首次成功合成时创建（容量 8192）。
 * 消费逻辑：
 *   - 从 queue 取一段 PCM；收到 end 哨兵退出循环；
 *   - 首段到来时：把状态推到 S4.x（多轮）、开启 WakeNet（这样播报中仍可用
 *     新唤醒词打断）、清 interrupt 标志、开启口型链路；
 *   - 按 ≤4096B 分块写 julia_lipsync_play()，每块之间检查
 *     s_interrupt_requested，被请求打断则立刻停；
 *   - 结束后关闭 WakeNet、结束口型，把结果写入 stream->result，并 give done。
 *
 * @param[in,out] arg 指向 stream_playback_t（在其生命周期内有效）。
 *
 * @note 该任务是动态创建的辅助任务；它调用 julia_voice_handle_event() 会去拿
 *       s_fsm_lock 更新状态（与生产者会话任务共享同一把锁）。
 */
static void stream_playback_task(void *arg)
{
    stream_playback_t *stream = arg;
    bool started = false;
    stream->result = ESP_OK;
    while (true) {
        stream_pcm_item_t item = {0};
        if (xQueueReceive(stream->queue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (item.end) break;
        if (stream->result == ESP_OK && item.pcm && item.samples) {
            if (!started) {
                julia_voice_handle_event(EVT_MULTI_TURN_DETECTED);
                /* 播报期间开 WakeNet：允许用户用新唤醒词打断上一段语音。 */
                s_afe->enable_wakenet(s_afe_data);
                s_interrupt_requested = false;
                julia_lipsync_begin();
                started = true;
                stream->played_any = true;
                ESP_LOGI(TAG, "First audio latency=%lldms",
                         esp_timer_get_time() / 1000 - stream->session_started_ms);
            }
            size_t remaining = item.samples * sizeof(int16_t);
            uint8_t *cursor = (uint8_t *)item.pcm;
            while (remaining && !s_interrupt_requested) {
                size_t chunk = remaining > 4096 ? 4096 : remaining;
                stream->result = julia_lipsync_play((const int16_t *)cursor,
                                                    chunk / sizeof(int16_t));
                if (stream->result != ESP_OK) break;
                cursor += chunk;
                remaining -= chunk;
            }
            /* 被打断时把结果标为"无效状态"，让上层按打断而不是按失败处理。 */
            if (s_interrupt_requested) stream->result = ESP_ERR_INVALID_STATE;
        }
        free(item.pcm);
    }
    if (started) {
        s_afe->disable_wakenet(s_afe_data);
        esp_err_t stop_err = julia_lipsync_end();
        if (stream->result == ESP_OK) stream->result = stop_err;
    }
    xSemaphoreGive(stream->done);
    vTaskDeleteWithCaps(NULL);
}

/**
 * @brief 对一段文本做 TTS，并把得到的 PCM 入队给流式播放任务。
 *
 * 这是"边合成边播报"的合成侧。职责：
 *   1) julia_speech_tts() 合成一段 PCM（产出自定缓冲，成功时需调用方释放）；
 *   2) 若播放任务尚未创建则创建（只在第一次成功时建一次）；
 *   3) 把 PCM 段封装成 stream_pcm_item_t 放入有界队列（容量 2），生产者侧
 *      用 15s 超时入队，队列满即失败——此时丢弃该段并返回超时。
 *
 * @param[in]  text         要合成的文本（非 NULL）。
 * @param[in]  stream       流式播放句柄。
 * @param[in]  task_started 播放任务是否已创建；函数可能按需置 true。
 * @return ESP_OK 已合成并入队；ESP_FAIL TTS 失败；ESP_ERR_TIMEOUT 队列满；
 *         ESP_ERR_NO_MEM 播放任务创建失败。
 *
 * @note TTS 后的 pcm 缓冲所有权转交队列；入队失败必须立即 free 防泄漏。
 */
static esp_err_t synthesize_and_queue_segment(const char *text, stream_playback_t *stream,
                                               bool *task_started)
{
    int16_t *pcm = NULL;
    size_t samples = 0;
    ESP_LOGI(TAG, "Streaming TTS segment: %u bytes", (unsigned)strlen(text));
    esp_err_t err = julia_speech_tts(text, &pcm, &samples);
    if (err != ESP_OK) { free(pcm); return err; }
    if (!*task_started) {
        if (xTaskCreateWithCaps(stream_playback_task, "voice_stream_play", 8192, stream, 6,
                                NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            free(pcm);
            return ESP_ERR_NO_MEM;
        }
        *task_started = true;
    }
    stream_pcm_item_t item = {.pcm = pcm, .samples = samples};
    if (xQueueSend(stream->queue, &item, pdMS_TO_TICKS(15000)) != pdTRUE) {
        free(pcm);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/**
 * @brief 用本地 TTS 分段朗读一段较长的回答（离线降级路径）。
 *
 * 本地 TTS 单次合成有长度上限，因此按 ≤700 字节切段；切段时确保不把
 * UTF-8 多字节字符拦腰截断（检查续字节掩码 0xc0/0x80）。任一段失败立即
 * 返回，不做部分播报。
 *
 * @param[in] text 要朗读的回答（非 NULL，可长）。
 * @return ESP_OK 全部段落朗读成功；ESP_ERR_INVALID_ARG 文本为空或切分异常；
 *         ESP_FAIL 某段本地 TTS 失败。
 *
 * @note 仅在 session_task 的降级分支中调用（云端输出链失败后）。
 */
static esp_err_t speak_local_chunked(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    const uint8_t *cursor = (const uint8_t *)text;
    size_t remaining = strlen(text);
    while (remaining) {
        size_t bytes = remaining > 700 ? 700 : remaining;
        /* 不在 UTF-8 多字节字符中间截断。 */
        while (bytes && bytes < remaining && (cursor[bytes] & 0xc0) == 0x80) --bytes;
        if (!bytes) return ESP_ERR_INVALID_ARG;
        char segment[704];
        memcpy(segment, cursor, bytes);
        segment[bytes] = '\0';
        esp_err_t err = julia_local_tts_speak(segment);
        if (err != ESP_OK) return err;
        cursor += bytes;
        remaining -= bytes;
    }
    return ESP_OK;
}

/**
 * @brief 一次完整语音会话：识别用户输入 -> 生成回答 -> 播报出声 -> 自删。
 *
 * 由 detect_task 在采集完成、判定"确有一段语音"后动态创建（栈内部 SRAM，
 * 因为会话中会写 NVS——见 detect_task 的说明）。单一实例运行：detect_task
 * 受 s_session_busy 门控，不会在会话结束前再开新一轮。
 *
 * 流程（每步都有失败回退与总时长闸门 SESSION_TIMED_OUT）：
 *   1) ASR：把采集的 PCM 识别成文本。首发失败且非"超时/未找到"则重试一次；
 *   2) 本地命令（status/time/standby）就地处理，直接结束；
 *   3) 情感关键词检测 -> 追加记忆、向 FSM 送 EVT_EMOTION_DETECTED；
 *   4) 记忆"遗忘"指令（julia_memory_handle_forget）则就地生成回答，不走 LLM；
 *   5) LLM（Qwen）：拼 system prompt（含记忆/配置提示）后发消息，流式收 chunk，
 *      按句切分成段，边收边 TTS 入队（流式播报）；
 *   6) 若未走流式（或流式不可用）则整体 TTS 后一次播报；
 *   7) done：三级输出降级（云端->本地 TTS->固化提示音），统一
 *      return_to_standby()、开回 WakeNet、清 s_session_busy，最后自删。
 *
 * @param[in] arg 未使用（本任务从全局 s_recording 提取数据）。
 *
 * @note 本任务结束会自删（vTaskDelete(NULL)）；调用方不得再引用其参数。
 * @note 会调用 julia_voice_handle_event()（拿 s_fsm_lock），与 detect/查询任务
 *       共享同一把锁，无锁序反转风险。
 */
static void session_task(void *arg)
{
    (void)arg;
    int64_t session_started_ms = esp_timer_get_time() / 1000;
#define SESSION_TIMED_OUT() ((esp_timer_get_time() / 1000 - session_started_ms) > VOICE_SESSION_MAX_MS)
    char user_text[1024] = {0};
    char answer[3072] = {0};
    int16_t *speech = s_recording;
    size_t samples = s_recorded_samples;
    bool response_played = false;
    voice_failure_t failure = VOICE_FAILURE_NONE;
    /* 采集缓冲一次性交接：取走后立即把全局清零，防止 detect_task
     * 在会话进行中向同一缓冲写入或释放。 */
    s_recording = NULL; s_recorded_samples = 0;

    ESP_LOGI(TAG, "ASR upload: %.2f seconds", (double)samples / JULIA_AUDIO_SAMPLE_RATE);
    // Move the avatar from listening to understanding while the request is decoded.
    julia_voice_handle_event(EVT_START_DIALOG);
    julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_THINKING);
    esp_err_t err = julia_speech_asr(speech, samples, user_text, sizeof(user_text));
    ESP_LOGI(TAG, "ASR elapsed=%lldms", esp_timer_get_time() / 1000 - session_started_ms);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "ASR first attempt failed: %s; retrying", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
        err = julia_speech_asr(speech, samples, user_text, sizeof(user_text));
    }
    free(speech);
    if (SESSION_TIMED_OUT()) { failure = VOICE_FAILURE_ASR; goto done; }
    if (err != ESP_OK) {
        failure = VOICE_FAILURE_ASR;
        ESP_LOGE(TAG, "ASR failed after retry: %s", esp_err_to_name(err));
        goto done;
    }
    ESP_LOGI(TAG, "User: %s", user_text);
    if (handle_local_command(user_text)) {
        response_played = true;
        goto done;
    }
    uint8_t memory_emotion = JULIA_MEMORY_EMOTION_NONE;
    const char *emotion_prompt = detect_emotion(user_text, &memory_emotion);
    bool streamed_reply = false;
    bool turn_recorded = false;
    if (emotion_prompt) {
        ESP_LOGI(TAG, "Emotion cue detected");
        julia_memory_append(1, memory_emotion, user_text);
        julia_voice_handle_event(EVT_EMOTION_DETECTED);
    }
    julia_voice_handle_event(EVT_START_DIALOG);
    if (!julia_memory_handle_forget(user_text, answer, sizeof(answer))) {
        if (!julia_wifi_is_connected()) {
            failure = VOICE_FAILURE_AI; ESP_LOGW(TAG, "WiFi unavailable"); goto done;
        }
        char system_prompt[4096];
#if 0
        const char *base_prompt =
            "你是 Julia，一个温柔、简洁的中文陪伴助手。回答适合语音朗读，"
            "通常不超过两句话。不要使用 Markdown。";
 #endif
        const char *base_prompt = "你是 Julia，一个温柔、简洁的中文陪伴助手。回答适合语音朗读，通常不超过两句话，不要使用 Markdown。";
        char configured_prompt[1024] = {0};
        if (julia_system_config_get("system_prompt", configured_prompt,
                                    sizeof(configured_prompt)) == ESP_OK &&
            configured_prompt[0]) {
            base_prompt = configured_prompt;
        }
        if (julia_memory_build_prompt(base_prompt, system_prompt, sizeof(system_prompt)) != ESP_OK)
            strlcpy(system_prompt, base_prompt, sizeof(system_prompt));
        if (emotion_prompt) {
            strlcat(system_prompt, "\n", sizeof(system_prompt));
            strlcat(system_prompt, emotion_prompt, sizeof(system_prompt));
        }
        err = julia_ai_send_message(user_text, system_prompt);
        if (err != ESP_OK) {
            failure = VOICE_FAILURE_AI;
            ESP_LOGE(TAG, "AI request failed: %s", esp_err_to_name(err)); goto done;
        }
        /* 走流式 LLM：先建好队列/信号量，再逐块收 chunk、按句切成段边合成边播。
         * stream_playback_t 同时供生产者（本任务）与消费者（stream_playback_task）读写。 */
        char pending[1024] = {0};
        stream_playback_t stream = {
            .queue = xQueueCreate(2, sizeof(stream_pcm_item_t)),
            .done = xSemaphoreCreateBinary(),
            .session_started_ms = session_started_ms,
        };
        bool playback_task_started = false;
        esp_err_t playback_err = ESP_OK;
        if (!stream.queue || !stream.done) {
            /* 队列/信号量任一创建失败都视为 TTS 链路不可用，回退离线输出。 */
            if (stream.queue) vQueueDelete(stream.queue);
            if (stream.done) vSemaphoreDelete(stream.done);
            failure = VOICE_FAILURE_TTS;
            err = ESP_ERR_NO_MEM;
            goto done;
        }
        /* 收流式 chunk，边收边把已完整的句子切出来 TTS；answer 作为全文备份，
         * pending 作为"尚未成句的尾部"累积，供下一次续切。 */
        while (strlen(answer) < sizeof(answer) - 1) {
            char chunk[256];
            err = julia_ai_receive_chunk(chunk, sizeof(chunk));
            if (err != ESP_OK) break;
            strlcat(answer, chunk, sizeof(answer));
            strlcat(pending, chunk, sizeof(pending));
            size_t segment_bytes;
            while ((segment_bytes = sentence_segment_bytes(pending, false)) > 0) {
                char segment[1024];
                memcpy(segment, pending, segment_bytes);
                segment[segment_bytes] = '\0';
                memmove(pending, pending + segment_bytes, strlen(pending + segment_bytes) + 1);
                playback_err = synthesize_and_queue_segment(segment, &stream,
                                                            &playback_task_started);
                if (playback_err != ESP_OK) break;
            }
            if (playback_err != ESP_OK) break;
        }
        /* ESP_ERR_TIMEOUT 作为"流已结束"的约定信号；此时取消请求是幂等清理，
         * 避免服务端残留半截会话。
         * NOTE：需结合调用方确认：julia_ai_receive_chunk() 的结束/超时返回约定
         * 是否确为 ESP_ERR_TIMEOUT（实现文件不在本目录内，仅据调用点推断）。 */
        if (err == ESP_ERR_TIMEOUT) {
            esp_err_t cancel_err = julia_ai_cancel_request();
            if (cancel_err != ESP_OK && cancel_err != ESP_ERR_INVALID_STATE)
                ESP_LOGW(TAG, "AI request cancellation failed: %s", esp_err_to_name(cancel_err));
        }
        /* 若收到流结束但 pending 里还有未成句的尾巴，作为最后一段补切。 */
        if (playback_err == ESP_OK && pending[0])
            playback_err = synthesize_and_queue_segment(pending, &stream,
                                                        &playback_task_started);
        ESP_LOGI(TAG, "AI elapsed=%lldms", esp_timer_get_time() / 1000 - session_started_ms);
        if (playback_task_started) {
            /* 放一个结束哨兵，等播放任务播完（semaphore done），再取回其结果。 */
            stream_pcm_item_t end = {.end = true};
            xQueueSend(stream.queue, &end, portMAX_DELAY);
            xSemaphoreTake(stream.done, portMAX_DELAY);
            if (playback_err == ESP_OK) playback_err = stream.result;
            response_played = stream.played_any && playback_err == ESP_OK;
            streamed_reply = true;
            if (playback_err != ESP_OK) {
                /* 区分"根本没出声"（TTS 失败）与"出声后失败"（播放失败）。 */
                failure = stream.played_any ? VOICE_FAILURE_PLAYBACK : VOICE_FAILURE_TTS;
                ESP_LOGE(TAG, "Streaming playback failed: %s", esp_err_to_name(playback_err));
            }
        }
        vQueueDelete(stream.queue);
        vSemaphoreDelete(stream.done);
    }
    if (!answer[0]) {
        failure = VOICE_FAILURE_AI; ESP_LOGE(TAG, "AI returned no text"); goto done;
    }
    if (SESSION_TIMED_OUT()) { failure = VOICE_FAILURE_AI; goto done; }
    ESP_LOGI(TAG, "Julia: %s", answer);
    /* 若走了流式播报，则记录对话轮次，并在"确实出声"的前提下开启追话窗口
     * （冷却 FOLLOWUP_COOLDOWN_MS 后才允许被追话 VAD 重新触发）。 */
    if (streamed_reply) {
        julia_memory_record_turn_with_emotion(user_text, answer, memory_emotion);
        turn_recorded = true;
        if (response_played) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            s_followup_ready_ms = now_ms + FOLLOWUP_COOLDOWN_MS;
            s_followup_until_ms = now_ms + FOLLOWUP_WINDOW_MS;
        }
        goto done;
    }
    // Keep the dialog animation active while cloud or offline speech is rendered.
    julia_voice_handle_event(EVT_MULTI_TURN_DETECTED);
    if (!turn_recorded) julia_memory_record_turn_with_emotion(user_text, answer, memory_emotion);
    /* 未采用流式播报：把整段回答一次性 TTS 后播放（非流式整包路径）。
     * 同样优先开 WakeNet 以允许播报中被唤醒词打断。 */
    int16_t *reply_pcm = NULL; size_t reply_samples = 0;
    ESP_LOGI(TAG, "Voice stack before TTS: %u bytes free",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    err = julia_speech_tts(answer, &reply_pcm, &reply_samples);
    ESP_LOGI(TAG, "TTS elapsed=%lldms", esp_timer_get_time() / 1000 - session_started_ms);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "TTS first attempt failed: %s; retrying", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
        err = julia_speech_tts(answer, &reply_pcm, &reply_samples);
    }
    if (SESSION_TIMED_OUT()) { free(reply_pcm); failure = VOICE_FAILURE_TTS; goto done; }
    if (err != ESP_OK) {
        failure = VOICE_FAILURE_TTS;
        ESP_LOGE(TAG, "TTS failed after retry: %s", esp_err_to_name(err)); goto done;
    }
    ESP_LOGI(TAG, "Playing %.2f seconds", (double)reply_samples / JULIA_AUDIO_SAMPLE_RATE);
    // Keep WakeNet active only during playback so a new wake word can interrupt TTS.
    s_afe->enable_wakenet(s_afe_data);
    s_interrupt_requested = false;
                julia_lipsync_begin();
                julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_SPEAKING);
    size_t remaining = reply_samples * sizeof(int16_t);
    uint8_t *cursor = (uint8_t *)reply_pcm;
    while (remaining && !s_interrupt_requested) {
        size_t chunk = remaining > 4096 ? 4096 : remaining;
        err = julia_lipsync_play((const int16_t *)cursor, chunk / sizeof(int16_t));
        if (err != ESP_OK) break;
        cursor += chunk;
        remaining -= chunk;
    }
    if (s_interrupt_requested) err = ESP_ERR_INVALID_STATE;
    s_afe->disable_wakenet(s_afe_data);
    esp_err_t stop_err = julia_lipsync_end();
    if (err == ESP_OK) err = stop_err;
    free(reply_pcm);
    if (err != ESP_OK) {
        failure = VOICE_FAILURE_PLAYBACK;
        ESP_LOGE(TAG, "Playback failed: %s", esp_err_to_name(err));
    }
    else {
        response_played = true;
        int64_t now_ms = esp_timer_get_time() / 1000;
        s_followup_ready_ms = now_ms + FOLLOWUP_COOLDOWN_MS;
        s_followup_until_ms = now_ms + FOLLOWUP_WINDOW_MS;
        ESP_LOGI(TAG, "Follow-up window open for %dms", FOLLOWUP_WINDOW_MS);
    }

done:
    /* 会话结束统一收尾：三级输出降级（见下），保证"尽量让用户听到点什么"，
     * 随后 return_to_standby() 复位状态，最后开回 WakeNet 并自删。 */
    /* 三级输出链：云端 PCM 已在上方尝试；未播放任何内容时再尝试本地
     * TTS。只有扬声器播放本身失败时才跳过本地，避免重复走同一故障。 */
    if (!response_played && failure != VOICE_FAILURE_PLAYBACK) {
        const char *local_text = answer[0] ? answer : "网络好像不太好，请稍后再试。";
        julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_SPEAKING);
        esp_err_t local_err = speak_local_chunked(local_text);
        if (local_err == ESP_OK) {
            response_played = true;
            ESP_LOGI(TAG, "Voice fallback selected: local TTS");
        } else {
            ESP_LOGW(TAG, "Local TTS fallback unavailable: %s", esp_err_to_name(local_err));
        }
    }
    if (!response_played) {
        const char *offline_text = "网络暂时不可用";
        if (failure == VOICE_FAILURE_ASR) offline_text = "没有听清楚，请再说一次";
        else if (failure == VOICE_FAILURE_TTS) offline_text = "语音生成失败，请稍后再试";
        else if (failure == VOICE_FAILURE_PLAYBACK) offline_text = "扬声器播放失败";
        bool network_failure = !julia_wifi_is_connected() || err == ESP_ERR_HTTP_CONNECT;
        esp_err_t prompt_err = ESP_FAIL;
        if (network_failure) {
            /* This cached sentence explicitly says the network is unstable;
             * never use it for ASR, TTS or speaker failures. */
    julia_lipsync_begin();
    julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_SPEAKING);
            prompt_err = julia_lipsync_play_file(NETWORK_PROMPT_PATH);
            esp_err_t stop_err = julia_lipsync_end();
            if (prompt_err == ESP_OK) prompt_err = stop_err;
        } else {
            /* 本地 TTS 不可用时仍播放固化 PCM，不能静音卡死。 */
            ESP_LOGW(TAG, "Non-network failure feedback: %s", offline_text);
            julia_lipsync_begin();
            julia_ui_set_dialog_phase(JULIA_DIALOG_PHASE_SPEAKING);
            prompt_err = julia_lipsync_play_file(NETWORK_PROMPT_PATH);
            esp_err_t stop_err = julia_lipsync_end();
            if (prompt_err == ESP_OK) prompt_err = stop_err;
        }
        if (prompt_err != ESP_OK) play_failure_code(failure ? failure : VOICE_FAILURE_AI);
    }
    return_to_standby();
    ESP_LOGI(TAG, "Voice session complete: total=%lldms failure=%d",
             esp_timer_get_time() / 1000 - session_started_ms, failure);
    ESP_LOGI(TAG, "Voice heap free=%u min=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    s_wake_resume_ms = esp_timer_get_time() / 1000 + 1000;
    s_afe->enable_wakenet(s_afe_data);
    s_session_busy = false;
    ESP_LOGI(TAG, "Ready: say %s", WAKE_WORD_DISPLAY_TEXT);
    vTaskDelete(NULL);
}

/**
 * @brief 麦克风 -> AFE 的喂数任务（绑定 core0/p6）。
 *
 * 从 julia_audio 读 20ms 块，按 AFE 要求的大小（get_feed_chunksize）送入
 * AFE。AFE 内部做前处理（回声/语音增强被关闭，AEC/SE=false），只保留
 * WakeNet 与 VAD。每 100 帧打印一次电平做诊断。
 *
 * @note 这是进入本任务后就不再退出的常驻任务；buffer 分配失败则自删退出。
 *       本任务不参与任何状态/锁，只做"读麦克风、喂 AFE"。
 */
static void feed_task(void *arg)
{
    (void)arg;
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int16_t *samples = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!samples) { ESP_LOGE(TAG, "feed buffer allocation failed"); vTaskDelete(NULL); return; }
    unsigned diagnostic_frames = 0;
    while (true) {
        size_t read = 0;
        if (julia_audio_mic_read(samples, chunk, &read) == ESP_OK && read == (size_t)chunk) {
            s_afe->feed(s_afe_data, samples);
            if (++diagnostic_frames >= 100) {
                int peak = 0; uint64_t energy = 0;
                for (int i = 0; i < chunk; ++i) {
                    int value = abs(samples[i]);
                    if (value > peak) peak = value;
                    energy += (uint64_t)value * value;
                }
                ESP_LOGI(TAG, "Mic level: peak=%d rms=%u gain=%dx", peak,
                         (unsigned)sqrt((double)energy / chunk), JULIA_AUDIO_MIC_GAIN);
                diagnostic_frames = 0;
            }
        }
    }
}

/**
 * @brief AFE 结果检测/采集任务（绑定 core1/p5）。
 *
 * 这是整个语音管线的"识别-采集"中枢：
 *   1) 拉取 AFE fetch 结果；空闲时做噪声自适应（仅当非会话、非语音时平滑
 *      环境底噪/底峰），并追踪最近语音活动时间；
 *   2) 判唤醒词/追话：命中且会话空闲、未在采集时，触发一次交互——关 WakeNet、
 *      通知 routine/微动作、向 FSM 送 EVT_USER_CALL、播放唤醒应答，然后分配
 *      s_recording 进入采集；命中且会话忙时，作为打断请求；
 *   3) 采集期间：把帧拷贝进 s_recording，按"能量门限 + VAD"双判据确认语音起止，
 *      达到结束条件（满时长/说完+静音/一直没话超时）后收尾；
 *   4) 采集完成：对语音起点做前后滚动（pre/post roll）裁剪，置 s_session_busy，
 *      递增对话轮数，创建 session_task 进入识别-生成-播放。
 *
 * @param[in] arg 未使用。
 *
 * @note 状态机事件在此任务中投递（julia_voice_handle_event）；它也是唯一会
 *       创建 session_task 的任务，结束后受 s_session_busy 门控，单会话运行。
 */
static void detect_task(void *arg)
{
    (void)arg;
    int chunk = s_afe->get_fetch_chunksize(s_afe_data);
    int frame_ms = chunk * 1000 / JULIA_AUDIO_SAMPLE_RATE;
    bool capturing = false, heard_speech = false;
    int silence_ms = 0, waiting_ms = 0, followup_vad_frames = 0;
    int capture_log_ms = 0, energy_speech_frames = 0;
    uint32_t noise_rms = 180, noise_peak = 500;
    uint32_t capture_rms_threshold = SPEECH_RMS_THRESHOLD;
    uint32_t capture_peak_threshold = SPEECH_PEAK_THRESHOLD;
    size_t first_speech_sample = SIZE_MAX, last_speech_sample = 0;
    while (true) {
        afe_fetch_result_t *result = s_afe->fetch(s_afe_data);
        if (!result || result->ret_value == ESP_FAIL) continue;
        size_t result_samples = result->data_size / sizeof(int16_t);
        int frame_peak = 0;
        uint64_t frame_energy = 0;
        for (size_t i = 0; i < result_samples; ++i) {
            int value = abs(result->data[i]);
            if (value > frame_peak) frame_peak = value;
            frame_energy += (uint64_t)value * value;
        }
        uint32_t frame_rms = result_samples
                                 ? (uint32_t)sqrt((double)frame_energy / result_samples)
                                 : 0;
        /* 噪声自适应：仅在"空闲、非语音、无会话"时用低通平滑吸收环境底噪/底峰，
         * 作为后续语音/追话的能量基准。会话或语音进行中不更新，防止把用户语音
         * 当作底噪。 */
        if (!capturing && !s_session_busy && result->vad_state != AFE_VAD_SPEECH &&
            frame_rms < noise_rms * 3U) {
            noise_rms = (noise_rms * 31U + frame_rms) / 32U;
            noise_peak = (noise_peak * 31U + (uint32_t)frame_peak) / 32U;
        }
        /* VAD 报告语音时刷新"最近语音活动"时间戳（供 last_audio_activity 查询）。 */
        if (result->vad_state == AFE_VAD_SPEECH)
            s_last_audio_activity_ms = esp_timer_get_time() / 1000;
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool followup_active = now_ms >= s_followup_ready_ms && now_ms < s_followup_until_ms;
        /* Never accumulate follow-up VAD while a session or TTS playback is
         * still active. Otherwise Julia's own speaker audio preloads the
         * counter and starts a new listening round as soon as the task ends. */
        if (!s_session_busy && followup_active && result->vad_state == AFE_VAD_SPEECH)
            ++followup_vad_frames;
        else followup_vad_frames = 0;
        bool followup_detected = followup_active && followup_vad_frames >= 3;
        bool wake_detected = result->wakeup_state == WAKENET_DETECTED && now_ms >= s_wake_resume_ms;
        if (s_session_busy && wake_detected) {
            // A wake word during playback is treated as an interrupt request.
            julia_voice_interrupt();
            continue;
        }
        /* 触发一轮交互：命中唤醒词，或命中追话（且尚未达到对话轮数上限）。
         * 这里集中做了：关 WakeNet、上报 routine/UI、送 EVT_USER_CALL 让状态机
         * 进入 S3.3（呼叫），再分配采集缓冲并开始"听"用户。 */
        if (!s_session_busy && !capturing && (wake_detected ||
            (followup_detected && s_dialog_rounds < MAX_DIALOG_ROUNDS))) {
            ESP_LOGI(TAG, "%s detected", wake_detected ? "Wake word" : "Follow-up speech");
            julia_routine_on_activity(JULIA_ACTIVITY_WAKE);
            s_afe->disable_wakenet(s_afe_data);
            on_user_interaction();
            julia_voice_handle_event(EVT_USER_CALL);
            s_followup_until_ms = 0; followup_vad_frames = 0;
            if (wake_detected) s_dialog_rounds = 0;
            if (wake_detected) {
                esp_err_t response_err = wake_reply_play();
                if (response_err != ESP_OK) {
                    ESP_LOGW(TAG, "Offline wake reply unavailable: %s",
                             esp_err_to_name(response_err));
                    julia_audio_play_tone(740, 70, 25);
                }
                ESP_LOGI(TAG, "Wake reply playback finished result=%s; starting capture",
                         esp_err_to_name(response_err));
            } else {
                julia_audio_play_tone(740, 70, 25);
            }
            s_recording = heap_caps_malloc(MAX_RECORD_SAMPLES * sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_recording) { s_afe->enable_wakenet(s_afe_data); continue; }
            s_recorded_samples = 0; capturing = true; heard_speech = false;
            silence_ms = 0; waiting_ms = 0; capture_log_ms = 0;
            energy_speech_frames = 0;
            first_speech_sample = SIZE_MAX; last_speech_sample = 0;
            /* 采集阈值相对自适应底噪抬高（噪声的 2 倍），并保底不小于默认阈值，
             * 兼顾"安静处更灵敏"与"喧闹处更鲁棒"。 */
            capture_rms_threshold = noise_rms * 2U;
            if (capture_rms_threshold < SPEECH_RMS_THRESHOLD)
                capture_rms_threshold = SPEECH_RMS_THRESHOLD;
            capture_peak_threshold = noise_peak * 2U;
            if (capture_peak_threshold < SPEECH_PEAK_THRESHOLD)
                capture_peak_threshold = SPEECH_PEAK_THRESHOLD;
            ESP_LOGI(TAG, "Listening... noise=%u/%u threshold=%u/%u",
                     (unsigned)noise_peak, (unsigned)noise_rms,
                     (unsigned)capture_peak_threshold, (unsigned)capture_rms_threshold);
        }
        if (!capturing) continue;
        size_t frame_samples = result_samples;
        if (frame_samples > MAX_RECORD_SAMPLES - s_recorded_samples)
            frame_samples = MAX_RECORD_SAMPLES - s_recorded_samples;
        memcpy(s_recording + s_recorded_samples, result->data, frame_samples * sizeof(int16_t));
        s_recorded_samples += frame_samples; waiting_ms += frame_ms;
        bool energy_speech = (uint32_t)frame_peak >= capture_peak_threshold ||
                             frame_rms >= capture_rms_threshold;
        if (energy_speech) {
            if (energy_speech_frames < SPEECH_CONFIRM_FRAMES) energy_speech_frames++;
        } else {
            energy_speech_frames = 0;
        }
        bool confirmed_energy_speech = energy_speech_frames >= SPEECH_CONFIRM_FRAMES;
        capture_log_ms += frame_ms;
        if (capture_log_ms >= 500) {
            ESP_LOGI(TAG, "Listening level: peak=%d rms=%u vad=%d heard=%d",
                     frame_peak, (unsigned)frame_rms,
                     result->vad_state == AFE_VAD_SPEECH, heard_speech);
            capture_log_ms = 0;
        }
        bool vad_above_noise = result->vad_state == AFE_VAD_SPEECH &&
                               frame_rms >= noise_rms + noise_rms / 3U;
        bool speech_evidence = confirmed_energy_speech || vad_above_noise;
        if (speech_evidence) {
            if (!heard_speech) ESP_LOGI(TAG, "Speech started: peak=%d rms=%u vad=%d",
                                        frame_peak, (unsigned)frame_rms,
                                        result->vad_state == AFE_VAD_SPEECH);
            if (first_speech_sample == SIZE_MAX)
                first_speech_sample = s_recorded_samples - frame_samples;
            last_speech_sample = s_recorded_samples;
            heard_speech = true; silence_ms = 0;
        }
        else if (heard_speech) silence_ms += frame_ms;
        /* 结束条件三选一：1) 采集达到上限；2) 已听到语音且过了最短时长又出现
         * 连续静音（说完一句）；3) 一直没话且等待超时（放弃）。 */
        bool finish = s_recorded_samples >= MAX_RECORD_SAMPLES ||
                      (heard_speech && waiting_ms >= MIN_CAPTURE_MS && silence_ms >= END_SILENCE_MS) ||
                      (!heard_speech && waiting_ms >= NO_SPEECH_TIMEOUT_MS);
        if (!finish) continue;
        capturing = false;
        /* 说话起止处做前后滚动，保留一点上下文，避免语音被硬切出爆音：
         * 前滚 200ms、后滚 250ms，并把窗口夹在缓冲范围内。 */
        if (heard_speech && first_speech_sample != SIZE_MAX && last_speech_sample > first_speech_sample) {
            const size_t pre_roll = JULIA_AUDIO_SAMPLE_RATE / 5;
            const size_t post_roll = JULIA_AUDIO_SAMPLE_RATE / 4;
            size_t start = first_speech_sample > pre_roll ? first_speech_sample - pre_roll : 0;
            size_t end = last_speech_sample + post_roll;
            if (end > s_recorded_samples) end = s_recorded_samples;
            size_t original_samples = s_recorded_samples;
            memmove(s_recording, s_recording + start, (end - start) * sizeof(int16_t));
            s_recorded_samples = end - start;
            ESP_LOGI(TAG, "Capture trimmed: %.2fs -> %.2fs",
                     (double)original_samples / JULIA_AUDIO_SAMPLE_RATE,
                     (double)s_recorded_samples / JULIA_AUDIO_SAMPLE_RATE);
        }
        if (!heard_speech) {
            /* This enclosure's AFE VAD can remain silent even when WakeNet
             * and raw microphone levels clearly detect the user. Preserve
             * the fixed capture and let cloud ASR make the final decision. */
            ESP_LOGW(TAG, "VAD did not confirm speech; submitting %dms fallback capture",
                     waiting_ms);
        }
        /* 有语音可提交：置会话忙，递增对话轮数（超上限则清空并关追话窗口），
         * 再创建 session_task 去识别-生成-播报。 */
        s_session_busy = true;
        if (++s_dialog_rounds > MAX_DIALOG_ROUNDS) {
            ESP_LOGW(TAG, "dialog round limit reached");
            s_dialog_rounds = 0;
            s_followup_until_ms = 0;
        }
        ESP_LOGI(TAG, "Capture complete: %dms, %u samples", waiting_ms,
                 (unsigned)s_recorded_samples);
        /* This task records conversation metadata in NVS. NVS flash access
         * temporarily disables the external-memory cache, so its stack must
         * live in internal SRAM (an external PSRAM stack asserts in IDF's
         * cache safety check). Core dumps show <12 KB peak usage. */
        if (xTaskCreate(session_task, "voice_session", 24576, NULL, 5, NULL) != pdPASS) {
            free(s_recording); s_recording = NULL; s_recorded_samples = 0; s_session_busy = false;
            return_to_standby(); s_afe->enable_wakenet(s_afe_data);
        }
    }
}

/**
 * @brief 初始化本地语音管线并拉起 feed/detect 任务。
 *
 * 顺序约束：本函数要求 DashScope 密钥已配置；开麦 + 建 CPU 频率锁 + 初始
 * Qwen 客户端都要放在 AFE 创建之前。任何步骤失败即返回对应错误码（用
 * ESP_RETURN_ON_FALSE/ON_ERROR 短路），不会留下半初始化状态。
 *
 * @return ESP_OK 成功，voice_feed/voice_detect 已在运行。
 * @return ESP_ERR_INVALID_STATE 密钥为空。
 * @return ESP_ERR_NOT_FOUND 模型分区/唤醒模型不可用。
 * @return ESP_ERR_NO_MEM AFE、互斥量或任务创建失败。
 *
 * @note 启动早期（网络尚未就绪）调用；装配顺序：mic 在 Qwen 前、AFE 在
 *       建锁后。注意本模块依靠 s_session_busy 门控单个会话，init 不应重复调用。
 */
esp_err_t julia_voice_init(void)
{
    /* 密钥为空则意味着无法进行云端 ASR/LLM/TTS，直接拒绝启动本地语音管线。 */
    ESP_RETURN_ON_FALSE(CONFIG_JULIA_AI_API_KEY[0], ESP_ERR_INVALID_STATE, TAG, "DashScope key is empty");
    /* 开麦：必须最先进行，后续 AFE 依赖实时的麦克风数据。 */
    ESP_RETURN_ON_ERROR(julia_audio_mic_start(), TAG, "start microphone");
    /* 语音识别/前处理会消耗大量 CPU 周期，锁定 CPU 频率上限以稳定时延。 */
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "voice_sr", &s_cpu_lock),
                        TAG, "create CPU frequency lock");
    ESP_RETURN_ON_ERROR(esp_pm_lock_acquire(s_cpu_lock), TAG, "lock CPU frequency");
    /* 初始化并启动 Qwen（LLM）客户端，供后续会话使用。 */
    ESP_RETURN_ON_ERROR(julia_ai_init_qwen(CONFIG_JULIA_AI_API_KEY), TAG, "initialize Qwen");
    ESP_RETURN_ON_ERROR(julia_ai_chat_start(), TAG, "start Qwen chat");
    /* 注册"智能家居"工具函数；失败不致命，仅少一些原生能力。 */
    esp_err_t home_err = julia_home_register_ai_functions();
    if (home_err != ESP_OK) ESP_LOGW(TAG, "home tools unavailable: %s", esp_err_to_name(home_err));
    /* 从 "model" 分区加载模型列表，并过滤出指定名称的 WakeNet 模型。 */
    s_models = esp_srmodel_init(ACTIVE_WAKENET_MODEL_PARTITION);
    ESP_RETURN_ON_FALSE(s_models, ESP_ERR_NOT_FOUND, TAG, "speech model partition unavailable");
    char *wake_model = esp_srmodel_filter(s_models, ESP_WN_PREFIX,
                                          ACTIVE_WAKENET_MODEL_NAME);
    ESP_RETURN_ON_FALSE(wake_model, ESP_ERR_NOT_FOUND, TAG, "WakeNet model unavailable");
    /* AFE 配置：单麦、无回声（AEC）无分离（SE），只保留 WakeNet + VAD。
     * 唤醒模型来自过滤出的 wake_model；VAD 用较宽松的 mode 0。 */
    afe_config_t config = AFE_CONFIG_DEFAULT();
    config.aec_init = false; config.se_init = false;
    config.vad_init = true; config.wakenet_init = true;
    /* The default mode 3 rejected normal speech on this enclosure. Mode 0 is
     * less restrictive; the consecutive-frame energy gate still suppresses
     * isolated ambient spikes. */
    config.vad_mode = VAD_MODE_0;
    config.wakenet_model_name = wake_model; config.afe_ringbuf_size = 50;
    config.wakenet_mode = DET_MODE_95;
    config.pcm_config.total_ch_num = 1; config.pcm_config.mic_num = 1; config.pcm_config.ref_num = 0;
    s_afe = &ESP_AFE_SR_HANDLE;
    s_afe_data = s_afe->create_from_config(&config);
    ESP_RETURN_ON_FALSE(s_afe_data, ESP_ERR_NO_MEM, TAG, "create AFE failed");
    /* FSM 互斥量 + 实例：锁保护 s_fsm，on_enter 接 voice_on_enter 驱动 UI。 */
    s_fsm_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_fsm_lock, ESP_ERR_NO_MEM, TAG, "create FSM lock failed");
    julia_fsm_init(&s_fsm); s_fsm.on_enter = voice_on_enter;
    s_last_audio_activity_ms = esp_timer_get_time() / 1000;
    /* 两个常驻任务：feed 在 core0（读麦喂 AFE），detect 在 core1（拉结果/采集）。
     * detect 创建失败时回收 feed 再返回，避免挂着半套管线。 */
    TaskHandle_t feed_handle = NULL;
    if (xTaskCreatePinnedToCore(feed_task, "voice_feed", 4096, NULL, 6,
                                &feed_handle, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(detect_task, "voice_detect", 6144, NULL, 5,
                                NULL, 1) != pdPASS) {
        vTaskDelete(feed_handle);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Voice pipeline ready model=%s wake_word=%s heap=%u psram=%u",
             wake_model, WAKE_WORD_DISPLAY_TEXT,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}
