"""Missing optional FFmpeg accessors must not manufacture media spans."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_video_pcm_startup import block


class AtempoStateTest(unittest.TestCase):
    def test_missing_legacy_and_extended_state(self):
        text = (Path(__file__).resolve().parents[1] / 'Source/stream_filter_audio_atempo.c').read_text()
        helpers = '\n'.join(block(text, name) for name in (
            'static int atempo_get_state(', 'static void atempo_omap_reset(',
            'static void atempo_omap_push(', 'static void atempo_omap_record(',
            'int stream_filter_audio_atempo_get_audit_state(STREAM_FILTER_AUDIO *f,',
            'int stream_filter_audio_atempo_lookup_output_media(STREAM_FILTER_AUDIO *f,'))
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef uint64_t UINT64;
typedef int64_t INT64;
typedef struct {} AVFilterContext;
static void (*avfilter_atempo_get_state)(AVFilterContext *,int *,int64_t *,int64_t *,int64_t *,int64_t *,double *);
static void (*avfilter_atempo_get_state_v2)(AVFilterContext *,int *,int64_t *,int64_t *,int64_t *,int64_t *,double *,int64_t *);
#define ATEMPO_OMAP_SIZE 64
struct atempo_omap_entry { UINT64 out_start; int nsamples; INT64 media_start, media_span; };
struct ctx {
    int sample_rate;
    AVFilterContext *atempo_ctx;
    struct atempo_omap_entry omap[64];
    int omap_head, omap_count;
    UINT64 omap_write_cursor;
    INT64 omap_prev_media;
};
typedef struct { struct ctx *priv; } STREAM_FILTER_AUDIO;
''' + helpers + r'''
static void legacy(AVFilterContext *c,int *ring,int64_t *a,int64_t *b,
    int64_t *in,int64_t *out,double *tempo) {
    (void)c;(void)a;(void)b;(void)out;(void)tempo;
    if(ring)*ring=100; if(in)*in=1100;
}
static void extended(AVFilterContext *c,int *ring,int64_t *a,int64_t *b,
    int64_t *in,int64_t *out,double *tempo,int64_t *media) {
    legacy(c,ring,a,b,in,out,tempo); if(media)*media=900;
}
int main(void) {
    AVFilterContext af;
    struct ctx ctx={.sample_rate=48000,.atempo_ctx=&af};
    STREAM_FILTER_AUDIO f={&ctx};
    int ring=-1, rate=0; int64_t in=0,media=-1,span=0;
    atempo_omap_reset(&ctx,0);
    assert(!atempo_get_state(&af,&ring,0,0,&in,0,0,&media));
    assert(!stream_filter_audio_atempo_get_audit_state(&f,&in,0,&ring,&rate));
    atempo_omap_record(&ctx,480,0,1);
    atempo_omap_record(&ctx,480,0,1);
    assert(ctx.omap_write_cursor==960 && ctx.omap_count==0);
    assert(!stream_filter_audio_atempo_lookup_output_media(&f,0,480,&span,&rate));
    avfilter_atempo_get_state=legacy;
    assert(atempo_get_state(&af,&ring,0,0,&in,0,0,&media));
    assert(media==-1 && in-ring==1000);
    assert(stream_filter_audio_atempo_get_audit_state(&f,&in,0,&ring,&rate));
    avfilter_atempo_get_state_v2=extended;
    assert(atempo_get_state(&af,&ring,0,0,&in,0,0,&media) && media==900);
    // Regained counters seed a new baseline, without attributing the unknown gap.
    atempo_omap_record(&ctx,480,1,900);
    assert(ctx.omap_count==0 && ctx.omap_write_cursor==1440);
    atempo_omap_record(&ctx,480,1,1620);
    assert(stream_filter_audio_atempo_lookup_output_media(&f,1440,480,&span,&rate));
    assert(span==720 && rate==48000);
    assert(!stream_filter_audio_atempo_lookup_output_media(&f,1400,480,&span,&rate));
    // Normal patched startup still records the entire first burst.
    atempo_omap_reset(&ctx,0);
    atempo_omap_record(&ctx,480,1,720);
    assert(stream_filter_audio_atempo_lookup_output_media(&f,0,480,&span,&rate) && span==720);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / 'state.c', Path(tmp) / 'state'
            source.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall',
                            '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
