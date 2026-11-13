"""Replay recoding transport retry, retained selection and raw AC3 framing."""
import unittest

from native_replay import extract, run_c, source


class Ac3TransportRetryTest(unittest.TestCase):
    def test_current_unit_adopts_resolved_transport_before_accounting(self):
        audio = source('stream_audio.c')
        accounting = audio[audio.index('int ac3_recode_output_frames = 1;'):]
        branch = extract(accounting, 'if( ac3_recoding )')
        run_c(r'''
#include <assert.h>
#define STREAM_SYNC_SAMPLES 1
typedef struct STREAM STREAM;
typedef struct { int (*get_passthrough)(STREAM *); } SINK;
struct STREAM { SINK *audio_sink; int sync_mode; };
static int resolved, queries, entries, last_pts;
static int get_mode(STREAM *s) { (void)s; queries++; return resolved; }
static void stream_audio_enter_mode2_sync_samples(STREAM *s, int pts, int recode) {
    assert(recode==1); entries++; last_pts=pts; s->sync_mode=STREAM_SYNC_SAMPLES;
}
static int update(STREAM *s, int ac3_recoding, int ac3_recode_mode2_sync) {
    int passthrough=1, passthrough_active=1, chunk_pts=320;
''' + branch + r'''
    assert(passthrough_active);
    return passthrough;
}
int main(void) {
    SINK sink={get_mode}; STREAM s={&sink,0};
    resolved=2;
    assert(update(&s,1,1)==2 && entries==1 && last_pts==320);
    assert(update(&s,1,1)==2 && entries==1);
    resolved=1; s.sync_mode=0;
    assert(update(&s,1,1)==1 && entries==1 && s.sync_mode==0);
    resolved=2;
    assert(update(&s,1,0)==2 && entries==1 && s.sync_mode==0);
    int before=queries;
    assert(update(&s,0,1)==1 && queries==before && entries==1);
    return 0;
}
''')

    def test_sink_retry_selection_and_framing(self):
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#define CONFIG_SPDIF
#define DBGS
#define DBGCA2
#define WAVE_FORMAT_AC3 0x2000
static void serprintf(const char *format, ...) { (void)format; }
typedef struct {
    int format, channels, samplesPerSec, bitsPerSample, bytesPerFrame;
    int sourceChannels, sourceSamples, sourceBitsPerSample, request_channels;
} AUDIO_PROPERTIES;
typedef struct { int mode; } CTX;
typedef struct STREAM STREAM;
typedef struct { int (*get_passthrough)(STREAM *); void (*set_vol)(STREAM *); } SINK;
struct STREAM {
    AUDIO_PROPERTIES props;
    CTX *audio_ctx;
    SINK *audio_sink;
    int audio_sink_open, ac3_recode_iec_rejected;
};
static int passthrough_on, recode, ac3_force_mode2, supports_iec = 1;
static int fail_mode1, fail_mode2, fail_start, fail_set, calls, starts;
static int attempts[4];
static int libavos_get_ac3_recoding_enabled(void) { return recode && passthrough_on; }
static void spdif_set_passthrough(int mode) { passthrough_on = mode; }
int get_hdmi_supports_iec(void) { return supports_iec; }
static int audio_interface_get_passthrough(CTX *ctx) { return ctx->mode; }
static int audio_interface_set_passthrough(CTX *ctx, int mode)
{ if (fail_set) return 1; ctx->mode = mode; return 0; }
static AUDIO_PROPERTIES *stream_audio_get_sink_props(STREAM *s) { return &s->props; }
static int get_passthrough(STREAM *s) { return s->audio_ctx->mode; }
static void set_vol(STREAM *s) { (void)s; }
static int audio_interface_set_output_params(CTX *ctx, int rate, int channels,
    int content_channels, int bits, int format) {
    assert(rate == 48000 && channels == 2 && content_channels == 6);
    assert(bits == 16 && format == WAVE_FORMAT_AC3);
    assert(calls < 4);
    attempts[calls++] = ctx->mode;
    assert(ctx->mode == passthrough_on);
    return ctx->mode == 1 ? fail_mode1 : (ctx->mode == 2 ? fail_mode2 : 0);
}
static int audio_interface_start(CTX *ctx) { (void)ctx; starts++; return fail_start; }
''' + extract(source('stream_sink_audio.c'), 'static int start( STREAM *s )')
        + extract(source('stream_audio.c'), 'static int stream_audio_select_ac3_passthrough_mode(') + r'''
typedef struct { unsigned char *data; int size, error, format, fakeSize; } AUDIO_FRAME;
static void *aparser;
static int _spdif_frame_samples(AUDIO_PROPERTIES *a, int mode)
{ (void)a; (void)mode; return 1536; }
static int raw_frame(AUDIO_PROPERTIES *a, unsigned char *data, int size,
    AUDIO_FRAME *frame, int *decoded, int frame_count) {
''' + extract(source('audio_spdif.c'),
              'if (passthrough_on == 2 && !aparser && libavos_get_ac3_recoding_enabled() && a->format == WAVE_FORMAT_AC3)')
        + r'''
    return -1;
}
int main(void) {
    CTX ctx = {1};
    SINK sink = {get_passthrough, set_vol};
    STREAM s = {.audio_ctx=&ctx, .audio_sink=&sink,
        .props={.format=WAVE_FORMAT_AC3,.channels=2,.samplesPerSec=48000,
            .bitsPerSample=16,.bytesPerFrame=4,.sourceChannels=6}};
    recode=1; passthrough_on=1; fail_mode1=1;
    assert(start(&s) == 0);
    assert(calls==2 && attempts[0]==1 && attempts[1]==2 && starts==1);
    assert(s.ac3_recode_iec_rejected && s.audio_sink_open && ctx.mode==2);
    // Lazy encoder setup, seek or track reconfiguration cannot reselect IEC.
    assert(stream_audio_select_ac3_passthrough_mode(&s,"reconfigure")==2);
    calls=starts=0;
    assert(start(&s)==0 && calls==1 && attempts[0]==2);
    unsigned char ac3[128]={0x0b,0x77};
    AUDIO_FRAME f={0}; int decoded=0;
    assert(raw_frame(&s.props,ac3,sizeof(ac3),&f,&decoded,2)==0);
    assert(f.data==ac3 && f.size==128 && decoded==128);
    assert(f.fakeSize==2*1536*4 && f.format==WAVE_FORMAT_AC3);

    // A fresh playback can use IEC again; failed selection is not a preference.
    s.ac3_recode_iec_rejected=0;
    assert(stream_audio_select_ac3_passthrough_mode(&s,"new")==1);
    ctx.mode=1; fail_mode1=0; calls=starts=0;
    assert(start(&s)==0 && calls==1 && !s.ac3_recode_iec_rejected);
    assert(raw_frame(&s.props,ac3,sizeof(ac3),&f,&decoded,1)==-1);

    // Both transports failing returns to the caller without recursion.
    fail_mode1=fail_mode2=1; calls=starts=0;
    assert(start(&s)==1 && calls==2 && starts==0 && !s.audio_sink_open);
    assert(ctx.mode==2 && passthrough_on==2);
    // Startup's caller can now take the separately tested PCM fallback.
    spdif_set_passthrough(0);
    assert(!libavos_get_ac3_recoding_enabled());

    // A failed mode setter must not submit new parameters under mixed modes.
    ctx.mode=passthrough_on=1; s.ac3_recode_iec_rejected=0;
    fail_set=1; calls=starts=0;
    assert(start(&s)==1 && calls==1 && starts==0);
    fail_set=0;

    // Direct passthrough and PCM never enter this retry.
    recode=0;
    for (int mode=0;mode<=2;mode++) {
        ctx.mode=passthrough_on=mode; s.ac3_recode_iec_rejected=0;
        calls=starts=0;
        assert(start(&s)==(mode!=0));
        assert(calls==1 && !s.ac3_recode_iec_rejected);
    }
    // Recoding already using Mode 2 has only one creation attempt.
    recode=1; ctx.mode=passthrough_on=2; calls=starts=0;
    assert(start(&s)==1 && calls==1 && !s.ac3_recode_iec_rejected);
    // A play/start failure is not a constructor failure: no transport retry.
    fail_mode1=fail_mode2=0; fail_start=1;
    ctx.mode=passthrough_on=1; calls=starts=0;
    assert(start(&s)==1 && calls==1 && starts==1 && !s.ac3_recode_iec_rejected);
    supports_iec=0;
    assert(stream_audio_select_ac3_passthrough_mode(&s,"caps")==2);
    supports_iec=1; ac3_force_mode2=1;
    assert(stream_audio_select_ac3_passthrough_mode(&s,"debug")==2);
    ac3_force_mode2=0;
    assert(stream_audio_select_ac3_passthrough_mode(&s,"debug cleared")==1);
    return 0;
}
''')

    def test_backend_recoding_failure_does_not_recurse_as_ac3(self):
        branch = extract(source('audio_interface_audiotrack_java.c'),
                         'if (!ac3_recoding_enabled && at->format == WAVE_FORMAT_AC3')
        run_c(r'''
#include <assert.h>
#define WAVE_FORMAT_AC3 0x2000
#define WAVE_FORMAT_EAC3 0x4747
#define HDMI_ENCODING_E_AC3 1
#define HDMI_CHECK_BIT(flags, bit) ((flags) & (1 << (bit)))
#define DBG if(0)
#define LOG(...) ((void)0)
typedef struct { int format; } AT;
static int calls;
static int get_hdmi_supported_audio_codecs(void) { return 2; }
static int audiotrack_set_output_params(AT *at,int rate,int channels,
    int content,int bits,int format) {
    (void)at;(void)rate;(void)channels;(void)content;(void)bits;
    assert(format==WAVE_FORMAT_EAC3); calls++; return 1;
}
static int retry(AT *at,int ac3_recoding_enabled,int track_format,int rate) {
''' + branch + r'''
    return 0;
}
int main(void) {
    AT at={WAVE_FORMAT_AC3};
    assert(retry(&at,1,5,48000)==0 && calls==0);
    assert(retry(&at,0,5,48000)==1 && calls==1);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
