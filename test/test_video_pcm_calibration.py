"""Exercise post-resume calibration tracking and lifecycle guards from production C."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_video_pcm_startup import block


class PcmCalibrationTest(unittest.TestCase):
    def test_delayed_calibration_and_lifecycle(self):
        source = (Path(__file__).resolve().parents[1] / 'Source/codec_sfdec2.c').read_text()
        matches = block(source, 'static int pcm_resume_matches_audio_l(')
        observe = block(source, 'static void pcm_resume_calibration_observe_l(')
        write_target = block(source, 'static int pcm_resume_write_target_l(')
        eligibility = source[source.index('if( p->pcm_resume_calibration_active &&'): ]
        eligibility = eligibility[:eligibility.index(';') + 1]
        def statement(marker):
            start = source.index(marker)
            return source[start:source.index(';', start) + 1]
        sampling_guard = statement('int pcm_write_clock =') + statement('int sample_pcm_resume =')
        sampling_guard += statement('int preserve_pcm_resume_anchor =')
        sampling_reset = statement('if( p->pcm_resume_sample_stage &&')
        slew = block(source, 'if (p->slew_active &&')
        remember = block(source, 'if( (p->pcm_startup_slew || p->pcm_resume_calibration_active ||')
        guard_start = source.index('int pcm_startup_new_slew_frame =')
        guard = source[guard_start:source.index(';', guard_start) + 1]
        fields = set(re.findall(r'p->(\w+)', matches + observe + write_target + slew + remember + guard + eligibility + sampling_guard + sampling_reset))
        fields.remove('pcm_resume_timeline')
        private = '\n'.join(
            f'{"const void *" if name.endswith("handle") else "int64_t "}{name};'
            for name in sorted(fields))
        code = r'''
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define DBGSI if (0)
#define serprintf(...) ((void)0)
#define pthread_mutex_lock(m) ((void)(m))
#define pthread_mutex_unlock(m) ((void)(m))
typedef int64_t INT64;
typedef struct { double speed, rst_anchor, ts_anchor; } timeline_state_t;
static timeline_state_t timeline = {1, 0, 0};
static timeline_state_t timeline_snapshot(void) { return timeline; }
typedef struct { int valid; } AUDIO;
typedef struct {
    AUDIO *audio;
    int audio_time, seek_paused, audio_start_pending, seek_epoch, av_delay;
    int audio_lifecycle_mutex, audio_reconfiguring, audio_lifecycle_generation, paused;
    int at_speed_epoch_active;
} STREAM;
static int speed_enabled;
static int audio_interface_is_audio_speed_enabled(void) { return speed_enabled; }
typedef struct {
    timeline_state_t pcm_resume_timeline;
''' + private + r'''
} priv_t;
''' + matches + observe + write_target + r'''
static int can_sample(priv_t *p, STREAM *s, int passthrough_mode, int preserve_output,
                      int pcm_latency_us, float current_speed) {
    int epoch_changed = 0, speed_changed = 0, no_sched_anchor = 0;
''' + sampling_guard + r'''
    return sample_pcm_resume && preserve_pcm_resume_anchor;
}
static void validate_sample(priv_t *p, STREAM *s, int pcm_resume, int sample_pcm_resume,
                            int speed_changed, int epoch_changed) {
''' + sampling_reset + r'''
}
static void validate(priv_t *p, STREAM *s, int passthrough_mode, int speed_changed,
                     int epoch_changed, int pcm_resume) {
''' + eligibility + r'''
}
static void step(priv_t *p, STREAM *s, int frame_time) {
    struct { int duration, time, epoch; const void *android_handle; }
        frame = {41, frame_time, 11, (void *)1}, *f = &frame;
    int mode2_new_slew_frame = 0, mode2_dynamic_active = 0;
''' + guard + slew + remember + r'''
}
int main(void) {
    AUDIO audio = {1};
    STREAM stream = {.audio = &audio, .audio_time = 610813,
                     .seek_epoch = 11, .audio_lifecycle_generation = 7}, *s = &stream;
    priv_t initial = {.render_offset_ns = 270114435348188LL,
                      .target_offset_ns = 270114435348188LL,
                      .render_offset_from_audio = 1,
                      .pcm_resume_calibration_active = 1,
                      .pcm_resume_calibration_us = 37690,
                      .pcm_resume_epoch = 11,
                      .pcm_resume_audio_generation = 7,
                      .pcm_resume_timeline = {1, 0, 0}}, state = initial, *p = &state;
    // The first resume slew has completed; tracking still belongs to this clock.
    assert(pcm_resume_matches_audio_l(p, s));
    pcm_resume_calibration_observe_l(p, 40000);
    pcm_resume_calibration_observe_l(p, 41000);
    assert(p->target_offset_ns == initial.target_offset_ns);
    pcm_resume_calibration_observe_l(p, 53080);
    assert(p->target_offset_ns == initial.target_offset_ns + 15390000);
    for (int i = 1; i <= 4; i++) {
        INT64 before = p->render_offset_ns;
        step(p, s, i);
        assert(p->render_offset_ns - before <= 4000000);
        before = p->render_offset_ns;
        step(p, s, i); // renderer lookahead retry cannot spend a second step
        assert(p->render_offset_ns == before);
    }
    assert(!p->slew_active && p->pcm_resume_calibration_active);
    // Later change, even after completion. Retargeting the same peeked frame
    // cannot reset its consumed allowance.
    pcm_resume_calibration_observe_l(p, 65000);
    INT64 before = p->render_offset_ns;
    step(p, s, 4);
    assert(p->render_offset_ns == before);
    for (int i = 5; i <= 8; i++) step(p, s, i);
    assert(p->render_offset_ns == initial.target_offset_ns + 27310000);
    pcm_resume_calibration_observe_l(p, 65000);
    assert(!p->slew_active); // no duplicate correction
    pcm_resume_calibration_observe_l(p, 37690);
    for (int i = 9; i < 20; i++) {
        before = p->render_offset_ns;
        step(p, s, i);
        assert(before - p->render_offset_ns <= 4000000);
    }
    assert(p->render_offset_ns == initial.render_offset_ns);
    // Adjust an unfinished target without losing its original residual.
    state = initial;
    p->slew_active = p->pcm_startup_slew = p->pcm_resume_slew = 1;
    p->target_offset_ns -= 18000000;
    pcm_resume_calibration_observe_l(p, 60330);
    assert(p->target_offset_ns == initial.target_offset_ns + 4640000);
    // Unavailable calibration, an excessive change, or another reanchor must
    // not convert a source switch into a supposed latency correction.
    int rejected[] = {-1, 400000};
    for (int i = 0; i < 2; i++) {
        state = initial;
        pcm_resume_calibration_observe_l(p, rejected[i]);
        assert(!p->pcm_resume_calibration_active);
        assert(p->target_offset_ns == initial.target_offset_ns);
    }
    state = initial; p->pending_reanchor = 1;
    pcm_resume_calibration_observe_l(p, 65000);
    assert(!p->pcm_resume_calibration_active);
    state = initial; p->pcm_resume_calibration_active = 0;
    pcm_resume_calibration_observe_l(p, 65000);
    assert(!p->slew_active && p->target_offset_ns == initial.target_offset_ns);
    for (int mode = 1; mode <= 3; mode++) {
        state = initial;
        validate(p, s, mode, 0, 0, 0);
        pcm_resume_calibration_observe_l(p, 65000);
        assert(!p->pcm_resume_calibration_active);
        assert(p->target_offset_ns == initial.target_offset_ns);
    }
    state = initial; validate(p, s, 0, 1, 0, 0);
    assert(!p->pcm_resume_calibration_active);
    state = initial; validate(p, s, 0, 0, 1, 0);
    assert(!p->pcm_resume_calibration_active);
    state = initial; validate(p, s, 0, 0, 0, 1);
    assert(!p->pcm_resume_calibration_active);
    state = initial; s->paused = 1; validate(p, s, 0, 0, 0, 0);
    assert(!p->pcm_resume_calibration_active); s->paused = 0;
    state = initial; validate(p, NULL, 0, 0, 0, 0);
    assert(!p->pcm_resume_calibration_active);
    state = initial;
#define REJECT_S(field, value) do { int old = s->field; s->field = value; \
    assert(!pcm_resume_matches_audio_l(p, s)); s->field = old; } while (0)
    REJECT_S(seek_epoch, 12);
    REJECT_S(av_delay, -100);
    REJECT_S(audio_lifecycle_generation, 8);
    REJECT_S(audio_reconfiguring, 1);
    REJECT_S(seek_paused, 1);
    REJECT_S(audio_start_pending, 1);
    REJECT_S(audio_time, -1);
    timeline.speed = 1.6;
    assert(!pcm_resume_matches_audio_l(p, s));
    timeline.speed = 1; timeline.ts_anchor = 100;
    assert(!pcm_resume_matches_audio_l(p, s));
    assert(!pcm_resume_matches_audio_l(p, NULL));

    // Legacy Sony trace: the first resumed clock is followed by only 32ms
    // of media progress over 174ms. Never keep that early sample as the target.
    timeline = initial.pcm_resume_timeline;
    state = initial; p->pcm_resume_calibration_active = 0;
    INT64 origin = 31454422503499LL, target = -1;
    INT64 wall = origin + 16145000000LL;
    assert(!pcm_resume_write_target_l(p, 16145, wall, &target));
    assert(pcm_resume_matches_audio_l(p, s));
    assert(!pcm_resume_write_target_l(p, 16145, wall + 22000000, &target));
    assert(!pcm_resume_write_target_l(p, 16177, wall + 174000000, &target));
    assert(!pcm_resume_write_target_l(p, 16209, wall + 178000000, &target));
    assert(!pcm_resume_write_target_l(p, 16241, wall + 189000000, &target));
    // Once refill has passed, writes run at 1x with 90-110ms phase relative
    // to that first sample. Duplicated put_time calls cannot bias the minimum.
    int ready = 0;
    for (int media = 128; media <= 768 && !ready; media += 32) {
        INT64 now = wall + (media + 90 + (media % 64 ? 20 : 0)) * 1000000LL;
        ready = pcm_resume_write_target_l(p, 16145 + media, now, &target);
        if (!ready) {
            assert(!pcm_resume_write_target_l(p, 16145 + media, now + 1000000, &target));
            assert(target == -1);
        }
    }
    assert(ready == 1 && target == origin + 90000000);
    assert(!p->pcm_resume_sample_stage);
    // Apply the qualified target through the existing bounded correction.
    p->render_offset_ns = origin;
    p->target_offset_ns = target;
    p->slew_active = p->pcm_startup_slew = p->pcm_resume_slew = 1;
    for (int frame = 30; frame < 55; frame++) {
        INT64 previous = p->render_offset_ns;
        step(p, s, frame);
        assert(p->render_offset_ns - previous <= 4000000);
        previous = p->render_offset_ns;
        step(p, s, frame);
        assert(p->render_offset_ns == previous);
    }
    assert(p->render_offset_ns == target && !p->slew_active);

    // A write burst without elapsed wall time cannot qualify. Nor can time
    // passing without writes. Failure preserves the caller's current target.
    state = initial; target = -1;
    assert(!pcm_resume_write_target_l(p, 1000, wall, &target));
    assert(!pcm_resume_write_target_l(p, 1500, wall + 1000000, &target));
    assert(!pcm_resume_write_target_l(p, 1500, wall + 1000000000, &target));
    assert(pcm_resume_write_target_l(p, 1500, wall + 2001000000, &target) == -1);
    assert(target == -1 && !p->pcm_resume_sample_stage);
    assert(!pcm_resume_write_target_l(p, 1500, wall, &target));
    assert(pcm_resume_write_target_l(p, 1499, wall + 1000000, &target) == -1);
    assert(target == -1 && !p->pcm_resume_sample_stage);
    // Pausing discards a partially collected window; the next resume needs
    // fresh wall/media progress rather than counting the paused interval.
    assert(!pcm_resume_write_target_l(p, 1500, wall, &target));
    p->pcm_resume_sample_stage = 0;
    assert(!pcm_resume_write_target_l(p, 1800, wall + 5000000000LL, &target));
    assert(p->pcm_resume_sample_stage == 1 && target == -1);
    state = initial; p->pcm_resume_calibration_active = 0;
    p->pcm_resume_sample_stage = 1;
    REJECT_S(seek_epoch, 12);
    REJECT_S(av_delay, -100);
    REJECT_S(audio_lifecycle_generation, 8);
    timeline.speed = 1.6;
    assert(!pcm_resume_matches_audio_l(p, s));
    timeline = initial.pcm_resume_timeline;
    state = initial; p->pcm_resume_pending = 1;
    assert(can_sample(p, s, 0, 1, -1, 1.0));
    for (int mode = 1; mode <= 3; mode++) assert(!can_sample(p, s, mode, 1, -1, 1.0));
    assert(!can_sample(p, s, 0, 0, -1, 1.0)); // backend discards paused output
    assert(!can_sample(p, s, 0, 1, 37000, 1.0)); // calibrated playhead
    assert(!can_sample(p, s, 0, 1, -1, 1.2)); // committed speed mapping
    speed_enabled = 1;
    assert(!can_sample(p, s, 0, 1, -1, 1.0)); // atempo/Sonic/PlaybackParams
    speed_enabled = 0; s->at_speed_epoch_active = 1;
    assert(!can_sample(p, s, 0, 1, -1, 1.0)); // preserved PlaybackParams checkpoint
    s->at_speed_epoch_active = 0; s->seek_paused = 1;
    assert(!can_sample(p, s, 0, 1, -1, 1.0));
    s->seek_paused = 0;
    // An observation must never survive a change of clock or output identity.
    p->pcm_resume_sample_stage = 2;
    validate_sample(p, s, 1, 1, 0, 0);
    assert(p->pcm_resume_sample_stage == 2);
    validate_sample(p, s, 1, 1, 1, 0);
    assert(!p->pcm_resume_sample_stage);
    p->pcm_resume_sample_stage = 2;
    validate_sample(p, s, 1, 1, 0, 1);
    assert(!p->pcm_resume_sample_stage);
    p->pcm_resume_sample_stage = 2; s->av_delay = -100;
    validate_sample(p, s, 1, 1, 0, 0);
    assert(!p->pcm_resume_sample_stage); s->av_delay = 0;
    p->pcm_resume_sample_stage = 2; s->audio_lifecycle_generation++;
    validate_sample(p, s, 1, 1, 0, 0);
    assert(!p->pcm_resume_sample_stage); s->audio_lifecycle_generation--;
    p->pcm_resume_sample_stage = 2;
    validate_sample(p, s, 1, 0, 0, 0);
    assert(!p->pcm_resume_sample_stage);
    p->pcm_resume_sample_stage = 2;
    validate_sample(p, s, 0, 1, 0, 0);
    assert(!p->pcm_resume_sample_stage);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'calibration.c').write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(root / 'calibration.c'), '-o', str(root / 'calibration')], check=True)
            subprocess.run([str(root / 'calibration')], check=True)


if __name__ == '__main__':
    unittest.main()
