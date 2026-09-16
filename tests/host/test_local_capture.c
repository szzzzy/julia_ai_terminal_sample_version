#include "local_capture.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static local_capture_t capture;
static unsigned starts, audio, ends, aborts;
static bool limit, reject;
static uint32_t id;
static int16_t first, last;
static int64_t ms;
static bool output(void *ctx, const lc_record_t *r)
{
    (void)ctx;
    if (reject) return false;
    if (r->event == LC_START) { ++starts; id = r->id; audio = 0; }
    if (r->event == LC_AUDIO) {
        assert(r->id == id && r->index == audio);
        if (!audio) first = r->pcm[0];
        last = r->pcm[319]; ++audio;
    }
    if (r->event == LC_END) { assert(r->index == audio); ++ends; limit = r->limit; }
    if (r->event == LC_ABORT) ++aborts;
    return true;
}
static void frame(int value)
{
    int16_t pcm[320];
    for (unsigned i=0; i<320; ++i) pcm[i] = (int16_t)value;
    ms += 20;
    assert(lc_process(&capture, pcm, ms));
}
static void init(lc_mode_t mode)
{
    lc_init(&capture, output, NULL);
    lc_set_mode(&capture, mode);
    starts=audio=ends=aborts=0; limit=reject=false; ms=0;
}
int main(void)
{
    int16_t zero[320]={0}, full[320];
    for(unsigned i=0;i<320;++i)full[i]=-32768;
    assert(lc_rms_dbfs(zero,320)==-120);
    assert(lc_rms_dbfs(full,320)==0);
    double p[]={0,10,20,30}; assert(fabs(lc_percentile10(p,4)-3)<1e-10);
    init(LC_DIALOG);
    for(unsigned i=0;i<25;++i)frame(1);
    for(unsigned i=0;i<5;++i)frame(200);
    assert(starts==0);
    frame(200); assert(starts==1 && audio==25 && first==1 && last==200);
    double frozen=capture.floor.bg;
    for(unsigned i=0;i<20;++i)frame(1);
    frame(200); /* 400 ms silence - 40 ms activity credit */
    for(unsigned i=0;i<16;++i)frame(1);
    assert(ends==0 && capture.floor.bg==frozen);
    frame(1); assert(ends==1 && !limit && last==1);
    init(LC_DIALOG);
    for(unsigned i=0;i<5;++i)frame(200);
    for(unsigned i=0;i<15;++i)frame(1);
    frame(200); assert(!starts); /* old active frames rolled out */
    init(LC_WAKE);
    frame(200); assert(starts==1 && audio==1);
    for(unsigned i=0;i<24;++i)frame(1);
    assert(!ends); frame(1); assert(ends==1);
    init(LC_WAKE);
    frame(200);
    for(unsigned i=0;i<20;++i)frame(1);
    frame(200);
    for(unsigned i=0;i<8;++i)frame(1);
    assert(!ends && capture.silence==480);
    frame(1); assert(ends==1 && !limit);
    init(LC_DIALOG);
    for(unsigned i=0;i<6;++i)frame(200);
    int64_t speech_end_ms=ms;
    /* 周期性短尖峰曾反复清掉静音计数；现在应按静音结束，不能再等到15秒上限。 */
    for(unsigned i=0;i<100 && !ends;++i)frame(i%5==4 ? 200 : 1);
    assert(ends==1 && !limit && ms-speech_end_ms==1660);
    init(LC_DIALOG);
    for(unsigned i=0;i<6;++i)frame(200);
    for(unsigned pause=0;pause<4;++pause) {
        for(unsigned i=0;i<30;++i)frame(1);
        assert(!ends);
        for(unsigned i=0;i<20;++i)frame(200);
        assert(!ends && capture.silence==0);
    }
    for(unsigned i=0;i<34;++i)frame(1);
    assert(!ends);
    frame(1); assert(ends==1 && !limit);
    init(LC_DIALOG);
    for(unsigned i=0;i<6;++i)frame(200);
    frame(1); frame(200);
    assert(capture.silence==0 && !ends);
    init(LC_DIALOG);
    for(unsigned i=0;i<750;++i)frame(200);
    assert(ends==1 && limit && audio==750);
    init(LC_WAKE);
    frame(200); lc_set_mode(&capture,LC_DIALOG);
    assert(aborts==1 && !capture.active && !capture.pre_count);
    reject=true;
    int16_t pcm[320]; for(unsigned i=0;i<320;++i)pcm[i]=200;
    for(unsigned i=0;i<5;++i)assert(lc_process(&capture,pcm,ms+=20));
    assert(!lc_process(&capture,pcm,ms+=20));
    assert(!lc_process(&capture,pcm,ms+=20));
    lc_floor_t *f=&capture.floor;
    lc_floor_reset(f,-60);
    for(int t=0;t<=1000;t+=20)lc_floor_frame(f,-40,false,t);
    assert(fabs(f->bg+58.5)<1e-9 && f->slow_count==0);
    double old=f->bg;
    for(int t=1020;t<4000;t+=20)lc_floor_frame(f,-20,true,t);
    assert(f->bg==old);
    lc_floor_frame(f,-40,false,4000); assert(f->rise==0);
    lc_floor_reset(f,-45);
    for(int t=0;t<=1000;t+=20)lc_floor_frame(f,-65,false,t);
    assert(f->bg==-65); /* slow p10 snap */
    lc_floor_reset(f,-60);
    for(int t=0;t<=1000;t+=20)lc_floor_frame(f,-62,false,t);
    assert(fabs(f->bg+60.25)<1e-9);
    lc_floor_reset(f,-60);
    for(int t=0;t<=10000;t+=20)lc_floor_frame(f,-120,false,t);
    assert(!f->updated && f->bg==-60);
    double segment[400];for(unsigned i=0;i<400;++i)segment[i]=-40;
    lc_floor_feed_segment(f,segment,400,12000);
    assert(f->bg>-60 && f->bg<=-35);
    puts("local capture: RMS, p10, freeze, windows, endpoints, tail, limit, abort and failure passed");
    return 0;
}
