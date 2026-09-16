#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* 凭据要作为 HTTP 头值使用：空串、控制字符与非 ASCII 在本机直接拒绝，既不打印
 * 凭据内容也不发起 TLS 连接（调用点是 wss_transport.c 的 wss_connect()，位于
 * esp_tls_init() 之前）。即空 token 属于本地拒绝，不依赖服务器回 401/4401。 */
static inline bool wss_auth_token_valid(const char *token)
{
    if (token == NULL || *token == '\0') return false;
    for (const unsigned char *p = (const unsigned char *)token; *p; ++p)
        if (*p <= 0x20 || *p >= 0x7f) return false;
    return true;
}

static inline bool wss_auth_response_rejected(const char *headers, size_t len)
{
    if (headers == NULL || len < 13) return false;
    if (memcmp(headers, "HTTP/1.1 ", 9) && memcmp(headers, "HTTP/1.0 ", 9))
        return false;
    return (!memcmp(headers + 9, "401", 3) || !memcmp(headers + 9, "403", 3)) &&
           (headers[12] == ' ' || headers[12] == '\r');
}

/* 被服务器拒绝认证后（HTTP 401/403 或 WS 4401），重连间隔单位秒且不低于 60；
 * 下限依据见 docs/MULTIDEVICE_CONTROL_V1.md、docs/MULTIDEVICE_VALIDATION_20260910.md。
 * 普通网络故障不受此下限影响，仍用调用方传入的间隔。 */
static inline uint32_t wss_auth_retry_seconds(bool rejected, uint32_t normal)
{
    return rejected && normal < 60U ? 60U : normal;
}

/* 服务端以这些 CLOSE 码结束会话时的重连下限（单位：秒）；0 表示不抬高，沿用普通
 * 重连间隔。码值含义与 60/30 的取值依据见 docs/MULTIDEVICE_CONTROL_V1.md。 */
static inline uint32_t wss_close_retry_floor(uint16_t code)
{
    if (code==4001 || code==4401 || code==4404) return 60U;
    if (code==4408 || code==4410 || code==4411) return 30U;
    return 0;
}
