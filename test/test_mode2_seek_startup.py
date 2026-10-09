"""Exercise production Mode 2 seek guards without Android or a decoder."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class Mode2SeekStartupTest(unittest.TestCase):
    def test_preview_publication_and_refill(self):
        root = Path(__file__).resolve().parents[1]
        codec = (root / 'Source/codec_sfdec2.c').read_text()
        sync = (root / 'Source/stream_sync.c').read_text()
        preview = block(codec, 'if (passthrough == 2 && !libavos_get_ac3_recoding_enabled()')
        publish = block(codec, 'if( s && time >= 0 && !s->paused && !s->seek_paused &&')
        catchup = block(sync, 'if( raw_heard_ts > s->mode2_heard_interp_ts &&')
        fields = sorted(set(re.findall(r'p->(\w+)', publish)))
        code = r'''
#include <stdint.h>
#include <assert.h>
typedef int64_t INT64;
#define STREAM_NO_PTS_VALUE INT32_MIN
#define DBGSI if (0)
#define serprintf_record(...) ((void)0)
typedef struct {
    int paused, seek_paused, play_n_video_frames, seek_video_drop;
    int seek_video_target_ts, seek_video_target_pending, seek_video_ready_ts, seek_epoch;
    int mode2_heard_interp_ts;
    int64_t mode2_seek_refill_until_ms;
} Stream;
typedef struct { int time, epoch; } Frame;
typedef struct { int venc_q; } Queue;
typedef struct { Queue locked;
''' + ''.join('int64_t ' + name + ';\n' for name in fields) + r'''
} Priv;
static Frame frame;
static int recoding;
static int libavos_get_ac3_recoding_enabled(void) { return recoding; }
static Frame *frame_q_get(int *q) { ++*q; return &frame; }
static int discard(Stream *s, Priv *p, int passthrough, int time) {
    Frame *f = &frame;
    f->time = time; f->epoch = s->seek_epoch;
    int consumed = 0, stale_epoch_drop = 0;
    do {
''' + preview + r'''
    } while (0);
    assert(consumed == stale_epoch_drop);
    return consumed;
}
static void anchor(Stream *s, Priv *p, int passthrough_mode, int time) {
    int64_t published_clock_ns = 10000000000LL;
''' + publish + r'''
}
static void refill(Stream *s, int raw_heard_ts, int64_t wall_now) {
''' + catchup + r'''
}
int main(void) {
    Stream s = {.seek_video_drop=1, .seek_video_target_ts=135550,
        .seek_video_ready_ts=135552, .seek_epoch=2};
    Priv p = {.pending_seek_reanchor=1, .render_offset_ns=-1};
    assert(discard(&s, &p, 2, 133091));
    assert(p.pending_seek_reanchor && p.render_offset_ns == -1);
    assert(!discard(&s, &p, 2, 135552));
    assert(!discard(&s, &p, 0, 133091));
    assert(!discard(&s, &p, 1, 133091));
    recoding=1;
    assert(!discard(&s, &p, 2, 133091));
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == -1);
    recoding=0;
    s.seek_paused=1;
    assert(!discard(&s, &p, 2, 133091));
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == -1);
    s.seek_paused=0; s.play_n_video_frames=1;
    assert(!discard(&s, &p, 2, 133091));
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == -1);
    s.play_n_video_frames=0; s.seek_video_target_pending=1;
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == -1);
    s.seek_video_target_pending=0;
    s.seek_video_ready_ts=STREAM_NO_PTS_VALUE;
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == -1);
    s.seek_video_ready_ts=135552;
    anchor(&s, &p, 2, -1);
    assert(p.render_offset_ns == -1);
    anchor(&s, &p, 0, 135353);
    assert(p.render_offset_ns == -1);
    anchor(&s, &p, 2, 135353);
    assert(p.render_offset_ns == 10000000000LL - 135353000000LL);
    assert(!p.pending_seek_reanchor && p.render_offset_from_audio);
    int64_t offset = p.render_offset_ns;
    anchor(&s, &p, 2, 135600); // refill must not publish a second anchor
    assert(p.render_offset_ns == offset);
    s.mode2_heard_interp_ts=1000; s.mode2_seek_refill_until_ms=1609;
    refill(&s, 1209, 1050); // accepted burst is not a presentation jump
    assert(s.mode2_heard_interp_ts == 1000);
    refill(&s, 1400, 1610); // bounded fallback still catches up
    assert(s.mode2_heard_interp_ts == 1400);
    s.mode2_seek_refill_until_ms=0;
    refill(&s, 1800, 1611); // existing track-recreation policy
    assert(s.mode2_heard_interp_ts == 1800);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'seek.c'
            binary = Path(directory) / 'seek'
            source.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
