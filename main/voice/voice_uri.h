/**
 * @file    voice_uri.h
 * @brief   FILE_SEND URI 到本地文件系统路径的受控映射。
 *
 * 只允许受控根目录内的相对路径："SD:/x/y" -> "/sdcard/x/y"，
 * "SPIFFS:/x/y" -> "/spiffs/x/y"。包含 "."/".." 段、反斜杠或绝对路径的
 * URI 一律拒绝，防止远端命令越出预期音频目录读取文件。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把 FILE_SEND URI 换算为受控根目录内的本地文件系统路径。
 *
 * @param[in]  uri      URI 字符串（"SD:/x/y" 或 "SPIFFS:/x/y"），不允许为 NULL。
 * @param[out] path     输出路径缓冲区，不允许为 NULL。
 * @param[in]  path_cap 输出缓冲区容量，必须大于 0。
 *
 * @return true 换算成功，path 已写入 NUL 结尾路径。
 * @return false URI 格式不支持、包含不安全路径段或缓冲区不足。
 *
 * @note 该映射不检查文件是否存在；调用方仍需自行 open。
 * @note 前缀匹配区分大小写；仅做纯字符串变换，无文件 I/O、无阻塞。
 *       path 容量须 ≥ 路径长度 + 1，本项目调用方使用 `VOICE_SERVICE_URI_MAX_LEN + 16`。
 * @note 不做百分号、URL 或 UTF-8 解码：检查的是调用方传入的原始字节，调用方必须传入
 *       未解码的原始 URI。若上游先做过解码，`%2e%2e` 会变成 `..` 而被本模块拒绝，
 *       但也可能把原本受控的路径改写成其它形式，因此不应在上游预先解码。
 * @note 失败时 path 的内容不可用——snprintf 截断前可能已经写入了部分路径，
 *       调用方只能依据返回值判断，不能读取或使用 path。
 */
bool voice_uri_to_path(const char *uri, char *path, size_t path_cap);

#ifdef __cplusplus
}
#endif
