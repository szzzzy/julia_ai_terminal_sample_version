"""Read-only production-code probes. Run with --cc <native C compiler>."""
import argparse
import importlib.util
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("recovery", ROOT / "tests/host/test_recovery_paths.py")
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


def extract(path, *names):
    source = (ROOT / path).read_text(encoding="utf-8")
    return "\n".join(recovery.function(source, name) for name in names)


def run(cc, out, name, source, extra=()):
    c = out / (name + ".c")
    exe = out / (name + ".exe")
    c.write_text(source, encoding="utf-8")
    subprocess.run([cc, str(c), "-o", str(exe), *map(str, extra)], check=True, cwd=ROOT)
    result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    print(result.stdout.strip())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    parser.add_argument("--idf", required=True)
    parser.add_argument("--out", default="build-review-20260917")
    args = parser.parse_args()
    out = ROOT / args.out
    out.mkdir(exist_ok=True)
    includes = ["tests/host/local_capture_stubs", "tests/host/stubs", "main/voice/capture",
                "main/voice/protocol", "main/network/wss", "components/julia_board_audio/include",
                "main/voice/uplink"]
    opts = ["-I" + str(ROOT / p) for p in includes]
    cjson = Path(args.idf) / "components/json/cJSON"
    opts += ["-I" + str(cjson), str(cjson / "cJSON.c")]
    opts += [ROOT / p for p in ["components/julia_board_audio/local_capture.c",
             "components/julia_board_audio/lc_spectrum.c", "main/voice/protocol/voice_control_guard.c",
             "main/voice/uplink/voice_uplink_ring.c", "tests/host/fft_math.def"]]
    baseline = (ROOT / "tests/host/test_voice_local_capture.c").as_posix()
    source = f'#define main baseline_main\n#include "{baseline}"\n#undef main\n'
    source += r'''
#include "voice_uplink_ring.h"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static voice_uplink_ring_t s_uplink_ring;
static bool s_uplink_ring_ready=true, s_mic_streaming=true;
static unsigned s_uplink_generation=10;
static int s_uplink_pump;
static uint8_t storage[128*656];
static uint16_t lengths[128];
static uint32_t generations[128];
static void voice_uplink_pump_start_generation(void *p, uint32_t g) {(void)p;(void)g;}
static void board_audio_enable_wss_mic(bool enabled) {(void)enabled;}
static esp_err_t ring_send(const uint8_t *p,size_t n,uint32_t g) {
    return voice_uplink_ring_push_generation(&s_uplink_ring,p,n,g)==VOICE_UPLINK_PUSH_OK
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}
'''
    source += extract("main/voice/voice_service.c", "voice_service_next_uplink_generation",
                      "voice_service_resume_uplink_after_file")
    source += r'''
int main(void) {
    assert(voice_uplink_ring_init(&s_uplink_ring,storage,lengths,generations,128,656));
    assert(voice_uplink_ring_start_generation(&s_uplink_ring,10));
    assert(voice_local_capture_init(ring_send,event)==ESP_OK);
    connect(10);
    input(1);
    voice_uplink_ring_stop_generation(&s_uplink_ring);
    voice_service_resume_uplink_after_file();
    for(unsigned i=0;i<6;i++) input(200);
    printf("busy_resume: capture_epoch=%u ring_epoch=%u session_failures=%u\n",
        (unsigned)atomic_load(&epoch),voice_uplink_ring_generation(&s_uplink_ring),failures);
    assert(atomic_load(&epoch)==10 && voice_uplink_ring_generation(&s_uplink_ring)==11);
    assert(failures>0);
    return 0;
}
'''
    run(args.cc, out, "busy_resume", source, opts)

    source = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef int esp_err_t;
typedef unsigned TickType_t;
typedef uint16_t lv_color_t;
typedef void *esp_lcd_panel_handle_t;
typedef struct {void *user_data;} lv_disp_drv_t;
typedef struct {int x1,y1,x2,y2;} lv_area_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_TYPE_DATA 2
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 4
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static bool s_display_target_off,s_display_off,s_refresh_paused,s_last_flush_was_final;
static bool s_display_state_known=true;
static int64_t s_wake_started_us;
static uint64_t s_flush_count,s_flush_total_us;
static uint32_t s_flush_max_us;
static unsigned released;
static void *s_color_done=(void *)1, *s_panel_mutex=(void *)2;
static bool dma_pending;
static int draw_result=ESP_FAIL;
static int64_t esp_timer_get_time(void){return 1000;}
static bool esp_ptr_external_ram(const void *p){return false;}
static int esp_cache_msync(void *p,unsigned n,int f){return ESP_OK;}
static int xSemaphoreTake(void *s, unsigned t){return s==s_panel_mutex;}
static void xSemaphoreGive(void *s){}
static int esp_lcd_panel_draw_bitmap(void *p,int a,int b,int c,int d,const void *px){dma_pending=draw_result==ESP_OK;return draw_result;}
static bool lv_disp_flush_is_last(lv_disp_drv_t *d){return true;}
static void lv_disp_flush_ready(lv_disp_drv_t *d){released++;}
static bool lvgl_port_lock(TickType_t t){return true;}
static void lvgl_port_unlock(void){}
static void lv_refr_now(void *p);
'''
    source += extract("main/display/lvgl_port/lvgl_port.c", "lvgl_port_draw_bitmap_sync", "lvgl_flush_cb", "lvgl_port_refr_now_sync")
    source += r'''
static void lv_refr_now(void *p){lv_disp_drv_t d={0};lv_area_t a={0,0,1,1};lv_color_t pixel[4];lvgl_flush_cb(&d,&a,pixel);}
int main(void){int result=lvgl_port_refr_now_sync(100);printf("display_failure: draw=ESP_FAIL refresh_result=%d buffer_released=%u\n",result,released);assert(result==ESP_OK && released==1);draw_result=ESP_OK;released=0;result=lvgl_port_refr_now_sync(100);printf("display_timeout: dma_pending=%d refresh_result=%d buffer_released=%u\n",dma_pending,result,released);assert(result==ESP_OK && dma_pending && released==1);return 0;}
'''
    run(args.cc, out, "display_failure", source)

    # Extract the actual OTA loop, retaining its EAGAIN counter branch and replacing
    # the unreachable payload-processing tail for this all-timeouts scenario.
    ota = (ROOT / "main/ota/ota_engine.c").read_text(encoding="utf-8")
    begin = ota.index("    unsigned empty_reads = 0;", ota.index("static void ota_engine_task(void *pvParameter)\n{"))
    end = ota.index("        empty_reads = 0;", begin)
    source = r'''
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#define OTA_MAX_EMPTY_READS 600U
#define BUFFSIZE 1024
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define NATIVE_OTA_FAILURE_NETWORK_TIMEOUT 1
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
static unsigned elapsed_ms, calls;
static int esp_http_client_read(void *c,void *b,int n){elapsed_ms+=5000;calls++;return -ESP_ERR_HTTP_EAGAIN;}
static bool esp_http_client_is_complete_data_received(void *c){return false;}
static void vTaskDelay(unsigned ms){elapsed_ms+=ms;}
int main(void){void *client=0;char ota_write_data[1024];int failure_reason=0;
'''
    source += ota[begin:end] + "\n}\ncleanup:\n"
    source += r'''
printf("ota_timeout: read_calls=%u elapsed_ms=%u failure=%d\n",calls,elapsed_ms,failure_reason);
assert(calls==601 && elapsed_ms==3011000 && failure_reason==1);return 0;}
'''
    run(args.cc, out, "ota_timeout", source)


if __name__ == "__main__":
    main()
