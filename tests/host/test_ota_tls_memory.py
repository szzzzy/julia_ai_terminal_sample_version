"""Exercise actual OTA error classification with ESP-TLS boundary failures."""
import argparse
from pathlib import Path
import subprocess
from test_recovery_paths import function

parser = argparse.ArgumentParser()
parser.add_argument('--cc', required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
args.out.mkdir(parents=True, exist_ok=True)
body = function((root / 'main/ota/ota_engine.c').read_text(encoding='utf-8'),
                'ota_http_open_failure_reason')
harness = r'''
#include <assert.h>
#include <stdio.h>
typedef int esp_err_t;
typedef void *esp_http_client_handle_t;
typedef int native_ota_failure_reason_t;
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED 0x801a
#define ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED 0x8015
#define MBEDTLS_ERR_SSL_ALLOC_FAILED -0x7f00
#define NATIVE_OTA_FAILURE_OUT_OF_MEMORY 17
#define NATIVE_OTA_FAILURE_TLS_VERIFY_FAILED 3
#define NATIVE_OTA_FAILURE_NETWORK_TIMEOUT 2
#define ESP_LOGE(...) ((void)0)
static int code, flags, error;
static int esp_http_client_get_and_clear_last_tls_error(void *c, int *v, int *f) {
    *v=code; *f=flags; return error;
}
'''
harness += body + r'''
int main(void) {
    /* ESP-IDF ssl_setup records -ret, i.e. a positive 0x7f00. */
    code=0x7f00; error=0x8017;
    assert(ota_http_open_failure_reason(0)==NATIVE_OTA_FAILURE_OUT_OF_MEMORY);
    code=-0x7f00;
    assert(ota_http_open_failure_reason(0)==NATIVE_OTA_FAILURE_OUT_OF_MEMORY);
    code=0; error=ESP_ERR_NO_MEM;
    assert(ota_http_open_failure_reason(0)==NATIVE_OTA_FAILURE_OUT_OF_MEMORY);
    error=ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED;
    assert(ota_http_open_failure_reason(0)==NATIVE_OTA_FAILURE_TLS_VERIFY_FAILED);
    error=0; flags=8;
    assert(ota_http_open_failure_reason(0)==NATIVE_OTA_FAILURE_TLS_VERIFY_FAILED);
    flags=0; error=0x8001;
    assert(ota_http_open_failure_reason(0)==NATIVE_OTA_FAILURE_NETWORK_TIMEOUT);
    puts("PASS: TLS allocation, certificate and network failures stay distinct");
    return 0;
}
'''
source = args.out / 'ota_tls_memory.c'
binary = args.out / 'ota_tls_memory.exe'
source.write_text(harness, encoding='utf-8')
subprocess.run([args.cc, str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary.resolve())], check=True)
