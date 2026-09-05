#include "download_protocol.h"

#include <stdint.h>
#include <string.h>
#include "http_parser.h"

static bool ascii_equal(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn) return false;
    for (size_t i = 0; i < an; ++i) {
        unsigned char ac = (unsigned char)a[i], bc = (unsigned char)b[i];
        if (ac >= 'A' && ac <= 'Z') ac += 'a' - 'A';
        if (bc >= 'A' && bc <= 'Z') bc += 'a' - 'A';
        if (ac != bc) return false;
    }
    return true;
}

void download_response_header(download_response_headers_t *headers,
                              const char *name, const char *value)
{
    if (headers == NULL || name == NULL || value == NULL) return;
    char *dest;
    size_t capacity;
    if (ascii_equal(name, strlen(name), "ETag", 4)) {
        dest = headers->etag;
        capacity = sizeof(headers->etag);
    } else if (ascii_equal(name, strlen(name), "Content-Range", 13)) {
        dest = headers->content_range;
        capacity = sizeof(headers->content_range);
    } else {
        return;
    }
    size_t length = strlen(value);
    if (length >= capacity || (dest[0] != '\0' && strcmp(dest, value) != 0)) {
        headers->invalid = true;
        return;
    }
    memcpy(dest, value, length + 1U);
}

static bool read_size(const char **cursor, size_t *value)
{
    const char *p = *cursor;
    if (*p < '0' || *p > '9') return false;
    size_t number = 0;
    do {
        unsigned digit = (unsigned)(*p - '0');
        if (number > (SIZE_MAX - digit) / 10U) return false;
        number = number * 10U + digit;
        ++p;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *value = number;
    return true;
}

bool download_parse_content_range(const char *value, size_t *start,
                                   size_t *end, size_t *total)
{
    if (value == NULL || start == NULL || end == NULL || total == NULL ||
        strncmp(value, "bytes ", 6) != 0) return false;
    const char *p = value + 6;
    size_t first, last, size;
    if (!read_size(&p, &first) || *p++ != '-' ||
        !read_size(&p, &last) || *p++ != '/' ||
        !read_size(&p, &size) || *p != '\0' || last < first || last >= size) return false;
    *start = first;
    *end = last;
    *total = size;
    return true;
}

bool download_url_host_allowed(const char *url, const char *allowlist)
{
    if (url == NULL || allowlist == NULL || strncmp(url, "https://", 8) != 0) return false;
    size_t length = strlen(url);
    if (length > UINT16_MAX) return false;
    struct http_parser_url parsed;
    http_parser_url_init(&parsed);
    if (http_parser_parse_url(url, length, 0, &parsed) != 0 ||
        !(parsed.field_set & (1U << UF_HOST)) ||
        (parsed.field_set & (1U << UF_USERINFO))) return false;
    if (allowlist[0] == '\0') return true;
    const char *host = url + parsed.field_data[UF_HOST].off;
    size_t host_length = parsed.field_data[UF_HOST].len;
    for (const char *p = allowlist; *p != '\0';) {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        const char *first = p;
        while (*p != '\0' && *p != ',') ++p;
        const char *last = p;
        while (last > first && (last[-1] == ' ' || last[-1] == '\t')) --last;
        if (ascii_equal(first, (size_t)(last - first), host, host_length)) return true;
    }
    return false;
}
