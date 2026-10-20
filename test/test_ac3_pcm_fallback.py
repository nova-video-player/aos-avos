"""Replay recoding preference and PCM recovery using production functions."""
import unittest

from native_replay import extract, run_c, source


POLICY = r'''
#include <assert.h>
#include <string.h>
#define CONFIG_SPDIF
static void serprintf(const char *format, ...) { (void)format; }
static int ac3_recoding_enabled, passthrough_on, target_stereo;
static int interface_exits, interface_inits;
static int spdif_is_passthrough_on(void) { return passthrough_on; }
static void spdif_set_passthrough(int mode) { passthrough_on = mode; }
static void audio_interface_exit(void) { interface_exits++; }
static void audio_interface_init(void) { interface_inits++; }
static void libavos_set_ac3_recode_target_stereo(int stereo) { target_stereo = stereo; }
''' + extract(source('libavos.c'), 'int libavos_get_ac3_recoding_enabled(') + extract(
    source('libavos.c'), 'void libavos_set_passthrough(')


class Ac3PcmFallbackTest(unittest.TestCase):
    def test_active_mode_and_next_playback_preference(self):
        run_c(POLICY + r'''
int main(void) {
    for (int mode = 0; mode <= 2; mode++) {
        libavos_set_passthrough(mode);
        assert(!libavos_get_ac3_recoding_enabled());
        assert(passthrough_on == mode);
    }
    libavos_set_passthrough(3);
    assert(libavos_get_ac3_recoding_enabled() && passthrough_on == 1);
    spdif_set_passthrough(2);
    assert(libavos_get_ac3_recoding_enabled());
    // A PCM fallback changes the active pipeline, not the configured preference.
    spdif_set_passthrough(0);
    assert(!libavos_get_ac3_recoding_enabled());
    assert(ac3_recoding_enabled == 1);
    // The app reapplies the setting on the next open.
    libavos_set_passthrough(3);
    assert(libavos_get_ac3_recoding_enabled() && passthrough_on == 1);
    assert(target_stereo == 0);
    assert(interface_exits == 5 && interface_inits == 5);
    return 0;
}
''')

    def test_recovery_reopens_pcm_and_stays_pcm_on_failure(self):
        run_c(POLICY + r'''
#define WAVE_FORMAT_PCM 1
typedef struct {
    int valid, format, channels, samplesPerSec, bitsPerSample;
    int sourceChannels, sourceSamples, sourceBitsPerSample;
    int bytesPerFrame, bytesPerSec, request_channels;
} AUDIO_PROPERTIES;
typedef struct STREAM STREAM;
typedef struct {
    void (*set_passthrough)(STREAM *, int);
    int (*start)(STREAM *);
} SINK;
struct STREAM {
    AUDIO_PROPERTIES *audio, audio_sink_props;
    SINK *audio_sink;
    void *audio_dec;
};
static int sink_mode, decoder_open, encoder_present, filters_open;
static int decoder_closes, filter_closes, sink_starts, failure;
static void set_passthrough(STREAM *s, int mode) { (void)s; sink_mode = mode; }
static int stream_audio_requested_passthrough_for_format(int format)
{ (void)format; return passthrough_on; }
''' + extract(source('stream_audio.c'), 'static void stream_audio_init_sink_defaults(')
        + extract(source('stream_audio.c'), 'AUDIO_PROPERTIES *stream_audio_get_sink_props(')
        + extract(source('stream_audio.c'), 'void stream_audio_copy_sink_from_source(') + r'''
static void stream_close_audio_filter(STREAM *s)
{ (void)s; filters_open = encoder_present = 0; filter_closes++; }
static void stream_close_audio_dec(STREAM *s)
{ s->audio_dec = NULL; decoder_open = 0; decoder_closes++; }
static void *stream_get_audio_dec(AUDIO_PROPERTIES *a) { return a; }
static int stream_open_audio_dec(STREAM *s) {
    // Decoder selection must already see PCM policy, before opening anything.
    assert(!libavos_get_ac3_recoding_enabled() && !passthrough_on);
    if (failure == 1) return 1;
    decoder_open = 1;
    // Model a stereo PCM route; production sink copying must honor this request.
    s->audio->request_channels = 2;
    s->audio->bitsPerSample = 16;
    return 0;
}
static int stream_open_audio_filter(STREAM *s) {
    (void)s;
    assert(!libavos_get_ac3_recoding_enabled());
    if (failure == 2) return 1;
    filters_open = 1;
    encoder_present = libavos_get_ac3_recoding_enabled();
    return 0;
}
static int start(STREAM *s) {
    assert(!libavos_get_ac3_recoding_enabled() && sink_mode == 0);
    assert(!encoder_present && decoder_open && filters_open);
    assert(s->audio_sink_props.format == WAVE_FORMAT_PCM);
    assert(s->audio_sink_props.channels == 2);
    assert(s->audio_sink_props.samplesPerSec == 44100);
    assert(s->audio_sink_props.bytesPerFrame == 4);
    assert(s->audio_sink_props.bytesPerSec == 176400);
    sink_starts++;
    return failure == 3;
}
''' + extract(source('stream_video.c'), 'static int stream_restart_audio_as_pcm(') + r'''
int main(void) {
    SINK sink = {set_passthrough, start};
    // Cover recoding with either transport and ordinary direct passthrough.
    for (int preference = 1; preference <= 3; preference++) {
        for (int transport = 1; transport <= 2; transport++) {
            for (failure = 0; failure <= 3; failure++) {
                AUDIO_PROPERTIES a = {
                    .valid=1, .format=0x4747, .channels=6, .samplesPerSec=48000,
                    .bitsPerSample=16, .sourceChannels=6, .sourceSamples=44100
                };
                STREAM s = {.audio=&a, .audio_sink=&sink, .audio_dec=&a};
                libavos_set_passthrough(preference);
                spdif_set_passthrough(transport);
                set_passthrough(&s, transport);
                encoder_present = preference == 3;
                decoder_open = filters_open = 1;
                decoder_closes = filter_closes = sink_starts = 0;
                int exits_before = interface_exits;
                assert(stream_restart_audio_as_pcm(&s, "sink failure") == (failure != 0));
                assert(!libavos_get_ac3_recoding_enabled() && !passthrough_on);
                assert(sink_mode == 0 && !encoder_present);
                assert(ac3_recoding_enabled == (preference == 3));
                // Recovery must not tear down the audio interface containing this sink.
                assert(interface_exits == exits_before);
                assert(decoder_closes >= 1 && filter_closes >= 1);
                assert(sink_starts == (failure == 0 || failure == 3));
                if (failure) assert(!decoder_open);
                else {
                    stream_audio_copy_sink_from_source(&s);
                    assert(s.audio_sink_props.format == WAVE_FORMAT_PCM);
                    assert(stream_restart_audio_as_pcm(&s, "repeat") == 1);
                }
            }
        }
    }
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
