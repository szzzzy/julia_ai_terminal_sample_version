#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "wss_auth_policy.h"

int main(void)
{
    assert(!wss_auth_token_valid(NULL));
    assert(!wss_auth_token_valid(""));
    assert(!wss_auth_token_valid("dummy\r\nInjected: value"));
    assert(!wss_auth_token_valid("dummy token"));
    assert(wss_auth_token_valid("test-only.a-b_c~1+/="));
    const char *responses[] = {"HTTP/1.1 401 Unauthorized\r\n",
                               "HTTP/1.0 403 Forbidden\r\n"};
    for (unsigned i = 0; i < 2; ++i) {
        assert(wss_auth_response_rejected(responses[i], strlen(responses[i])));
        for (size_t n = 0; n < 13; ++n)
            assert(!wss_auth_response_rejected(responses[i], n));
    }
    assert(!wss_auth_response_rejected("HTTP/1.1 101 Switching\r\n", 24));
    assert(!wss_auth_response_rejected("HTTP/1.1 503 Busy\r\n", 19));
    assert(!wss_auth_response_rejected("HTTP/1.1 4010 invalid\r\n", 23));
    assert(!wss_auth_response_rejected("HTTP/1.1 200 OK\r\nX: 401", 24));
    assert(wss_auth_retry_seconds(false,5)==5);
    assert(wss_auth_retry_seconds(true,5)==60);
    assert(wss_auth_retry_seconds(true,120)==120);
    assert(wss_close_retry_floor(4001)==60);
    assert(wss_close_retry_floor(4408)==30);
    assert(wss_close_retry_floor(4410)==30 && wss_close_retry_floor(4411)==30);
    assert(wss_close_retry_floor(1000)==0);
    puts("PASS: missing/invalid credentials and bounded HTTP auth status parsing");
    return 0;
}
