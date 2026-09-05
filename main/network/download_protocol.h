#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char etag[128];
    char content_range[96];
    bool invalid;
} download_response_headers_t;

/* 在 HTTP header callback 中复制需要跨回调使用的值；拒绝截断或冲突的重复头。 */
void download_response_header(download_response_headers_t *headers,
                              const char *name, const char *value);
bool download_parse_content_range(const char *value, size_t *start,
                                   size_t *end, size_t *total);
/* 与 ESP-IDF HTTP 客户端共用 URL parser；清单不得携带 userinfo。 */
bool download_url_host_allowed(const char *url, const char *allowlist);
