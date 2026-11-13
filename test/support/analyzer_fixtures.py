"""Synthetic logcat builders shared by the analyzer and campaign tests."""


def fixture(mode='resume', seconds=3, speed=1, start=1000, sequence=False):
    rows = []
    marker = 'WALLCLOCK_RESET: by pause resume' if mode == 'resume' else 'VIDEO_SEEK_TARGET_READY: epoch=1'
    if mode != 'speed':
        rows.append(f'{start:.3f} {marker}')
    for i in range(seconds * 25 + 1):
        t = start + .01 + i * .04
        ns = round(t * 1e9)
        frame = i * 40
        rows += [
            f'{t:.3f} put_time_calc: speed={speed:.3f} allow_reanchor=0 disc=0',
            f'{t:.3f} audiotrack_write: wrote 7680 out of 7680 bytes',
            f'{t:.3f} audio_present_diag: presented={i*1920} source=playhead age_ms=0 sample_ns={ns}',
            f'{t:.3f} video_sched_diag: frame={frame} audio={frame} frame_minus_heard=0 seek_epoch=1',
            f'{t:.3f} video_render_diag: frame={frame} epoch=1 deadline_ns={ns+100000000} submit_ns={ns} interval_ms=40 anchor_age_ms=10 phase_ms=0' +
            (f' render_seq={i+1}' if sequence else ''),
        ]
    rows.append(f'{start+seconds+.02:.3f} AVOS_TEST_POLL_1')
    return '\n'.join(rows)
