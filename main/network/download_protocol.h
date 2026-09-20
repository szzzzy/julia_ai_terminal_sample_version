/**
 * @file    download_protocol.h
 * @brief   下载链共用的 HTTP 响应头采集、Content-Range 解析与 URL 主机校验。
 *
 * 本模块只处理协议细节，不建立连接、不写 Flash、不校验内容：把需要在事件回调之外
 * 使用的 ETag / Content-Range 复制出来、解析 Content-Range，并按允许列表核对清单
 * URL 的主机。长度与 SHA-256 的一致性仍由各调用方在自己的写入路径上比对。
 *
 * 安全约束：download_url_host_allowed() 在允许列表为空时放行任意主机，因此允许列表
 * 属于部署配置；生产环境必须显式配置，否则清单 URL 可以指向任意 HTTPS 主机。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 一次响应中采集到的头字段；调用方必须在每次请求前重新清零或新建。
 *
 * etag 与 content_range 只在取值能完整存入时写入：长度达到容量上限（etag 128、
 * content_range 96，含 NUL）或同名头出现不同取值时置 invalid，并保留先到的值。
 * invalid 是粘滞标志且没有重置接口，因此同一结构清零之前一直有效。
 */
typedef struct {
    char etag[128];
    char content_range[96];
    bool invalid;
} download_response_headers_t;

/**
 * @brief 在 HTTP 事件回调中记录 ETag 与 Content-Range。
 *
 * 头名按 ASCII 大小写不敏感比较；同名头重复出现时，取值完全相同则接受，取值不同
 * 或超长则置 invalid。其它头一律忽略，不产生副作用。
 *
 * @param[in,out] headers 采集目标；为 NULL 时直接返回，不置错误。
 * @param[in]     name    头名。
 * @param[in]     value   头值，必须是以 NUL 结尾的字符串。
 */
void download_response_header(download_response_headers_t *headers,
                              const char *name, const char *value);

/**
 * @brief 解析带起止区间的 Content-Range，例如 `bytes 100-199/2000`。
 *
 * 只接受单区间且满足 last >= first、last < total 的形式；HTTP 416 的 Content-Range
 * 用单个星号代替区间、只给出总长，这里一律返回 false，调用方必须按"服务器无法
 * 续传"处理，不能把它当作有效的断点响应。
 *
 * @param[in]  value 待解析的头值，允许为 NULL（返回 false）。
 * @param[out] start 区间起始偏移，单位为字节。
 * @param[out] end   区间结束偏移（含），单位为字节。
 * @param[out] total 完整对象长度，单位为字节。
 *
 * @return true 解析成功且三个输出参数均已写入；false 格式不符，输出参数不被修改。
 */
bool download_parse_content_range(const char *value, size_t *start,
                                   size_t *end, size_t *total);

/**
 * @brief 判断清单 URL 的主机是否被允许。
 *
 * 要求 URL 为 https、带主机名且不含 userinfo；允许列表按逗号分隔，逐项做主机名
 * 精确匹配（大小写不敏感，不支持通配与子域匹配），端口与路径不参与判断。
 *
 * @param[in] url       清单中的下载地址。
 * @param[in] allowlist 允许的主机名列表；为空串时不做主机校验并直接放行（见文件头
 *                      的安全约束），生产必须配置。
 *
 * @return true 允许下载；false URL 形式不合法或主机不在列表中。
 */
bool download_url_host_allowed(const char *url, const char *allowlist);

/* 复制 URL 供传输层使用，只重映射旧开发期 HTTPS 主机名；
 * 输入输出缓冲不得重叠；不记录路径、查询串与凭证；
 * 持久化的清单身份字段有意保持不变。 */
bool download_server_url(const char *url, bool legacy, char *out, size_t capacity);
