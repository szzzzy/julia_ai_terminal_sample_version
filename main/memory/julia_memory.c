/**
 * @file    julia_memory.c
 * @brief   未参与当前构建的记忆／画像持久化参考实现。
 *
 * 本文件不在 main/CMakeLists.txt 中，当前固件不会创建其队列、任务或存储目录。
 * 下述流程只描述参考代码契约，不能据此声称产品已保存用户记忆。
 *
 * 数据流（输入 → 校验 → 转换 → 存储/状态更新 → 输出）：
 *  - 输入：对话文本（record_turn）、情绪事件文本（append）、遗忘指令、关键词。
 *  - 校验：敏感词打码（contains_sensitive）、UTF-8 安全截断（utf8_copy）、
 *    类型区间（type<=3）、SD 挂载状态、锁/队列/NVS 可用性。
 *  - 转换：cJSON 序列化/解析、事件字段打包（julia_event_t+crc32）、
 *    会话 JSONL 逐行追加、画像去重（add_unique 上限 12）。
 *  - 存储：SD 卡目录 /sdcard/julia/memory 下的多个文件；NVS（julia_memory）
 *    保存 turn_count 与最后交互时间；事件日志走环形覆盖 + 双头（ping-pong）。
 *  - 状态更新：s_last_interaction、s_turn_count、s_profile、s_summary、事件环形
 *    缓冲与 CRC 校验；主动触发 julia_routine_on_activity(JULIA_ACTIVITY_DIALOG)。
 *  - 输出：构建好的 system prompt、LLM 可用记忆片段、遗忘答复、最近/关键词事件。
 *
 * 所有权与并发：
 *  - s_lock 保护画像/摘要/会话/NVS 相关状态（不含事件）。
 *  - s_event_lock 保护事件环形缓冲 s_events 与 s_event_header。
 *  - s_event_queue 把已校验事件交给后台 event_writer_task 落盘，避免阻塞调用方。
 *  - 直接文件操作（parse/rename/fsync/clean_old_log）在持锁下串行执行；事件文件
 *    由写入任务独占访问（调用方只入队，不直接写该文件）。
 *
 * 持久化介质：SD 卡 FAT（/sdcard），必须在 julia_memory_init 前完成挂载；
 * 过期策略：会话日志按时间(30 天)和大小(24KB)双限，事件日志为环形覆盖。
 *
 * 若未来重新接入，必须先统一 SD mount/lock owner，并重新审查文本留存与删除策略。
 */

#include "julia_memory.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "julia_sd.h"
#include "julia_routine.h"
#include "nvs.h"
#include <unistd.h>

#define TAG "memory"
#define MEMORY_DIR JULIA_SD_MOUNT_POINT "/julia/memory"
#define PROFILE_PATH MEMORY_DIR "/profile.json"                 /* 用户画像（cJSON 对象） */
#define PROFILE_TEMP MEMORY_DIR "/profile.tmp"                  /* 画像写临时后再改名，防崩溃损坏 */
#define CONVERSATION_PATH MEMORY_DIR "/conversation_v2.jsonl"   /* 追加式会话日志，每行一个 JSON */
#define CONVERSATION_TEMP MEMORY_DIR "/conversation.tmp"        /* 会话清理临时文件 */
#define SUMMARY_PATH MEMORY_DIR "/summary.txt"                  /* 最近对话摘要（s_summary 持久化） */
#define MAX_LOG_BYTES (24 * 1024)                               /* 会话日志超过则截断到末尾 24KB */
#define RETENTION_SECONDS (30LL * 24 * 60 * 60)                 /* 会话日志按 30 天过期 */
#define EVENT_PATH MEMORY_DIR "/events_v1.bin"                  /* 事件日志（环形覆盖） */
#define EVENT_CAPACITY 50                                       /* 环形缓冲区最多保留条数 */
#define EVENT_SLOT_COUNT (EVENT_CAPACITY + 1)                   /* 物理槽位=容量+1（多出 1 用于 head 定位） */
#define EVENT_HEADER_COPIES 2                                   /* 双头(ping-pong)，写损坏时回退到上一代 */
#define EVENT_MAGIC 0x4a4d4531U                                 /* 'JME1' 魔数 */
#define EVENT_VERSION 1U                                        /* 事件文件格式版本 */
#define EVENT_QUEUE_DEPTH 16                                    /* 事件入队深度，提供短暂背压 */

void julia_routine_background_flush(void);

/*
 * 事件文件头。与事件记录一样采用“末尾 CRC32”布局，落盘后校验。
 * 双份头（EVENT_HEADER_COPIES=2）写入同一文件的不同偏移，每次写相邻槽位，
 * 崩溃时总能读到至少一份完整合法的头（选择 generation 更大者）。
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t generation;   /* 每次写入递增，用于区分两份头的新旧 */
    uint32_t write_idx;    /* 下一个要写入的槽位（环形） */
    uint32_t count;        /* 有效事件数（≤ EVENT_CAPACITY） */
    uint32_t epoch;        /* 环形写满后覆盖的次数（用于观察重写轮数） */
    uint32_t reserved;
    uint32_t crc32;
} event_header_t;

/*
 * RAM 内缓存的事件（紧凑形式，不含 96 字节记录里的 storage_padding）。
 * 磁盘/API 使用完整的 julia_event_t（96 字节），加载/回读时经 cache_event 与
 * expand_event 在这两种布局间转换，从而省掉内存里 20 字节/条的填充。
 */
typedef struct {
    uint32_t timestamp;
    uint8_t type;
    uint8_t emotion;
    uint8_t reserved[2];
    char summary[64];
    uint32_t crc32;
} cached_event_t;

_Static_assert(sizeof(event_header_t) == 32, "event header must be 32 bytes");

/* 状态（见文件头并发说明）。s_profile/s_summary 持锁后在多个函数间共享，
 * s_NVS 保存 turn_count/last_time，s_last_interaction 供长离隔判断。 */
static SemaphoreHandle_t s_lock;
static cJSON *s_profile;
static char s_summary[2048];
static nvs_handle_t s_nvs;
static uint32_t s_turn_count;
static int64_t s_last_interaction;
static SemaphoreHandle_t s_event_lock;
static QueueHandle_t s_event_queue;
static cached_event_t s_events[EVENT_SLOT_COUNT];
static event_header_t s_event_header;
static uint8_t s_event_header_slot;

/* CRC 覆盖 crc32 字段之前的所有字节（offsetof 恰好排除尾部 crc32）。 */
static uint32_t event_crc(const julia_event_t *event)
{
    return esp_crc32_le(0, (const uint8_t *)event, offsetof(julia_event_t, crc32));
}

static uint32_t header_crc(const event_header_t *header)
{
    return esp_crc32_le(0, (const uint8_t *)header, offsetof(event_header_t, crc32));
}

/* 头合法判定：魔数/版本/大小/计数约束 + 末尾 CRC 自洽。 */
static bool valid_header(const event_header_t *header)
{
    return header->magic == EVENT_MAGIC && header->version == EVENT_VERSION &&
           header->size == sizeof(*header) && header->write_idx < EVENT_SLOT_COUNT &&
           header->count <= EVENT_CAPACITY && header->crc32 == header_crc(header);
}

/* 事件合法判定：CRC 校验且非全零（CRC==0 视为无效待写入的空槽）。 */
static bool valid_event(const julia_event_t *event)
{
    return event->crc32 != 0 && event->crc32 == event_crc(event);
}

/* 96 字节 julia_event_t → 76 字节 cached_event_t（略去 storage_padding）。 */
static void cache_event(cached_event_t *cached, const julia_event_t *event)
{
    cached->timestamp = event->timestamp;
    cached->type = event->type;
    cached->emotion = event->emotion;
    memcpy(cached->reserved, event->reserved, sizeof(cached->reserved));
    memcpy(cached->summary, event->summary, sizeof(cached->summary));
    cached->crc32 = event->crc32;
}

/* compact cached_event_t → 96 字节 julia_event_t，并做 CRC 校验（供对外返回）。 */
static bool expand_event(const cached_event_t *cached, julia_event_t *event)
{
    memset(event, 0, sizeof(*event));
    event->timestamp = cached->timestamp;
    event->type = cached->type;
    event->emotion = cached->emotion;
    memcpy(event->reserved, cached->reserved, sizeof(event->reserved));
    memcpy(event->summary, cached->summary, sizeof(event->summary));
    event->crc32 = cached->crc32;
    return valid_event(event);
}

/* 生成一个全新的、后续未覆盖过的空头（用于首次创建/全部清除后）。 */
static void init_event_header(void)
{
    memset(&s_event_header, 0, sizeof(s_event_header));
    s_event_header.magic = EVENT_MAGIC;
    s_event_header.version = EVENT_VERSION;
    s_event_header.size = sizeof(s_event_header);
    s_event_header.crc32 = header_crc(&s_event_header);
}

/* 把 stdio 缓冲刷到 OS 并强制落盘（fsync）。返回 ESP_OK/ESP_FAIL。 */
static esp_err_t sync_file(FILE *file)
{
    if (fflush(file) != 0) return ESP_FAIL;
    return fsync(fileno(file)) == 0 ? ESP_OK : ESP_FAIL;
}

/*
 * 持久化一条事件到磁盘：先写事件本体，再写“下一版”头到相邻槽位，成功后更新
 * 内存缓存与头。两步都 fsync，避免崩溃时读到半写状态。
 *  - 事件槽位 = 双头之后按 write_idx 顺序排列（事件部分本身不带头）。
 *  - 头槽位 = s_event_header_slot ^ 1（ping-pong 轮流写，保留上一代作回退）。
 * 并发：仅在后台写入任务中调用，因此对文件与 s_event_header 的访问不会与其它
 * 任务竞争；中间的 s_event_lock 只保证读端（get_recent/recall）能看到成对一致的
 * 缓存与头。
 */
static esp_err_t persist_event(const julia_event_t *event)
{
    FILE *file = fopen(EVENT_PATH, "r+b");
    if (!file) file = fopen(EVENT_PATH, "w+b");
    if (!file) return ESP_FAIL;

    uint32_t index = s_event_header.write_idx;
    long offset = (long)(EVENT_HEADER_COPIES * sizeof(event_header_t) +
                         index * sizeof(julia_event_t));
    esp_err_t err = ESP_FAIL;
    if (fseek(file, offset, SEEK_SET) == 0 &&
        fwrite(event, 1, sizeof(*event), file) == sizeof(*event) &&
        sync_file(file) == ESP_OK) {
        event_header_t next = s_event_header;
        next.generation++;
        next.write_idx = (index + 1U) % EVENT_SLOT_COUNT;      /* 环形推进 */
        if (next.count < EVENT_CAPACITY) next.count++;          /* 未满则增加计数 */
        else next.epoch++;                                      /* 已满则只记覆盖轮数 */
        next.crc32 = header_crc(&next);
        uint8_t slot = s_event_header_slot ^ 1U;                /* 写到另一份头 */
        if (fseek(file, (long)(slot * sizeof(event_header_t)), SEEK_SET) == 0 &&
            fwrite(&next, 1, sizeof(next), file) == sizeof(next) &&
            sync_file(file) == ESP_OK) {
            xSemaphoreTake(s_event_lock, portMAX_DELAY);
            cache_event(&s_events[index], event);
            s_event_header = next;
            s_event_header_slot = slot;
            xSemaphoreGive(s_event_lock);
            err = ESP_OK;
        }
    }
    fclose(file);
    return err;
}

/* 后台写盘任务：排队收到事件即 persist（fsync 可能较慢，借助队列做背压），
 * 每次循环顺带触发例行检测的延迟落盘。取到事件即持久化，无其它任务写事件文件。 */
static void event_writer_task(void *arg)
{
    (void)arg;
    julia_event_t event;
    while (true) {
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            esp_err_t err = persist_event(&event);
            if (err != ESP_OK) ESP_LOGE("memory", "event persist failed: %s", esp_err_to_name(err));
        }
        julia_routine_background_flush();
    }
}

/*
 * 加载事件文件：读取并校验两份头，选择合法且 generation 更大的一版；随后按槽位
 * 读回每条事件（CRC 校验失败视为空槽），全部装入内存缓存。最后创建事件队列与
 * 写入任务（优先把任务栈放在 PSRAM，不可用则退回内部 RAM）。
 * 若文件不存在，则以一个全新的空头起步（s_events 全零）。
 */
static esp_err_t load_events(void)
{
    init_event_header();
    FILE *file = fopen(EVENT_PATH, "rb");
    if (file) {
        event_header_t headers[EVENT_HEADER_COPIES] = {0};
        fread(headers, sizeof(event_header_t), EVENT_HEADER_COPIES, file);
        bool valid0 = valid_header(&headers[0]);
        bool valid1 = valid_header(&headers[1]);
        if (valid0 || valid1) {
            /* 任一头合法即可；两者都合法时取 generation 更大者作为当前头。 */
            s_event_header_slot = valid1 && (!valid0 || headers[1].generation > headers[0].generation);
            s_event_header = headers[s_event_header_slot];
        }
        for (uint32_t i = 0; i < EVENT_SLOT_COUNT; ++i) {
            julia_event_t event;
            long offset = (long)(EVENT_HEADER_COPIES * sizeof(event_header_t) +
                                 i * sizeof(julia_event_t));
            if (fseek(file, offset, SEEK_SET) != 0 ||
                fread(&event, 1, sizeof(event), file) != sizeof(event) ||
                !valid_event(&event)) memset(&s_events[i], 0, sizeof(s_events[i]));
            else cache_event(&s_events[i], &event);
        }
        fclose(file);
    }
    s_event_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(julia_event_t));
    if (!s_event_queue) return ESP_ERR_NO_MEM;
    BaseType_t writer = xTaskCreateWithCaps(event_writer_task, "memory_writer", 8192,
                                            NULL, 2, NULL,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (writer != pdPASS) {
        ESP_LOGW(TAG, "PSRAM memory_writer stack unavailable; trying internal RAM");
        writer = xTaskCreate(event_writer_task, "memory_writer", 8192, NULL, 2, NULL);
    }
    if (writer != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI("memory", "event ring ready: count=%lu epoch=%lu", (unsigned long)s_event_header.count,
             (unsigned long)s_event_header.epoch);
    return ESP_OK;
}

/* 敏感词判定：命中即整体打码/丢弃，绝不持久化原文。列表为中文金融/身份类词与
 * 常见英文密钥字段，属简化启发式，不是严格 PII 检测。 */
static bool contains_sensitive(const char *text)
{
    static const char *words[] = {"密码", "身份证", "银行卡", "信用卡", "验证码", "access key", "api key", "secret"};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        if (strstr(text, words[i])) return true;
    return false;
}

/* 按 size-1 上限复制，但会把末尾回退到完整 UTF-8 字符边界：从尾部丢弃所有
 * 10xxxxxx 续字节，避免把一个多字节字符拦腰截断（防乱码）。 */
static void utf8_copy(char *dest, size_t size, const char *source, size_t length)
{
    if (!size) return;
    if (length >= size) length = size - 1;
    while (length && ((unsigned char)source[length] & 0xC0) == 0x80) --length;
    memcpy(dest, source, length); dest[length] = 0;
}

/* 从文本里抓取 marker 之后直到第一个句末标点/空格的字段值，用于“我叫/叫我/我的生日是/
 * 我喜欢/记住”等画像抽取。找不到 marker 则置空并返回。 */
static void extract_after(const char *text, const char *marker, char *value, size_t value_size)
{
    const char *start = strstr(text, marker);
    if (!start) { value[0] = 0; return; }
    start += strlen(marker);
    while (*start == ' ' || *start == ':') ++start;
    if (strncmp(start, "：", strlen("：")) == 0) start += strlen("：");
    const char *end = start + strlen(start);
    static const char *stops[] = {"。", "，", "！", "？", ",", ".", "!", "?", "\n"};
    for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); ++i) {
        const char *stop = strstr(start, stops[i]);
        if (stop && stop < end) end = stop;
    }
    while (end > start && end[-1] == ' ') --end;
    utf8_copy(value, value_size, start, end - start);
}

/* 新建一份默认画像（version=1，各字段为空/空数组）。 */
static cJSON *new_profile(void)
{
    cJSON *profile = cJSON_CreateObject();
    cJSON_AddNumberToObject(profile, "version", 1);
    cJSON_AddStringToObject(profile, "name", "");
    cJSON_AddStringToObject(profile, "preferred_name", "");
    cJSON_AddStringToObject(profile, "birthday", "");
    cJSON_AddArrayToObject(profile, "preferences");
    cJSON_AddArrayToObject(profile, "notes");
    return profile;
}

/* 先写临时文件再 rename 覆盖正式文件，避免半写损坏画像。 */
static esp_err_t save_profile(void)
{
    char *json = cJSON_Print(s_profile);
    if (!json) return ESP_ERR_NO_MEM;
    FILE *file = fopen(PROFILE_TEMP, "wb");
    if (!file) { cJSON_free(json); return ESP_FAIL; }
    size_t length = strlen(json);
    bool ok = fwrite(json, 1, length, file) == length;
    fclose(file); cJSON_free(json);
    if (!ok) return ESP_FAIL;
    remove(PROFILE_PATH);
    return rename(PROFILE_TEMP, PROFILE_PATH) == 0 ? ESP_OK : ESP_FAIL;
}

/* 从 SD 卡读取画像；文件缺失/损坏/超大(>32KB)都回退到新画像并落盘一次。 */
static void load_profile(void)
{
    FILE *file = fopen(PROFILE_PATH, "rb");
    if (!file) { s_profile = new_profile(); save_profile(); return; }
    fseek(file, 0, SEEK_END); long length = ftell(file); rewind(file);
    char *json = length > 0 && length < 32768 ? malloc(length + 1) : NULL;
    if (json && fread(json, 1, length, file) == (size_t)length) {
        json[length] = 0; s_profile = cJSON_Parse(json);
    }
    free(json); fclose(file);
    if (!s_profile) { s_profile = new_profile(); save_profile(); }
}

/* 一次性载入最近对话摘要（s_summary 是有界缓冲，见 append_summary）。 */
static void load_summary(void)
{
    FILE *file = fopen(SUMMARY_PATH, "rb");
    if (!file) return;
    size_t length = fread(s_summary, 1, sizeof(s_summary) - 1, file);
    s_summary[length] = 0; fclose(file);
}

/* 向数组追加去重后的字符串值；数组超过 12 条时丢弃最旧（从头部删除）。 */
static void add_unique(cJSON *array, const char *value)
{
    if (!array || !value[0]) return;
    cJSON *item;
    cJSON_ArrayForEach(item, array)
        if (cJSON_IsString(item) && strcmp(item->valuestring, value) == 0) return;
    while (cJSON_GetArraySize(array) >= 12) cJSON_DeleteItemFromArray(array, 0);
    cJSON_AddItemToArray(array, cJSON_CreateString(value));
}

/* 更新/新增画像字符串字段。 */
static void set_string(const char *name, const char *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(s_profile, name);
    if (item) cJSON_SetValuestring(item, value);
    else cJSON_AddStringToObject(s_profile, name, value);
}

/* 从一轮对话中抽取画像信息：敏感文本直接跳过；按“我叫/叫我/我的生日是/我喜欢/记住”
 * 提取到对应字段。只做规则匹配，不依赖 LLM。 */
static void update_profile(const char *text)
{
    if (contains_sensitive(text)) return;
    char value[160];
    extract_after(text, "我叫", value, sizeof(value));
    if (value[0]) set_string("name", value);
    extract_after(text, "叫我", value, sizeof(value));
    if (value[0]) set_string("preferred_name", value);
    extract_after(text, "我的生日是", value, sizeof(value));
    if (value[0]) set_string("birthday", value);
    extract_after(text, "我喜欢", value, sizeof(value));
    if (value[0]) add_unique(cJSON_GetObjectItemCaseSensitive(s_profile, "preferences"), value);
    extract_after(text, "记住", value, sizeof(value));
    if (value[0]) add_unique(cJSON_GetObjectItemCaseSensitive(s_profile, "notes"), value);
}

/* 追加一行“用户：…\nJulia：…\n”到最近对话摘要。超出缓冲时保留后半段
 * （从 UTF-8 边界切分），保证提示里最新对话总在。随后写回 summary.txt。 */
static void append_summary(const char *user_text, const char *assistant_text)
{
    char user[128], assistant[128], line[300];
    utf8_copy(user, sizeof(user), user_text, strlen(user_text));
    utf8_copy(assistant, sizeof(assistant), assistant_text, strlen(assistant_text));
    snprintf(line, sizeof(line), "用户：%s\nJulia：%s\n", user, assistant);
    size_t line_length = strlen(line), used = strlen(s_summary);
    if (used + line_length >= sizeof(s_summary)) {
        size_t keep = sizeof(s_summary) / 2;
        const char *start = s_summary + used - keep;
        while (*start && ((unsigned char)*start & 0xC0) == 0x80) ++start;
        memmove(s_summary, start, strlen(start) + 1); used = strlen(s_summary);
    }
    strlcat(s_summary, line, sizeof(s_summary));
    FILE *file = fopen(SUMMARY_PATH, "wb");
    if (file) { fwrite(s_summary, 1, strlen(s_summary), file); fclose(file); }
}

/* 会话日志维护：
 * 1) 逐行解析 JSONL，删除 timestamp 早于 RETENTION_SECONDS(30 天)的条目；
 * 2) 若结果仍超过 MAX_LOG_BYTES(24KB)，用 in-place shift 把文件截断到末尾 24KB
 *    （并回退到换行边界，保证不出现半行的 JSON）；
 * 3) 生成新文件后替换正式文件（CONVERSATION_TEMP → CONVERSATION_PATH）。
 * 注意：条目内的 timestamp 是记录时的 wall-clock 秒；若时钟未同步（<2024），
 * 该条不会被保留（时间戳为 0 或 uptime 值）。 */
static void clean_old_log(void)
{
    FILE *input = fopen(CONVERSATION_PATH, "rb");
    if (!input) return;
    FILE *output = fopen(CONVERSATION_TEMP, "wb");
    if (!output) { fclose(input); return; }
    char *line = malloc(4096);
    if (!line) { fclose(input); fclose(output); remove(CONVERSATION_TEMP); return; }
    int64_t cutoff = (int64_t)time(NULL) - RETENTION_SECONDS;
    while (fgets(line, 4096, input)) {
        cJSON *entry = cJSON_Parse(line);
        cJSON *timestamp = entry ? cJSON_GetObjectItemCaseSensitive(entry, "timestamp") : NULL;
        if (cJSON_IsNumber(timestamp) && (int64_t)timestamp->valuedouble >= cutoff)
            fputs(line, output);
        cJSON_Delete(entry);
    }
    free(line); fclose(input);
    long output_size = ftell(output);
    fclose(output);
    if (output_size > MAX_LOG_BYTES) {
        output = fopen(CONVERSATION_TEMP, "r+b");
        if (output) {
            /* 定位到“末尾 MAX_LOG_BYTES”所在行的起始处，用读写游标把尾部前移。 */
            long read_pos = output_size - MAX_LOG_BYTES;
            fseek(output, read_pos, SEEK_SET);
            int ch;
            while ((ch = fgetc(output)) != '\n' && ch != EOF) {}
            read_pos = ftell(output);
            long write_pos = 0;
            char block[512];
            size_t got;
            while ((got = fread(block, 1, sizeof(block), output)) > 0) {
                fseek(output, write_pos, SEEK_SET);
                fwrite(block, 1, got, output);
                write_pos += (long)got;
                read_pos += (long)got;
                fseek(output, read_pos, SEEK_SET);
            }
            fflush(output);
            ftruncate(fileno(output), write_pos);
            fclose(output);
        }
    }
    remove(CONVERSATION_PATH); rename(CONVERSATION_TEMP, CONVERSATION_PATH);
}

/* 初始化记忆系统。
 * 前置：SD 已挂载（否则返回 ESP_ERR_INVALID_STATE）。
 * 副作用：mkdir 目录、加载/创建画像与摘要、清理过期会话日志、打开 NVS
 *         （julia_memory 命名空间）、读取 turn_count/last_time，
 *          并由 load_events() 启动事件写入后台任务。
 * 调用上下文：应在系统启动早期、任何记忆读写之前调用一次。
 * NOTE：本工程暂未发现对该函数的调用点（见报告）。 */
esp_err_t julia_memory_init(void)
{
    if (!julia_sd_is_mounted()) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Initializing memory directories");
    mkdir(JULIA_SD_MOUNT_POINT "/julia", 0775); mkdir(MEMORY_DIR, 0775);
    s_lock = xSemaphoreCreateMutex();
    s_event_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_event_lock) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Loading profile");
    load_profile();
    ESP_LOGI(TAG, "Loading summary");
    load_summary();
    ESP_LOGI(TAG, "Checking conversation retention");
    clean_old_log();
    ESP_LOGI(TAG, "Opening memory NVS");
    esp_err_t err = nvs_open("julia_memory", NVS_READWRITE, &s_nvs);
    if (err == ESP_OK) {
        nvs_get_u32(s_nvs, "turn_count", &s_turn_count);
        nvs_get_i64(s_nvs, "last_time", &s_last_interaction);
    }
    ESP_LOGI(TAG, "Memory ready: %u turns, profile=%s", s_turn_count, PROFILE_PATH);
    return load_events();
}

/* 组装 system prompt：base_prompt ＋ 画像(JSON) ＋ 最近对话摘要 ＋ 防泄露提示。
 * 输出须 >= output_size；画像/摘要缺任一时都按空串拼接，不会失败。
 * 调用上下文：语音对话任务发送 AI 请求前调用一次。 */
esp_err_t julia_memory_build_prompt(const char *base_prompt, char *output, size_t output_size)
{
    if (!base_prompt || !output || !output_size || !s_lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *profile = cJSON_PrintUnformatted(s_profile);
    snprintf(output, output_size,
             "%s\n以下是持久化用户画像(JSON)：%s\n最近对话摘要：\n%s\n"
             "只在相关时自然使用这些记忆，不要主动泄露敏感数据。",
             base_prompt, profile ? profile : "{}", s_summary);
    cJSON_free(profile); xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* 记录一轮对话（情绪取 NONE）——见 with_emotion 版本。 */
esp_err_t julia_memory_record_turn(const char *user_text, const char *assistant_text)
{
    return julia_memory_record_turn_with_emotion(user_text, assistant_text, JULIA_MEMORY_EMOTION_NONE);
}

/* 记录一轮对话（带情绪）。
 * 输入：用户/助手文本，emotion（julia_emotion_t）。
 * 副作用：更新画像并保存、追加摘要、追加会话 JSONL（敏感词打码）、
 *          更新 s_last_interaction 与 s_turn_count 并提交到 NVS、每 10 轮清理一次
 *          会话日志、写一条 type=0 事件、通知例行检测 DIALOG 活动。
 * 返回：evt_err（事件入队结果），但即使事件失败，对话记录本身仍已落盘。
 * 调用上下文：语音对话任务完成一轮后调用。 */
esp_err_t julia_memory_record_turn_with_emotion(const char *user_text,
                                                const char *assistant_text,
                                                uint8_t emotion)
{
    if (!user_text || !assistant_text || !s_lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    update_profile(user_text); save_profile(); append_summary(user_text, assistant_text);
    cJSON *entry = cJSON_CreateObject(); s_last_interaction = time(NULL);
    cJSON_AddNumberToObject(entry, "timestamp", (double)s_last_interaction);
    cJSON_AddStringToObject(entry, "user", contains_sensitive(user_text) ? "[sensitive omitted]" : user_text);
    cJSON_AddStringToObject(entry, "assistant", contains_sensitive(user_text) ? "[sensitive omitted]" : assistant_text);
    char *line = cJSON_PrintUnformatted(entry); cJSON_Delete(entry);
    FILE *file = fopen(CONVERSATION_PATH, "ab");
    if (file && line) { fputs(line, file); fputc('\n', file); fclose(file); }
    cJSON_free(line); ++s_turn_count;
    if (s_nvs) {
        nvs_set_u32(s_nvs, "turn_count", s_turn_count);
        nvs_set_i64(s_nvs, "last_time", s_last_interaction); nvs_commit(s_nvs);
    }
    if ((s_turn_count % 10) == 0) clean_old_log();
    xSemaphoreGive(s_lock);
    esp_err_t event_err = julia_memory_append(0, emotion, user_text);
    julia_routine_on_activity(JULIA_ACTIVITY_DIALOG);
    return event_err;
}

/* 处理“遗忘类”请求，命中则返回 true（调用方跳过 AI 并直接使用 reply 答复）。
 * 匹配规则（按优先级）：
 *  - 含“所有/全部/清空记忆”→ 全清（重置画像、清摘要、删会话与摘要文件）；
 *  - 含“名字/称呼”→ 清 name+preferred_name；
 *  - 含“生日”→ 清 birthday；
 *  - 含“喜好/喜欢”→ 清空 preferences 数组；
 *  - 其它含“忘记/清空记忆”→ 删除最近一条 notes；
 *  - 不含任何遗忘关键词 → 返回 false。
 * 注意“清空记忆”同时出现在第 1 分支与总入口判定里。 */
bool julia_memory_handle_forget(const char *text, char *reply, size_t reply_size)
{
    if (!text || (!strstr(text, "忘记") && !strstr(text, "清空记忆"))) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strstr(text, "所有") || strstr(text, "全部") || strstr(text, "清空记忆")) {
        cJSON_Delete(s_profile); s_profile = new_profile(); s_summary[0] = 0;
        remove(CONVERSATION_PATH); remove(SUMMARY_PATH);
        strlcpy(reply, "好的，我已经清空了长期记忆。", reply_size);
    } else if (strstr(text, "名字") || strstr(text, "称呼")) {
        set_string("name", ""); set_string("preferred_name", "");
        strlcpy(reply, "好的，我不会再记住你的名字和称呼。", reply_size);
    } else if (strstr(text, "生日")) {
        set_string("birthday", ""); strlcpy(reply, "好的，我已经忘记生日信息。", reply_size);
    } else if (strstr(text, "喜好") || strstr(text, "喜欢")) {
        cJSON_ReplaceItemInObject(s_profile, "preferences", cJSON_CreateArray());
        strlcpy(reply, "好的，我已经清除保存的喜好。", reply_size);
    } else {
        cJSON *notes = cJSON_GetObjectItemCaseSensitive(s_profile, "notes");
        int count = cJSON_GetArraySize(notes); if (count) cJSON_DeleteItemFromArray(notes, count - 1);
        strlcpy(reply, "好的，我已经忘记最近记住的那件事。", reply_size);
    }
    save_profile(); xSemaphoreGive(s_lock); return true;
}

/* 返回最后交互的 wall-clock 秒；未初始化时为 0。供长离隔判断使用。 */
int64_t julia_memory_last_interaction(void) { return s_last_interaction; }

/* 入队一条事件记录（异步，由后台任务持久化）。
 * 校验：summary 非空、type<=3（0=对话/1=情绪/3=界面；2 保留）、队列已创建。
 * 时间戳：优先 wall-clock 秒（time(NULL)）；若时钟 <2024（未同步）则退化为
 *          uptime 秒（esp_timer_get_time()/1e6），避免 0xFFFFFFFF 附近跳变，且
 *          API 层保证非负。type==0 的 summary 额外截到 61 字节（原因待确认）。
 * 失败：队列满时阻塞最多 500ms，超时返回 ESP_ERR_TIMEOUT（背压策略）。
 * 副作用：仅入队，不直接写文件；事件是否落盘由 event_writer_task 决定。 */
esp_err_t julia_memory_append(uint8_t type, uint8_t emotion, const char *summary)
{
    if (!summary || type > 3 || !s_event_queue) return ESP_ERR_INVALID_ARG;
    julia_event_t event = {0};
    time_t now = time(NULL);
    event.timestamp = now >= 1704067200 ? (uint32_t)now
                                        : (uint32_t)(esp_timer_get_time() / 1000000ULL);
    event.type = type;
    event.emotion = emotion;
    size_t summary_size = type == 0 ? 61U : sizeof(event.summary);
    utf8_copy(event.summary, summary_size, summary, strlen(summary));
    event.crc32 = event_crc(&event);
    /* 写入任务执行 fsync 可能较慢；短暂背压比静默丢失记忆更可靠。 */
    return xQueueSend(s_event_queue, &event, pdMS_TO_TICKS(500)) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* 取最近 n 条有效事件，新的在前。n 先被 max 截断，再被实际条数截断；
 * 从当前 write_idx 的前一个槽位向前回读，CRC 校验不过（空槽/损坏）则跳过。
 * 返回实际写出的条数。取到一致快照（锁保护缓存/头成对更新）。 */
int julia_memory_get_recent(int n, julia_event_t *out, int max)
{
    if (n <= 0 || !out || max <= 0 || !s_event_lock) return 0;
    if (n > max) n = max;
    xSemaphoreTake(s_event_lock, portMAX_DELAY);
    int available = (int)s_event_header.count;
    if (n > available) n = available;
    int written = 0;
    for (int age = 0; age < available && written < n; ++age) {
        int index = ((int)s_event_header.write_idx - 1 - age + EVENT_SLOT_COUNT) % EVENT_SLOT_COUNT;
        julia_event_t event;
        if (expand_event(&s_events[index], &event)) out[written++] = event;
    }
    xSemaphoreGive(s_event_lock);
    return written;
}

/* 关键词回忆：与 get_recent 相同的从新到旧回读，但只保留 summary 中含 kw 子串的
 * 事件（strstr 大小写敏感、按子串匹配，不做分词）。返回命中数，最多 max 条。 */
int julia_memory_recall_keyword(const char *kw, julia_event_t *out, int max)
{
    if (!kw || !kw[0] || !out || max <= 0 || !s_event_lock) return 0;
    xSemaphoreTake(s_event_lock, portMAX_DELAY);
    int written = 0;
    int available = (int)s_event_header.count;
    for (int age = 0; age < available && written < max; ++age) {
        int index = ((int)s_event_header.write_idx - 1 - age + EVENT_SLOT_COUNT) % EVENT_SLOT_COUNT;
        julia_event_t event;
        if (expand_event(&s_events[index], &event) && strstr(event.summary, kw))
            out[written++] = event;
    }
    xSemaphoreGive(s_event_lock);
    return written;
}

/* 把最近最多 4 条事件的 summary 拼成一行（"; " 分隔），供拼进 prompt。
 * 内部把输出上限钳到 200 字节，一次调用最多写入 buf_size 字节。返回字符串长度。
 * NOTE：本工程尚未看到调用点，可能为预留接口。 */
int julia_memory_format_for_prompt(char *buf, int buf_size)
{
    if (!buf || buf_size <= 0) return 0;
    int limit = buf_size < 201 ? buf_size : 201;
    buf[0] = '\0';
    julia_event_t recent[4];
    int count = julia_memory_get_recent(4, recent, 4);
    for (int i = 0; i < count; ++i) {
        char line[80];
        snprintf(line, sizeof(line), "%s%s", i ? "; " : "", recent[i].summary);
        size_t used = strlen(buf);
        if (used >= (size_t)limit - 1) break;
        utf8_copy(buf + used, (size_t)limit - used, line, strlen(line));
    }
    return (int)strlen(buf);
}

/* 清空全部事件：丢弃在途队列、清内存缓存、复位头、删除事件文件。
 * 仅针对事件日志，不影响画像/摘要/会话日志。 */
esp_err_t julia_memory_forget_all(void)
{
    if (!s_event_lock || !s_event_queue) return ESP_ERR_INVALID_STATE;
    xQueueReset(s_event_queue);
    xSemaphoreTake(s_event_lock, portMAX_DELAY);
    memset(s_events, 0, sizeof(s_events));
    init_event_header();
    s_event_header_slot = 0;
    xSemaphoreGive(s_event_lock);
    return remove(EVENT_PATH) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL;
}
