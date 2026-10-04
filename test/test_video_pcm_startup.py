"""Replay first-clock handoff guards and bounded slew from production C."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class PcmStartupTest(unittest.TestCase):
    def test_handoff_and_lifecycle_exclusions(self):
        source = (Path(__file__).resolve().parents[1] / 'Source/codec_sfdec2.c').read_text()
        helper = block(source, 'static int pcm_startup_anchor_target_l(')
        apply = block(source, 'if (preserve_pcm_startup_anchor)')
        slew = block(source, 'if (p->slew_active &&')
        fields = sorted(set(re.findall(r'p->(\w+)', helper + apply + slew)) - {'speed_cadence'})
        private = '\n'.join(
            f'{"const void *" if name.endswith("handle") else "int64_t "}{name};'
            for name in fields)
        code = r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define DBGSI if (0)
#define serprintf(...) ((void)0)
typedef int64_t INT64;
typedef struct { int valid; } AUDIO;
typedef struct {
    AUDIO *audio;
    int put_time_mode, audio_time, paused, seek_paused, play_n_video_frames;
    int audio_start_pending, audio_resume_pending, seek_epoch;
} STREAM;
typedef struct {
    struct { int valid, epoch; } speed_cadence;
''' + private + r'''
} priv_t;
''' + helper + r'''
static void apply(priv_t *p, STREAM *s, INT64 startup_target_ns) {
    int preserve_pcm_startup_anchor = 1, time = 22;
    INT64 startup_offset_ns = p->render_offset_ns;
    (void)s; (void)time;
''' + apply + r'''
}
static void step(priv_t *p, STREAM *s, int distinct) {
    struct { int duration; } frame = { 41 }, *f = &frame;
    int mode2_new_slew_frame = 0, mode2_dynamic_active = 0;
    int pcm_startup_new_slew_frame = distinct;
''' + slew + r'''
}
int main(void) {
    AUDIO audio = { 1 };
    STREAM stream = { .audio = &audio, .put_time_mode = 1,
                      .audio_time = 96, .seek_epoch = 1 }, *s = &stream;
    priv_t initial = { .render_offset_ns = 11822397541009LL,
                      .render_offset_from_audio = 1,
                      .speed_cadence = { 1, 1 } }, state, *p = &state;
    INT64 target, now = initial.render_offset_ns + 22000000LL + 22189700LL;
    state = initial;
    assert(pcm_startup_anchor_target_l(p, s, 0, 22, 0, now, &target));
    assert(target - initial.render_offset_ns == 22189700LL);
    apply(p, s, target);
    assert(p->render_offset_ns == initial.render_offset_ns);
    for (int i = 0; i < 24; i++) {
        INT64 previous = p->render_offset_ns;
        for (int retry = 0; retry < 10; retry++) {
            step(p, s, 0);
            assert(p->render_offset_ns == previous);
        }
        step(p, s, 1);
        assert(p->render_offset_ns - previous <= 1000000);
        assert(p->render_offset_ns >= previous);
    }
    assert(!p->slew_active && !p->pcm_startup_slew);
    assert(p->render_offset_ns == target);
    state = initial;
    apply(p, s, initial.render_offset_ns - 22189700LL);
    for (int i = 0; i < 24; i++) {
        INT64 previous = p->render_offset_ns;
        step(p, s, 1);
        assert(previous - p->render_offset_ns <= 1000000);
        assert(p->render_offset_ns <= previous);
    }
    assert(p->render_offset_ns == initial.render_offset_ns - 22189700LL);
    state = initial;
    for (int mode = 1; mode <= 3; mode++)
        assert(!pcm_startup_anchor_target_l(p, s, mode, 22, 0, now, &target));
    assert(!pcm_startup_anchor_target_l(p, s, 0, 22, 1, now, &target));
    assert(!pcm_startup_anchor_target_l(p, NULL, 0, 22, 0, now, &target));
    assert(!pcm_startup_anchor_target_l(p, s, 0, -1, 0, now, &target));
    assert(pcm_startup_anchor_target_l(p, s, 0, 0, 0, now, &target));
    assert(!pcm_startup_anchor_target_l(p, s, 0, 22, 0, now + 200000000LL, &target));
    assert(!pcm_startup_anchor_target_l(p, s, 0, 22, 0, now - 200000000LL, &target));
#define REJECT_P(field, value) do { state = initial; p->field = value; \
    assert(!pcm_startup_anchor_target_l(p, s, 0, 22, 0, now, &target)); } while (0)
    REJECT_P(pause_armed, 1);
    REJECT_P(pcm_resume_pending, 1);
    REJECT_P(pending_seek_reanchor, 1);
    REJECT_P(venc_ref_time, 123);
    REJECT_P(sched_start_mono_ns, 123);
    REJECT_P(render_offset_ns, -1);
    REJECT_P(render_offset_from_audio, 0);
    REJECT_P(speed_cadence.valid, 0);
    REJECT_P(speed_cadence.epoch, 2);
    state = initial;
#define REJECT_S(field) do { s->field = 1; \
    assert(!pcm_startup_anchor_target_l(p, s, 0, 22, 0, now, &target)); \
    s->field = 0; } while (0)
    REJECT_S(paused);
    REJECT_S(seek_paused);
    REJECT_S(play_n_video_frames);
    REJECT_S(audio_start_pending);
    REJECT_S(audio_resume_pending);
    audio.valid = 0;
    assert(!pcm_startup_anchor_target_l(p, s, 0, 22, 0, now, &target));
    s->audio = NULL;
    assert(!pcm_startup_anchor_target_l(p, s, 0, 22, 0, now, &target));
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
