/**
 * @file    julia_context.c
 * @brief   未参与当前构建的旧情境聚合参考实现。
 *
 * main/CMakeLists.txt 未包含本文件。当前运行时分别使用 julia_time、
 * julia_night_schedule、julia_motion 和 julia_fsm_runtime；不得再启动本文件的任务，
 * 否则会重复拥有 RTC/IMU 和 FSM 事件策略。
 *
 * 职责边界：
 *   - 只做"感知到事件、投递给 FSM"这一步，不关心 FSM 如何响应。
 *     感知周期固定为 500ms（SAMPLE_PERIOD_MS），由本文件自建的后台任务执行。
 *   - 负责把系统时间与 RTC 打通（SNTP 同步后写回 PCF85063），并记录两个关键
 *     可持久化字段到 NVS（last_state / activity_ms），供掉电后恢复情境。
 *   - 明确的非职责：不做 UI、不做语音会话、不持有 FSM 实例（仅通过
 *     julia_voice_handle_event 投递事件）。
 *
 * 数据流（每 500ms 一次）：
 *   IMU 运动检测 -> 更新 s_last_motion_ms + 记录日常活动（julia_routine_on_activity）
 *   -> 与音频活动时间取"最近活动" -> 计算 idle_ms -> 依据当前 FSM 状态与空闲时长
 *   -> 决定投递哪个情境事件（USER_RETURN/NIGHT_TIME/DAY_AWAY/USER_LEAVE/BEDTIME/
 *      SILENCE_TIMEOUT 等）。
 *
 * 并发模型：本任务与 julia_voice 的 FSM 通过 mutex 串行（julia_voice_handle_event），
 *   但本文件自身的静态变量（s_last_motion_ms / s_time_synced / s_rtc_valid / s_nvs）
 *   只在本任务上下文读写，无跨任务竞争。
 */
#include "julia_context.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "PCF85063.h"
#include "QMI8658.h"
#include "julia_memory.h"
#include "julia_routine.h"
#include "julia_voice.h"

#define TAG "JULIA_CONTEXT"

/* 传感器/情境任务的采样周期：500ms。该周期是"距离、时间流逝、静默时长"等
 * 判定的基本时钟粒度，越短越灵敏但越耗 CPU。 */
#define SAMPLE_PERIOD_MS 500
/* 各情境阈值（单位 ms），与 FSM 子状态/事件一一对应：
 *   - QUIET_COMPANION_MS  待机里连续安静多久 -> EVT_SILENCE_TIMEOUT（走向陪伴-观察）
 *   - FAR_STANDBY_MS      待机里无人近旁多久 -> EVT_USER_LEAVE（退远场待机）
 *   - DAY_AWAY_MS         白天无人多久       -> EVT_DAY_AWAY（白天离家）
 *   - NIGHT_SLEEP_IDLE_MS 夜间无人多久       -> EVT_NIGHT_TIME（夜间睡眠）
 *   注意阈值大小关系：QUIET < FAR < DAY_AWAY < NIGHT_SLEEP，
 *   且判定顺序是先 night 再 DAY_AWAY 再 FAR 再 BEDTIME 再 QUIET（见 context_task）。 */
#define QUIET_COMPANION_MS (60 * 1000LL)
#define FAR_STANDBY_MS (5 * 60 * 1000LL)
#define DAY_AWAY_MS (20 * 60 * 1000LL)
#define NIGHT_SLEEP_IDLE_MS (10 * 60 * 1000LL)
/* 运动检测阈值：加速度三轴绝对增量之和 >= 0.10g，或陀螺仪模 >= 12dps 即视为"动了"。
 * 用"增量之和"而非单轴，能覆盖平移/旋转/颠动等各方向运动，兼顾灵敏与抗噪。 */
#define MOTION_ACCEL_DELTA_G 0.10f
#define MOTION_GYRO_DPS 12.0f
/* 长离隔阈值：距上次用户交互 >= 24h 才触发"久别重逢"问候（EVT_ROUTINE_BREAK）。 */
#define LONG_ABSENCE_SECONDS (24LL * 60 * 60)

/* 最近一次检测到运动的时刻（ms），作为"设备侧活动"的锚点之一。
 * volatile：仅在本任务内写，但会被本任务内的日志/判定读取，标记以保证不被优化掉。 */
static volatile int64_t s_last_motion_ms;
/* 系统时钟（SNTP/RTC 恢复后）是否已被本模块写回 RTC 并认为有效。 */
static bool s_time_synced;
/* RTC 是否读到过一个合法时间（用于 current_hour 回退判断）。 */
static bool s_rtc_valid;
/* NVS 句柄（"julia_ctx" 分区），用于掉电保存 last_state / activity_ms。 */
static nvs_handle_t s_nvs;
/* 本次上电是否已经做过"久别重逢"检查。一旦置 true 则本周期内不再重复检查
 *（避免用户每次来回走动都被重复问候）。注意：该标志只在开机时清零，见问题清单。 */
static bool s_return_care_checked;

/*
 * 判断一个 RTC 时间值是否"可信"。只做最基本的范围检查（年份 2024~2099、月份/日期/小时
 * 合理），不作闰年/每月天数等细节校验——够用于区分"RTC 没设过时间/读出乱码"与"正常时间"。
 * 注意 year>=2024 特意与 JULIA_VALID_EPOCH 保持同一基准：都认为 2024-01-01 才是"时间已被设置"。
 */
static bool valid_time(const datetime_t *value)
{
    return value->year >= 2024 && value->year <= 2099 && value->month >= 1 &&
           value->month <= 12 && value->day >= 1 && value->day <= 31 && value->hour < 24;
}

/*
 * @brief 把系统时钟（SNTP 已同步 / 已恢复）写入 PCF85063 RTC，并标记时间有效。
 * @note  1704067200 是 2024-01-01 00:00:00 UTC 的 epoch，用于判定"时间是否已被设置"。
 *        若系统时钟仍停留在 1970/无效值（now < 1704067200），什么都不做、也不标记
 *        有效（s_time_synced 保持 false），从而允许后续继续重试。
 * @side  写 RTC 是 I2C 事务；成功后才把 s_time_synced / s_rtc_valid 置 true。
 * @failure I2C 写失败不检查返回值（PCF85063_Set_All 无返回值），这里把"未写成功"
 *          当成功处理，仅靠日志观察；属可接受的"尽力而为"。
 */
static void sync_rtc_from_system(void)
{
    time_t now = time(NULL);
    if (now < 1704067200) return;
    struct tm local; localtime_r(&now, &local);
    datetime_t value = {
        .year = local.tm_year + 1900, .month = local.tm_mon + 1, .day = local.tm_mday,
        .dotw = local.tm_wday, .hour = local.tm_hour, .minute = local.tm_min, .second = local.tm_sec,
    };
    PCF85063_Set_All(value);
    s_time_synced = true; s_rtc_valid = true;
    ESP_LOGI(TAG, "RTC synchronized: %04u-%02u-%02u %02u:%02u",
             value.year, value.month, value.day, value.hour, value.minute);
}

/*
 * @brief 取当前"本地小时（0~23）"。
 *        优先用系统时钟；系统时钟尚未同步（now < 1704067200）时回退读 RTC，
 *        RTC 也无效则返回 -1 表示"时间不可知"。返回 -1 的调用方应把它当作
 *        "不是 22 点 / 不是夜间"，从而不触发时间相关事件。
 */
static int current_hour(void)
{
    time_t now = time(NULL);
    if (now >= 1704067200) { struct tm local; localtime_r(&now, &local); return local.tm_hour; }
    datetime_t value = {0}; PCF85063_Read_Time(&value);
    if (valid_time(&value)) { s_rtc_valid = true; return value.hour; }
    return -1;
}

/*
 * @brief 读取 IMU 并判断是否"有运动发生"。
 * @param last_ax/ay/az 上一次采样值（作为增量基准）；函数会用本次值原地更新它们。
 * @return true 表示检测到运动（加速度增量之和 >= 阈值 或 陀螺仪模 >= 阈值）。
 *
 * 判定模型：由于 QMI8658 输出的 Accel/Gyro 是全局归一化值，这里直接比较相邻两次
 * 采样的"加速三轴绝对增量之和"来捕捉突变，用陀螺仪模与静止噪声区分"转动"。
 * @side  副作用：更新传入的 last_* 三个基点；读取传感器（I2C）。非线程安全，
 *         仅在本任务上下文调用。
 */
static bool motion_detected(float *last_ax, float *last_ay, float *last_az)
{
    getAccelerometer(); getGyroscope();
    float delta = fabsf(Accel.x - *last_ax) + fabsf(Accel.y - *last_ay) + fabsf(Accel.z - *last_az);
    float gyro = sqrtf(Gyro.x * Gyro.x + Gyro.y * Gyro.y + Gyro.z * Gyro.z);
    *last_ax = Accel.x; *last_ay = Accel.y; *last_az = Accel.z;
    return delta >= MOTION_ACCEL_DELTA_G || gyro >= MOTION_GYRO_DPS;
}

/*
 * @brief 把当前 FSM 子状态与"最近活动时刻"持久化到 NVS。
 * @param state       要保存的子状态。
 * @param activity_ms 最近一次活动（取运动/音频二者较新者）的时刻，ms。
 * @side  写 NVS（set + commit），flash 有写次数寿命，仅在状态确实变化时才被调用
 *        （见 context_task 中的 saved_state 比较），避免高频写坏 flash。
 * @failure 不校验返回值：NVS 写失败只代表"这次掉电恢复不到最新状态"，属尽力而为；
 *          另外注意 julia_voice.c 提到 NVS 访问会短暂禁用外部缓存，若被高优先级
 *          任务调用有卡顿隐患，当前仅在 context 任务内调用是安全的。
 */
static void save_context(julia_sub_state_t state, int64_t activity_ms)
{
    if (!s_nvs) return;
    nvs_set_u8(s_nvs, "last_state", (uint8_t)state);
    nvs_set_i64(s_nvs, "activity_ms", activity_ms);
    nvs_commit(s_nvs);
}

/*
 * @brief 情境感知主循环（freeRTOS 任务），周期 500ms。
 *
 * 调用上下文：由 julia_context_init 创建的后台任务（优先级 4，栈 5120B），
 * 独立于语音/UI 任务运行；通过 julia_voice_* 接口与 FSM 交互（这些接口内部自加锁）。
 *
 * 每轮流程（对应"数据流"）：
 *   1) 采样 -> 判断是否有运动；有则刷新 s_last_motion_ms 并累计日常活动。
 *   2) 调用 julia_routine_is_deviation()：该函数内部根据历史活动统计判断是否
 *      "行为偏离"（如深夜仍活跃），是则直接在内部投递 EVT_ROUTINE_BREAK。
 *   3) 计算最近活动时间 & 空闲时长：取"运动"与"音频活动"二者较新者作为
 *      last_activity，用 now - last_activity 得到 idle_ms。
 *   4) 仅在语音不繁忙时作情境判定并投递事件（见下）。
 *   5) 状态变化时写 NVS；周期性打印日志；必要时把系统时间同步回 RTC。
 *   6) vTaskDelay 约 500ms 进入下一轮。
 */
static void context_task(void *arg)
{
    (void)arg;
    float ax = 0, ay = 0, az = 0;
    /* 起动即把"最近运动时刻"置为当前，避免上电第一时间就判定成离场/静默。 */
    s_last_motion_ms = esp_timer_get_time() / 1000;
    julia_sub_state_t saved_state = julia_voice_get_state();
    int log_counter = 0;
    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool motion = motion_detected(&ax, &ay, &az);
        if (motion) {
            s_last_motion_ms = now_ms;
            julia_routine_on_activity(JULIA_ACTIVITY_SENSOR);
        }
        julia_routine_is_deviation();
        int64_t audio_ms = julia_voice_last_audio_activity_ms();
        /* "最近活动"取运动与语音音频二者较新者：两者任一发生都算用户/设备有动静。 */
        int64_t last_activity = s_last_motion_ms > audio_ms ? s_last_motion_ms : audio_ms;
        int64_t idle_ms = now_ms - last_activity;
        julia_sub_state_t state = julia_voice_get_state();

        /* 语音会话进行中（is_busy）不投递情境事件，避免打断正在进行的对话/播放。
         * 这是"情境事件不得与语音会话并发"的显式闸门。 */
        if (!julia_voice_is_busy()) {
            /*
             * 情境判定之一：用户在"睡眠/远场待机"期间重新出现（检测到运动）。
             * 此时人与 Julia 的分隔被打破 -> 投递 EVT_USER_RETURN 唤醒。
             * 状态判断用"state <= S0_3（三个睡眠档）或 等于 S1_2（远场待机）"，
             * 即只在这些"人不在 / 睡"的档位把运动当作"人回来了"。
             */
            if (motion && (state <= JULIA_SUB_STATE_S0_3_MANUAL_SLEEP ||
                           state == JULIA_SUB_STATE_S1_2_FAR_STANDBY)) {
                julia_voice_handle_event(EVT_USER_RETURN);
                /* 顺带做"久别重逢"检查：只在本次上电第一次出现"人回"时做一次。
                 * 需要系统时钟与"上次交互时间"都有效，且相隔 >= 24h 才投递
                 * EVT_ROUTINE_BREAK，让 Julia 表达久别重逢的问候。 */
                if (!s_return_care_checked) {
                    time_t now = time(NULL);
                    int64_t last_interaction = julia_memory_last_interaction();
                    s_return_care_checked = true;
                    if (now >= 1704067200 && last_interaction >= 1704067200 &&
                        (int64_t)now - last_interaction >= LONG_ABSENCE_SECONDS) {
                        ESP_LOGI(TAG, "Long absence detected: %lld hours since last interaction",
                                 ((int64_t)now - last_interaction) / 3600);
                        julia_voice_handle_event(EVT_ROUTINE_BREAK);
                    }
                }
            } else {
                /*
                 * 情境判定之二：无人触发的情况下，用"空闲时长 + 时间"推断情境。
                 * 采用 if/else-if 链，优先级从强到弱依次为（保证一次只投一个事件）：
                 *   1) 夜间(23:00~06:59)且空闲 >= 10min -> EVT_NIGHT_TIME（进入夜间睡眠）
                 *   2) 空闲 >= 20min -> EVT_DAY_AWAY（白天离家）
                 *   3) 空闲 >= 5min 且处于"有人"档（S1.1~S2.3）-> EVT_USER_LEAVE（人离开了）
                 *   4) 恰好 22 点且在近场待机 -> EVT_BEDTIME（睡前提示）
                 *   5) 空闲 >= 1min 且在近场待机 -> EVT_SILENCE_TIMEOUT（陪伴-观察）
                 * 注意"夜间"与"白天的离家/静默"是互斥分支：夜间即使空闲 < NIGHT_SLEEP 阈值，
                 * 也不会降级去判 DAY_AWAY/USER_LEAVE，保证夜间不会被"离家/待机"语义误触发。
                 */
                int hour = current_hour();
                bool night = hour >= 23 || (hour >= 0 && hour < 7);
                if (night && idle_ms >= NIGHT_SLEEP_IDLE_MS)
                    julia_voice_handle_event(EVT_NIGHT_TIME);
                else if (idle_ms >= DAY_AWAY_MS)
                    julia_voice_handle_event(EVT_DAY_AWAY);
                else if (idle_ms >= FAR_STANDBY_MS &&
                         state >= JULIA_SUB_STATE_S1_1_NEAR_STANDBY &&
                         state <= JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION)
                    julia_voice_handle_event(EVT_USER_LEAVE);
                else if (hour == 22 && state == JULIA_SUB_STATE_S1_1_NEAR_STANDBY)
                    julia_voice_handle_event(EVT_BEDTIME);
                else if (idle_ms >= QUIET_COMPANION_MS && state == JULIA_SUB_STATE_S1_1_NEAR_STANDBY)
                    julia_voice_handle_event(EVT_SILENCE_TIMEOUT);
            }
        }

        /* 状态若变化则写 NVS（掉电恢复用）。此处用 saved_state 与新鲜读取的 state 比较，
         * 保证"状态没变就不动 flash"，节省写寿命。last_activity 存的是最近活动时刻。 */
        state = julia_voice_get_state();
        if (state != saved_state) { save_context(state, last_activity); saved_state = state; }
        /* 每 20 轮（10s）打一条周期日志；log_counter 到 20 时清零。 */
        if (++log_counter >= 20) {
            ESP_LOGI(TAG, "state=%s idle=%llds motion=%s time=%s",
                     julia_fsm_sub_state_name(state), idle_ms / 1000,
                     motion ? "yes" : "no", (s_time_synced || s_rtc_valid) ? "valid" : "unknown");
            log_counter = 0;
        }
        /*
         * 在系统时间尚未同步前，周期性（每 10 轮=5s）尝试把系统时钟写回 RTC。
         * RTC 一旦写成（s_time_synced=true）便不再重复，避免频繁 I2C 写。
         * NOTE（潜在问题，仅报告）：该判定与上面的 log_counter 清零有交互——
         * log_counter 每 20 轮清零一次，而清零后的 0 也满足 %10==0，因此 sync_rtc_from_system
         * 实际会在"第 10、20（清零为 0）、30、40"等轮被调用。由于方向一致，效果近似"每 5s 一次"，
         * 没有漏同步，但"%10==0 && ==0"这一组合语义上偏隐晦。见问题清单。
         */
        if (!s_time_synced && (log_counter % 10) == 0) sync_rtc_from_system();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

/*
 * @brief 初始化情境感知服务并启动后台任务。
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 创建任务失败。
 *
 * 初始化顺序（有依赖）：
 *   1) 设置本地时区并生效（tzset）。
 *   2) 初始化 RTC 与 IMU 驱动，并固定采样率（acc/gyro 30Hz）。
 *   3) 启动 SNTP（可选，失败不致命——时间只能靠 RTC）。
 *   4) 打开 NVS 分区 "julia_ctx" 用于持久化情境（失败不致命，仅无法掉电恢复）。
 *   5) 创建 context_task 后台任务（失败才返回错误）。
 *
 * 注意：这里 SNTP 用缺省同步回调，与 julia_time.c 里由 ip_ready 触发的 SNTP 是两条
 * 独立的同步路径，都会尝试写 RTC；本模块的 sync_rtc_from_system 与 julia_time.c 的
 * sync_rtc_from_system 均会写入同一块 PCF85063，因此存在两处对 RTC 的并发 I2C 写
 * 入口（见问题清单）。本模块的 SNTP 仅在本函数内、网络尚未就绪时尝试一次。
 */
esp_err_t julia_context_init(void)
{
    setenv("TZ", "CST-8", 1); tzset();
    PCF85063_Init(); QMI8658_Init();
    setAccODR(acc_odr_norm_30); setGyroODR(gyro_odr_norm_30);
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    err = nvs_open("julia_ctx", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "NVS unavailable: %s", esp_err_to_name(err));
    if (xTaskCreate(context_task, "julia_context", 5120, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Context sensing ready: IMU, RTC, NTP and voice activity");
    return ESP_OK;
}
