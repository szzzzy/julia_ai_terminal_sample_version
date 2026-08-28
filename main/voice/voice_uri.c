/**
 * @file    voice_uri.c
 * @brief   FILE_SEND URI 到本地文件系统路径的受控映射实现。
 *
 * 职责：把远端/命令携带的文件 URI（"SD:/x/y"、"SPIFFS:/x/y"）安全地换算为设备
 * 本地受控根目录（/sdcard、/spiffs）内的相对路径。只做纯字符串变换：不打开文件、
 * 不判扩展名、不查文件是否存在，也不限制文件大小——这些由调用方 voice_service
 * 在推流前另行检查（扩展名 .wav、大小 ≤ 8MiB）。
 *
 * 边界：只接受两个固定前缀 + 相对路径段；拒含 "."/".." 段、反斜杠或其余前缀的 URI，
 * 防止远端命令越出音频目录读取文件。这是"受控映射"的核心，详见 voice_uri_segments_safe。
 *
 * 调用方：voice_service_push_file()（WSS 会话任务上下文）。映射本身无阻塞、无锁、
 * 无 GPIO/网络，可安全在任何任务上下文调用，也不必持有 SD 锁。
 *
 * 数据流：URI 字符串 -> 前缀匹配 -> 相对路径段安全性校验 -> "%s/%s" 拼接 -> 返回路径。
 */

#include "voice_uri.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief 检查受控根目录下的相对路径段是否安全。
 *
 * 逐段扫描，拒绝两类输入：
 *   1. 反斜杠 "\\"：只接受 POSIX 分隔符，防止 Windows 风格路径被文件系统在
 *      某些环境下当成目录分隔符，造成与预期不一致的访问目标；
 *   2. "." 或 ".." 段：直接命中当前/父目录，可越出受控根目录。
 *
 * 设计说明（隐藏不变量）：
 *   - 空段（如 "a//b" 里的双斜杠、首尾斜杠）会被 strcspn 加连续跳过分隔符的
 *     循环直接折叠，因此首尾斜杠/重复斜杠不构成拒绝条件——这使 "SD:/x" 与
 *     "SD://x" 都归一化到 "/sdcard/x"，属于"宽松归一化"而非安全漏洞。
 *   - 本函数只校验"段"本身，不校验字符串长度；长度上限由上层缓冲区里的
 *     snprintf 结果判断（voice_uri_to_path 中 n >= path_cap 分支）。
 *
 * @param[in] rel 相对路径（不含 URI 前缀），不允许为 NULL。
 * @return true 路径段安全；false 包含 "."/".." 段或反斜杠。
 */
static bool voice_uri_segments_safe(const char *rel)
{
    const char *p = rel;
    while (*p != '\0') {
        if (*p == '\\') {                   /* 只接受 POSIX 分隔符，防混淆 */
            return false;
        }
        size_t seg_len = strcspn(p, "/");
        /* 恰好一个字符 "." 或恰好 "." + "." 的段：拒绝路径穿越。 */
        if (seg_len == 1 && p[0] == '.') {
            return false;
        }
        if (seg_len == 2 && p[0] == '.' && p[1] == '.') {
            return false;
        }
        p += seg_len;
        while (*p == '/') {
            p++;
        }
    }
    return true;
}

/**
 * @brief 把 FILE_SEND URI 换算为受控根目录内的本地文件路径（见 voice_uri.h）。
 *
 * 数据流：uri -> strncmp 前缀匹配（区分 "SD:/" 与 "SPIFFS:/"）-> 取相对段 rel
 *         -> voice_uri_segments_safe 校验 -> snprintf("%s/%s", root, rel) -> path。
 *
 * @param[in]  uri      URI 字符串，不允许为 NULL；前缀匹配区分大小写（"sd:/" 不匹配）。
 * @param[out] path     输出路径缓冲区，不允许为 NULL。
 * @param[in]  path_cap 输出缓冲区容量，必须大于 0。
 * @return true 换算成功，path 已写入 NUL 结尾路径。
 * @return false 前缀不支持、rel 为空、包含不安全路径段、或拼接结果超出缓冲区。
 *
 * @note 结果路径保留 rel 的结尾斜杠（如 "SD:/x/" -> "/sdcard/x/"），可能指向目录
 *       而非文件；当前调用方按文件 fopen，若遇到目录路径会得到 open 失败。
 * @note 该映射不检查文件是否存在、扩展名或大小；调用方仍需自行 open 并校验。
 *       参数为 NULL / 容量为 0 时直接返回 false，不做部分写入。
 */
bool voice_uri_to_path(const char *uri, char *path, size_t path_cap)
{
    if (uri == NULL || path == NULL || path_cap == 0U) {
        return false;
    }
    const char *root = NULL;
    const char *rel = NULL;
    if (strncmp(uri, "SD:/", 4) == 0) {
        root = "/sdcard";
        rel = uri + 4;
    } else if (strncmp(uri, "SPIFFS:/", 8) == 0) {
        root = "/spiffs";
        rel = uri + 8;
    } else {
        return false;
    }
    if (rel[0] == '\0' || !voice_uri_segments_safe(rel)) {
        return false;
    }
    int n = snprintf(path, path_cap, "%s/%s", root, rel);
    if (n <= 0 || (size_t)n >= path_cap) {
        return false;
    }
    return true;
}
