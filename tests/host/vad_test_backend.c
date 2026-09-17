/* Deterministic backend for integration/failure tests, NOT a VAD accuracy model. */
#include "esp_vad.h"
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
int vad_test_fail_create, vad_test_fail_reset, vad_test_fail_process;
int vad_test_override = -1;
unsigned vad_test_calls, vad_test_resets;
vad_handle_t vad_create(vad_mode_t mode)
{
    if (vad_test_fail_create) return NULL;
    assert(mode >= 0 && mode <= 3);
    return malloc(1);
}
void vad_destroy(vad_handle_t handle) { free(handle); }
int WebRtcVad_Init(void *handle)
{
    assert(handle); ++vad_test_resets;
    return vad_test_fail_reset ? -1 : 0;
}
int WebRtcVad_set_mode(void *handle, int mode)
{
    assert(handle && mode >= 0 && mode <= 3); return 0;
}
int WebRtcVad_Process(void *handle, int rate, const int16_t *pcm, size_t n)
{
    assert(handle && rate == 16000 && n == 320);
    ++vad_test_calls;
    if (vad_test_fail_process) return -1;
    if (vad_test_override >= 0) return vad_test_override;
    for (unsigned i = 0; i < n; ++i) if (abs(pcm[i]) > 50) return 1;
    return 0;
}
