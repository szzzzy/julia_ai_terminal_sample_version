#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Credentials are HTTP header values: reject empty/control/non-ASCII values
 * locally without printing them or contacting the server. */
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

static inline uint32_t wss_auth_retry_seconds(bool rejected, uint32_t normal)
{
    return rejected && normal < 60U ? 60U : normal;
}

static inline uint32_t wss_close_retry_floor(uint16_t code)
{
    if (code==4001 || code==4401 || code==4404) return 60U;
    if (code==4408 || code==4410 || code==4411) return 30U;
    return 0;
}
