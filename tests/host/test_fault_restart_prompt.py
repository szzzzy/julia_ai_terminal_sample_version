"""Exercise the firmware restart notice with unavailable and stalled audio."""
import argparse
from pathlib import Path
from test_recovery_paths import run_case

parser = argparse.ArgumentParser()
parser.add_argument("--cc", required=True)
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
args.root = Path(__file__).resolve().parents[2]
args.out.mkdir(parents=True, exist_ok=True)

run_case(args, "fault_restart_prompt", r'''
#define JULIA_AVATAR_DIALOG_LISTENING 1
static const uint8_t wav[104492];
#define restart_after_issue_wav_start wav
#define restart_after_issue_wav_end (wav + sizeof(wav))
static int64_t now;
static bool talking, active, unavailable, stalled;
static int starts, stops, phase;
static int64_t esp_timer_get_time(void){return now;}
static void julia_avatar_set_dialog_phase(int p){phase=p;}
static void julia_avatar_set_dozing(bool d){assert(!d);}
static void julia_avatar_talking_start(void){talking=true;}
static void julia_avatar_talking_stop(void){talking=false;}
static bool play_local_prompt(const uint8_t *p,size_t n,const char *name,bool idle,uint32_t *g){
    assert(p==wav && n==sizeof(wav) && !idle && talking && phase==1);
    starts++;if(unavailable)return false;*g=7;active=true;return true;
}
static bool voice_playback_generation_is_active(uint32_t g){assert(g==7);return active;}
static bool voice_playback_is_active(void){return active;}
static bool voice_playback_stop_generation(uint32_t g){assert(g==7);stops++;active=false;return true;}
static void vTaskDelay(unsigned ticks){
    now+=(int64_t)ticks*10000;
    if(!stalled && now>=3280000)active=false;
}
''', [("main/fsm/julia_fsm_runtime.c", ["play_fault_restart_prompt"])], r'''
int main(void){
    play_fault_restart_prompt();
    assert(now>=3280000 && now<4000000 && !talking && !active && stops==0);
    now=0;unavailable=true;
    play_fault_restart_prompt();
    assert(now==0 && !talking && stops==0);
    now=0;unavailable=false;stalled=true;
    play_fault_restart_prompt();
    assert(now>=5264000 && now<5300000 && !talking && !active && stops==1);
    assert(starts==3);
    puts("PASS: fault notice drains before restart, skips unavailable audio, bounds stalled playback");
    return 0;
}
''')
