"""Verify local prompt mouth visibility on closed-eye portraits."""
import argparse
from pathlib import Path
from test_recovery_paths import run_case

parser = argparse.ArgumentParser()
parser.add_argument('--cc', required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
args.root = Path(__file__).resolve().parents[2]
args.out.mkdir(parents=True, exist_ok=True)
run_case(args, 'prompt_mouth', r'''
#define AVATAR_PCM_HOLD_MS 180U
#define JULIA_AVATAR_DIALOG_LISTENING 1
typedef int julia_avatar_dialog_phase_t;
typedef int avatar_mouth_shape_t;
static bool s_ready=true,s_dozing,s_talking,visible,lock_ok=true;
static int s_dialog_phase=1,shape;
static uint8_t s_target_mouth_level,s_mouth_level;
static uint32_t s_last_pcm_ms,s_smoothed_rms;
static int64_t now=1000000;
static bool lvgl_port_lock(unsigned t){return lock_ok;}
static void lvgl_port_unlock(void){}
static int64_t esp_timer_get_time(void){return now;}
static void avatar_mouth_set_visible(bool v){visible=v;}
static void avatar_mouth_set_shape(int s,uint32_t rms){shape=s;}
''', [('main/ui/julia_avatar.c', ['avatar_sync_mouth', 'julia_avatar_talking_start',
                                   'julia_avatar_talking_stop'])], r'''
int main(void){
    avatar_sync_mouth();assert(!visible);
    julia_avatar_talking_start();assert(visible && shape==0);
    s_target_mouth_level=3;avatar_sync_mouth();assert(visible && shape==3);
    /* Reapplying LISTEN cannot hide active local speech. */
    avatar_sync_mouth();assert(visible && shape==3);
    julia_avatar_talking_stop();assert(!visible && shape==0);
    lock_ok=false;julia_avatar_talking_start();assert(!visible);
    lock_ok=true;avatar_sync_mouth();assert(visible);
    s_target_mouth_level=2;avatar_sync_mouth();assert(shape==2);
    now+=181000;avatar_sync_mouth();assert(shape==0);
    s_dozing=true;avatar_sync_mouth();assert(!visible);
    s_dozing=false;julia_avatar_talking_stop();assert(!visible);
    s_dialog_phase=3;avatar_sync_mouth();assert(visible && shape==0);
    s_ready=false;julia_avatar_talking_start();
    s_ready=true;s_dialog_phase=1;avatar_sync_mouth();assert(visible);
    puts("PASS: local prompt mouth, stop, UI lock retry, stale PCM and sleep visibility");
    return 0;
}
''')
