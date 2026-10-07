"""Check the production startup trigger without changing Bluetooth/speed guards."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_video_pcm_startup import block


class PcmStartupCorrectionTest(unittest.TestCase):
    def test_trusted_startup_phase_check(self):
        source = (Path(__file__).resolve().parents[1] / 'Source/stream_sync.c').read_text()
        decision = block(source, 'if( s->pcm_startup_correction_pending )')
        constants = '\n'.join(re.findall(
            r'^#define STREAM_PCM_STARTUP_(?:CORRECTION_\w+|DIRECT_STREAK)\s+\d+',
            source, re.M))
        code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define CONFIG_AUDIO_AC3
#define DBG if (0)
#define serprintf printf
#define ABS(x) abs(x)
''' + constants + r'''
typedef struct STREAM STREAM;
typedef struct { int (*get_passthrough)(STREAM *); } SINK;
struct STREAM {
    SINK *audio_sink;
    int pcm_startup_correction_pending, pcm_startup_correction_seek_epoch, seek_epoch;
    int pcm_startup_correction_speed_epoch, audio_speed_diag_epoch;
    int pcm_startup_seed_delay_ms, at_speed_epoch_active, video_speed_num, video_speed_den;
};
typedef struct { int has_dynamic_evidence, dynamic_evidence_streak, dynamic_evidence_ms; } STATUS;
static int mode, recode, speed_enabled;
static int passthrough(STREAM *s) { (void)s; return mode; }
static int libavos_get_ac3_recoding_enabled(void) { return recode; }
static int audio_interface_is_audio_speed_enabled(void) { return speed_enabled; }
static float audio_interface_get_audio_speed(void) { return speed_enabled ? 1.6f : 1.0f; }
static int stream_get_pcm_startup_seed_delay_ms(STREAM *s) { (void)s; return 200; }
static int stream_get_atempo_delay(STREAM *s) { (void)s; return 30; }
static int check(STREAM *s, STATUS delay_status) {
    int pcm_startup_request_correction = 0;
''' + decision + r'''
    return pcm_startup_request_correction;
}
int main(void) {
    SINK sink = {passthrough};
    STREAM initial = {.audio_sink = &sink, .pcm_startup_correction_pending = 1,
        .pcm_startup_correction_seek_epoch = 3, .seek_epoch = 3,
        .pcm_startup_correction_speed_epoch = 7, .audio_speed_diag_epoch = 7,
        .pcm_startup_seed_delay_ms = 86, .video_speed_num = 1, .video_speed_den = 1};
    STREAM s = initial;
    STATUS evidence = {1, 10, 76};
    // Sony seek: latency changes only 10ms, but renderer phase is about 105ms.
    // Ask the existing renderer to inspect phase once direct timing is trusted.
    assert(check(&s, evidence));
    assert(!s.pcm_startup_correction_pending);
    assert(!check(&s, evidence)); // exactly once per armed startup
    s = initial; evidence.dynamic_evidence_ms = 86;
    assert(check(&s, evidence)); // equal latency also says nothing about wall phase
    // Retain the Bluetooth warm-up protection; fallback evidence cannot trigger.
    s = initial; evidence.dynamic_evidence_streak = 9;
    assert(!check(&s, evidence) && s.pcm_startup_correction_pending);
    evidence.dynamic_evidence_streak = 10; evidence.has_dynamic_evidence = 0;
    assert(!check(&s, evidence) && s.pcm_startup_correction_pending);
    evidence.has_dynamic_evidence = 1;
    for (mode = 1; mode <= 3; mode++) {
        s = initial;
        assert(!check(&s, evidence) && !s.pcm_startup_correction_pending);
    }
    mode = 0; recode = 1; s = initial;
    assert(!check(&s, evidence) && !s.pcm_startup_correction_pending);
    recode = 0; s = initial; s.seek_epoch++;
    assert(!check(&s, evidence) && !s.pcm_startup_correction_pending);
    s = initial; s.audio_speed_diag_epoch++;
    assert(!check(&s, evidence) && s.pcm_startup_correction_pending);
    assert(s.pcm_startup_seed_delay_ms == 230);
    assert(s.pcm_startup_correction_speed_epoch == s.audio_speed_diag_epoch);
    // The original latency-delta trigger is unchanged for software speed,
    // a retained PlaybackParams checkpoint and an uncommitted return to 1x.
    int deltas[] = {-501, -500, -16, -15, 0, 15, 16, 500, 501};
    for (int policy = 0; policy < 4; policy++) {
        for (unsigned i = 0; i < sizeof(deltas)/sizeof(deltas[0]); i++) {
            s = initial; s.pcm_startup_seed_delay_ms = 1000;
            speed_enabled = policy == 1;
            s.at_speed_epoch_active = policy == 2;
            s.video_speed_num = policy == 3 ? 2 : 1;
            evidence.dynamic_evidence_ms = 1000 + deltas[i];
            int expected = abs(deltas[i]) <= 500 && (policy == 0 || abs(deltas[i]) >= 16);
            assert(check(&s, evidence) == expected);
            assert(!s.pcm_startup_correction_pending);
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'startup.c').write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(root / 'startup.c'), '-o', str(root / 'startup')], check=True)
            subprocess.run([str(root / 'startup')], check=True)


if __name__ == '__main__':
    unittest.main()
