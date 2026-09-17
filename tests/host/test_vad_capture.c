#include "local_capture.h"
#include "lc_vad.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
extern int vad_test_fail_create, vad_test_fail_reset, vad_test_fail_process, vad_test_override;
extern unsigned vad_test_calls, vad_test_resets;
static local_capture_t c;
static lc_vad_t vad;
static int16_t history[1024][320], tone[320], dc[320], quiet[320];
static unsigned pos, first_pos, audio, starts, ends, aborts;
static bool limit;
static int reject = -1;
static bool output(void *ctx, const lc_record_t *r)
{
    (void)ctx;
    if (r->event == reject) return false;
    if (r->event == LC_START) { ++starts; audio = 0; first_pos = pos-c.pre_count+1; }
    if (r->event == LC_AUDIO) {
        assert(r->index == audio);
        assert(!memcmp(r->pcm, history[(first_pos+audio)%1024], sizeof(tone)));
        ++audio;
    }
    if (r->event == LC_END) { assert(r->index == audio); ++ends; limit = r->limit; }
    if (r->event == LC_ABORT) ++aborts;
    return true;
}
static bool frame(const int16_t *pcm)
{
    memcpy(history[pos%1024], pcm, sizeof(tone));
    bool ok = lc_process(&c, pcm, (int64_t)pos*20);
    ++pos; return ok;
}
static void init(lc_mode_t mode)
{
    lc_vad_destroy(&vad);
    vad_test_fail_create = vad_test_fail_reset = vad_test_fail_process = 0;
    vad_test_override = -1; reject = -1;
    vad_test_calls = vad_test_resets = 0;
    assert(lc_vad_init(&vad, 1));
    lc_init(&c, output, NULL); c.fft_enabled = true;
    assert(lc_set_voice_detector(&c, lc_vad_frame, lc_vad_reset, &vad, 500, 600));
    lc_set_mode(&c, mode);
    pos = audio = starts = ends = aborts = 0; limit = false;
}
int main(void)
{
    for (unsigned i = 0; i < 320; ++i) {
        tone[i] = (int16_t)(2000*sin(6.283185307179586*i/16)); dc[i] = 2000;
    }
    for (lc_mode_t mode=LC_WAKE; mode<=LC_DIALOG; ++mode) {
        unsigned need = mode==LC_WAKE ? 1 : 6, tail = mode==LC_WAKE ? 25 : 30;
        init(mode);
        /* FFT rejects DC even with VAD=true. VAD rejects tone despite FFT pass. */
        vad_test_override=1;
        for(unsigned i=0;i<25;++i) assert(frame(dc));
        assert(!starts && vad_test_calls==25);
        vad_test_override=0;
        for(unsigned i=0;i<25;++i) assert(frame(tone));
        assert(!starts);
        /* Energy rejects quiet despite VAD=true; classifier still sees every frame. */
        vad_test_override=1;
        for(unsigned i=0;i<25;++i) assert(frame(quiet));
        assert(!starts && vad_test_calls==75 && vad_test_resets==1);
        for(unsigned i=0;i<need;++i) assert(frame(tone));
        assert(starts==1 && audio==25);
        /* Same high-energy FFT-passing tone rejected by VAD must stop extending. */
        vad_test_override=0;
        for(unsigned i=0;i<tail-1;++i) { assert(frame(tone)); assert(!ends); }
        assert(frame(tone)); assert(ends==1 && !limit && audio==25+tail);
        vad_test_override=1;
        for(unsigned i=0;i<need;++i) assert(frame(tone));
        assert(starts==2);
        while(ends<2) assert(frame(tone));
        assert(limit && audio==(mode==LC_WAKE?400:750));
        init(mode);
        for(unsigned i=0;i<need;++i) assert(frame(tone));
        for(unsigned i=0;i<tail-1;++i) assert(frame(quiet));
        assert(!ends); assert(frame(tone));
        assert(c.silence==(tail-1)*20-(mode==LC_WAKE?80:40));
        lc_set_mode(&c, LC_OFF); assert(aborts==1);
        unsigned calls=vad_test_calls;
        assert(frame(tone) && vad_test_calls==calls);
        lc_set_mode(&c, mode); assert(frame(tone) && vad_test_resets==2);
    }
    init(LC_DIALOG);
    vad_test_fail_process=1; assert(!frame(tone) && c.failed && !starts);
    vad_test_fail_process=0; assert(!frame(tone));
    lc_set_mode(&c, LC_OFF); lc_set_mode(&c, LC_DIALOG); assert(frame(tone));
    c.voice_reset_pending=true; vad_test_fail_reset=1; assert(!frame(tone));
    lc_vad_destroy(&vad); vad_test_fail_create=1; assert(!lc_vad_init(&vad,1));
    assert(!lc_vad_init(&vad,4));
    for(int event=LC_START;event<=LC_END;++event) {
        init(LC_WAKE);
        if(event==LC_END) { assert(frame(tone)); for(int i=0;i<24;++i) assert(frame(quiet)); }
        reject=event;
        assert(!frame(event==LC_END?quiet:tone));
        assert(c.failed && !frame(tone));
    }
    lc_vad_destroy(&vad);
    puts("VAD adapter + energy/FFT combination: onset, continuous feed, raw/order, tails, limits, reset and errors passed (test backend)");
    return 0;
}
