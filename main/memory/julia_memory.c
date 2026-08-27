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
#define PROFILE_PATH MEMORY_DIR "/profile.json"
#define PROFILE_TEMP MEMORY_DIR "/profile.tmp"
#define CONVERSATION_PATH MEMORY_DIR "/conversation_v2.jsonl"
#define CONVERSATION_TEMP MEMORY_DIR "/conversation.tmp"
#define SUMMARY_PATH MEMORY_DIR "/summary.txt"
#define MAX_LOG_BYTES (24 * 1024)
#define RETENTION_SECONDS (30LL * 24 * 60 * 60)
#define EVENT_PATH MEMORY_DIR "/events_v1.bin"
#define EVENT_CAPACITY 50
#define EVENT_SLOT_COUNT (EVENT_CAPACITY + 1)
#define EVENT_HEADER_COPIES 2
#define EVENT_MAGIC 0x4a4d4531U
#define EVENT_VERSION 1U
#define EVENT_QUEUE_DEPTH 16

void julia_routine_background_flush(void);

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t generation;
    uint32_t write_idx;
    uint32_t count;
    uint32_t epoch;
    uint32_t reserved;
    uint32_t crc32;
} event_header_t;

typedef struct {
    uint32_t timestamp;
    uint8_t type;
    uint8_t emotion;
    uint8_t reserved[2];
    char summary[64];
    uint32_t crc32;
} cached_event_t;

_Static_assert(sizeof(event_header_t) == 32, "event header must be 32 bytes");

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

static uint32_t event_crc(const julia_event_t *event)
{
    return esp_crc32_le(0, (const uint8_t *)event, offsetof(julia_event_t, crc32));
}

static uint32_t header_crc(const event_header_t *header)
{
    return esp_crc32_le(0, (const uint8_t *)header, offsetof(event_header_t, crc32));
}

static bool valid_header(const event_header_t *header)
{
    return header->magic == EVENT_MAGIC && header->version == EVENT_VERSION &&
           header->size == sizeof(*header) && header->write_idx < EVENT_SLOT_COUNT &&
           header->count <= EVENT_CAPACITY && header->crc32 == header_crc(header);
}

static bool valid_event(const julia_event_t *event)
{
    return event->crc32 != 0 && event->crc32 == event_crc(event);
}

static void cache_event(cached_event_t *cached, const julia_event_t *event)
{
    cached->timestamp = event->timestamp;
    cached->type = event->type;
    cached->emotion = event->emotion;
    memcpy(cached->reserved, event->reserved, sizeof(cached->reserved));
    memcpy(cached->summary, event->summary, sizeof(cached->summary));
    cached->crc32 = event->crc32;
}

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

static void init_event_header(void)
{
    memset(&s_event_header, 0, sizeof(s_event_header));
    s_event_header.magic = EVENT_MAGIC;
    s_event_header.version = EVENT_VERSION;
    s_event_header.size = sizeof(s_event_header);
    s_event_header.crc32 = header_crc(&s_event_header);
}

static esp_err_t sync_file(FILE *file)
{
    if (fflush(file) != 0) return ESP_FAIL;
    return fsync(fileno(file)) == 0 ? ESP_OK : ESP_FAIL;
}

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
        next.write_idx = (index + 1U) % EVENT_SLOT_COUNT;
        if (next.count < EVENT_CAPACITY) next.count++;
        else next.epoch++;
        next.crc32 = header_crc(&next);
        uint8_t slot = s_event_header_slot ^ 1U;
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

static bool contains_sensitive(const char *text)
{
    static const char *words[] = {"密码", "身份证", "银行卡", "信用卡", "验证码", "access key", "api key", "secret"};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        if (strstr(text, words[i])) return true;
    return false;
}

static void utf8_copy(char *dest, size_t size, const char *source, size_t length)
{
    if (!size) return;
    if (length >= size) length = size - 1;
    while (length && ((unsigned char)source[length] & 0xC0) == 0x80) --length;
    memcpy(dest, source, length); dest[length] = 0;
}

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

static void load_summary(void)
{
    FILE *file = fopen(SUMMARY_PATH, "rb");
    if (!file) return;
    size_t length = fread(s_summary, 1, sizeof(s_summary) - 1, file);
    s_summary[length] = 0; fclose(file);
}

static void add_unique(cJSON *array, const char *value)
{
    if (!array || !value[0]) return;
    cJSON *item;
    cJSON_ArrayForEach(item, array)
        if (cJSON_IsString(item) && strcmp(item->valuestring, value) == 0) return;
    while (cJSON_GetArraySize(array) >= 12) cJSON_DeleteItemFromArray(array, 0);
    cJSON_AddItemToArray(array, cJSON_CreateString(value));
}

static void set_string(const char *name, const char *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(s_profile, name);
    if (item) cJSON_SetValuestring(item, value);
    else cJSON_AddStringToObject(s_profile, name, value);
}

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

esp_err_t julia_memory_record_turn(const char *user_text, const char *assistant_text)
{
    return julia_memory_record_turn_with_emotion(user_text, assistant_text, JULIA_MEMORY_EMOTION_NONE);
}

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

int64_t julia_memory_last_interaction(void) { return s_last_interaction; }

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
