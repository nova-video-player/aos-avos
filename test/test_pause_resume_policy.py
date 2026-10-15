"""Current audio pause contracts, independent of Android/JNI scheduling."""
import unittest

from native_replay import extract, run_c, source

PREAMBLE = r'''
#define CONFIG_SFDEC
#include "global.h"
#include "stream.h"
#include "audio_interface.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DBG if (0)
#define DBG3 if (0)
#define serprintf printf
static int64_t now_ms = 10000;
int64_t atime64(void) { return now_ms; }
void stream_audiotrack_epoch_reset(STREAM *s) { s->at_speed_epoch_active = 0; }
'''


class PauseResumePolicyTest(unittest.TestCase):
    def test_manual_delay_epochs_and_checkpoint_running_time(self):
        audio, sync, stream, video = map(source, (
            'stream_audio.c', 'stream_sync.c', 'stream.c', 'stream_video.c'))
        start = stream.index('if( !s || s->atempo_commit_count <= 0')
        poll_guard = stream[start:stream.index('return;', start) + 7]
        start = video.index('int do_audio_preload =')
        preload = video[start:video.index(';', start) + 1]
        run_c(PREAMBLE + r'''
static int silence, mode;
static int _wait(STREAM *s,int ms) { (void)s; silence+=ms; return ms; }
static int passthrough(STREAM *s) { (void)s; return mode; }
''' + extract(sync, 'static void _stream_pcm_delay_memory_reset(') + r'''
static void manual_delay(STREAM *s) {
''' + extract(audio, 'if( s->put_time_mode && s->audio_sink )') + r'''
}
static void resume_timeout(STREAM *s) {
''' + extract(video, 'if( s->pause_timing_valid )') + r'''
}
static int poll_allowed(STREAM *s, int drained) {
''' + poll_guard.replace('return;', 'return 0;') + r'''
    return 1;
}
static int preload_allowed(STREAM *s,int preserve_output,int compressed_resume) {
    int stream_zero_fill=1;
''' + preload + r'''
    return do_audio_preload;
}
int main(void) {
    STREAM s={0}; STREAM_SINK_AUDIO sink={.get_passthrough=passthrough};
    AUDIO_PROPERTIES audio={.valid=1};
    s.audio=&audio; s.audio_sink=&sink; s.put_time_mode=1; s.av_delay=-200;
    manual_delay(&s); assert(silence==200);
    s.manual_audio_hold_pending_ms=40;
    s.at_speed_epoch_active=1; s.atempo_ledger_active=1; s.atempo_ledger_count=2;
    s.atempo_commit_head=STREAM_ATEMPO_COMMIT_MAX-1; s.atempo_commit_count=2;
    s.atempo_commit_q[s.atempo_commit_head]=(STREAM_ATEMPO_COMMIT){.boundary=1234,.wall_ms=8000,.speed=1.2f};
    s.atempo_commit_q[0]=(STREAM_ATEMPO_COMMIT){.boundary=5678,.wall_ms=9500,.speed=1.5f};
    for(int i=0;i<3;i++) {
        s.pause_started_ms=now_ms; s.pause_timing_valid=1; s.paused=1;
        int64_t waited=s.pause_started_ms-s.atempo_commit_q[0].wall_ms;
        _stream_pcm_delay_memory_reset(&s,1);
        assert(s.manual_audio_hold_pending_ms==40 && s.manual_audio_delay_applied_ms==200);
        assert(s.at_speed_epoch_active && s.atempo_ledger_count==2);
        assert(s.atempo_commit_count==2 && s.atempo_commit_q[0].boundary==5678);
        assert(!poll_allowed(&s,0));
        now_ms+=30000; resume_timeout(&s);
        assert(now_ms-s.atempo_commit_q[0].wall_ms==waited && !s.pause_timing_valid);
        s.paused=0; manual_delay(&s); assert(silence==200);
        assert(poll_allowed(&s,0)); now_ms+=100;
    }
    s.pause_started_ms=now_ms; s.pause_timing_valid=1;
    s.atempo_commit_q[0].wall_ms=now_ms+50; // Created during pause.
    now_ms+=500; resume_timeout(&s);
    assert(s.atempo_commit_q[0].wall_ms==now_ms);
    s.manual_audio_delay_applied_ms=80; s.manual_audio_hold_pending_ms=20;
    manual_delay(&s); assert(silence==320 && s.manual_audio_hold_pending_ms==140);
    s.av_delay=-300; manual_delay(&s); assert(silence==420);
    _stream_pcm_delay_memory_reset(&s,0);
    assert(!s.manual_audio_delay_applied_ms && !s.manual_audio_hold_pending_ms);
    assert(!s.at_speed_epoch_active && !s.atempo_ledger_count);
    assert(s.atempo_commit_count==1 && s.atempo_commit_q[0].speed==1.5f);
    assert(s.atempo_commit_q[0].boundary==STREAM_ATEMPO_COMMIT_BOUNDARY_DEFER);
    manual_delay(&s); assert(silence==720);
    mode=1; manual_delay(&s); assert(silence==720 && !s.manual_audio_delay_applied_ms);
    s.speed=STREAM_SPEED_NORMAL;
    assert(!preload_allowed(&s,1,0)); // Retained Java queue at every speed.
    assert(preload_allowed(&s,0,0)); // Legacy backend discarded its queue.
    assert(!preload_allowed(&s,0,1)); // Active or requested compressed mode.
    audio.valid=0; assert(!preload_allowed(&s,0,0));
    s.paused_internal=1; assert(!poll_allowed(&s,0)); assert(poll_allowed(&s,1));
    s.aborted=1; assert(!poll_allowed(&s,1));
    return 0;
}
''')

    def test_retained_wait_preload_failure_and_abort(self):
        audio, sync = source('stream_audio.c'), source('stream_sync.c')
        run_c(PREAMBLE + extract(sync, 'static void _stream_pcm_delay_memory_reset(') + r'''
static int mode, flushes, preloads, starts, fail_start, waits, abort_requested;
static int abort_in_wait, preserve=1, zero_time;
static STREAM *current;
int stream_audio_paused;
static int passthrough(STREAM *s) { (void)s; return mode; }
static int syncable(STREAM *s) { (void)s; return 1; }
static int flush(STREAM *s) { (void)s; ++flushes; return 0; }
static int preload(STREAM *s) { (void)s; assert(flushes>preloads); ++preloads; return 0; }
static int start(STREAM *s) { (void)s; assert(preloads>starts); ++starts; return fail_start; }
void stream_sync_pcm_output_reset(STREAM *s) { _stream_pcm_delay_memory_reset(s,0); }
void stream_audio_sink_failed(STREAM *s,const char *why) { (void)why; s->video_error=1; }
static void _write_zero_data(STREAM *s,int ms) { (void)s; (void)ms; assert(0); }
static int _abort(STREAM *s) { (void)s; return abort_requested; }
void stream_yield_RT(void) {
    assert(++waits<5);
    if(abort_in_wait) { abort_requested=1; return; }
    current->paused=stream_audio_paused=current->audio_pause_requested=0;
    current->audio_preload=!preserve;
}
''' + extract(audio, 'static void _stream_audio_apply_preload(') + '\n' + extract(
            audio, 'void stream_audio_prepare_resume(') + '\n' + extract(
            audio, 'static int _stream_audio_wait_for_resume(') + r'''
int main(void) {
    STREAM_SINK_AUDIO sink={.get_passthrough=passthrough,.syncable=syncable,
        .flush=flush,.preload=preload,.start=start};
    STREAM s={.audio_sink=&sink,.pcm_accum_size=37,.paused=1}; current=&s;
    assert(!_stream_audio_wait_for_resume(&s));
    assert(s.pcm_accum_size==37 && !flushes && !starts);
    assert(s.audio_resume_pending && s.video_hold_for_delay && s.video_hold_for_resume_audio);
    s.seek_paused=1; s.video_hold_for_delay=s.video_hold_for_resume_audio=0;
    s.paused=1; waits=0; assert(!_stream_audio_wait_for_resume(&s));
    assert(!s.video_hold_for_delay && !s.video_hold_for_resume_audio);
    s.seek_paused=0; preserve=0; waits=0; s.paused=1;
    s.manual_audio_delay_applied_ms=200; s.manual_audio_hold_pending_ms=40;
    assert(!_stream_audio_wait_for_resume(&s));
    assert(flushes==1 && preloads==1 && starts==1 && !s.audio_preload);
    assert(!s.manual_audio_delay_applied_ms && !s.manual_audio_hold_pending_ms);
    assert(!_stream_audio_wait_for_resume(&s) && starts==1);
    s.audio_preload=1; fail_start=1;
    assert(_stream_audio_wait_for_resume(&s) && s.video_error);
    s.video_error=0; s.audio_preload=1; mode=2;
    _stream_audio_apply_preload(&s); assert(flushes==2 && starts==2);
    s.paused=1; abort_in_wait=1; waits=0;
    assert(_stream_audio_wait_for_resume(&s)); assert(starts==2);
    return 0;
}
''')

    def test_pause_interrupted_zero_write_classification(self):
        track = source('audio_interface_audiotrack_java.c')
        guard = extract(track, 'if (ret == 0 && (write_started_paused')
        run_c(r'''
#include <assert.h>
#define AUDIO_WRITE_RETRY -99
#define DBG if (0)
#define LOG(...) ((void)0)
struct track { int track_paused; unsigned pause_generation; };
static int classify(struct track *at,int ret,int write_started_paused,unsigned write_pause_generation) {
''' + guard + r'''
    return ret;
}
int main(void) {
    struct track at={.pause_generation=2};
    assert(classify(&at,0,0,1)==AUDIO_WRITE_RETRY);
    assert(classify(&at,0,1,2)==AUDIO_WRITE_RETRY);
    assert(classify(&at,0,0,2)==0);
    at.track_paused=1; assert(classify(&at,0,0,2)==AUDIO_WRITE_RETRY);
    assert(classify(&at,16,0,1)==16); // Accepted prefix must not be retried.
    assert(classify(&at,-6,0,1)==-6); // Real failure isn't a pause retry.
    return 0;
}
''')

    def test_writer_retains_unaccepted_pcm_through_pause(self):
        audio = source('stream_audio.c')
        start = audio.index('DBG3 serprintf("stream_audio: checking if sink can_write')
        capacity = audio[start:audio.index('if( use_atempo &&', start)]
        start = audio.index('size             -= size_written;')
        advance = audio[start:audio.index('audio_frame.data += size_written;', start) +
                        len('audio_frame.data += size_written;')]
        run_c(r'''
#define CONFIG_SFDEC
#include "global.h"
#include "stream.h"
#include "audio_interface.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DBG if (0)
#define DBG3 if (0)
#define serprintf printf
static int scenario, capacity_calls, write_calls, accepted, waits, aborted;
static STREAM *current;
int stream_audio_paused;
static int _abort(STREAM *s) { (void)s; return aborted; }
static void _stream_audio_apply_preload(STREAM *s) { assert(!s->audio_preload); }
void stream_yield_RT(void) {
    assert(++waits<10);
    if(scenario==4) { aborted=1; return; }
    current->paused=0;
}
static int can_write(STREAM *s,int size) {
    (void)size;
    if((scenario==1 || scenario==4) && ++capacity_calls==1) { s->paused=1; return 0; }
    return 1;
}
static int write_output(STREAM *s,AUDIO_FRAME *frame) {
    assert(!s->paused && frame->size==8-accepted);
    assert(!memcmp(frame->data,&"retained"[accepted],frame->size));
    ++write_calls;
    if(scenario==2 && write_calls==1) { s->paused=1; return AUDIO_WRITE_RETRY; }
    int count=frame->size;
    if(scenario==3 && write_calls==1) { count=2; s->paused=1; }
    accepted+=count; return count;
}
''' + extract(audio, 'void stream_audio_prepare_resume(') + '\n' + extract(
            audio, 'static int _stream_audio_wait_for_resume(') + r'''
static void write_retained(STREAM *s) {
    AUDIO_FRAME audio_frame={.data=(unsigned char *)"retained"};
    int size=8, compressed_unit=0;
    while(size>0) {
        if(_stream_audio_wait_for_resume(s)) return;
        audio_frame.size=size;
''' + capacity + r'''
        int size_written=s->audio_sink->write(s,&audio_frame);
''' + extract(audio, 'if( !compressed_unit && size_written == AUDIO_WRITE_RETRY )') + advance + r'''
    }
}
int main(void) {
    STREAM_SINK_AUDIO sink={.can_write=can_write,.write=write_output};
    for(scenario=1;scenario<=4;scenario++) {
        STREAM s={.audio_sink=&sink,.pcm_accum_size=37}; current=&s;
        capacity_calls=write_calls=accepted=waits=aborted=0;
        write_retained(&s);
        assert(s.pcm_accum_size==37);
        if(scenario==4) assert(aborted && !accepted);
        else assert(accepted==8);
        if(scenario==2 || scenario==3) assert(write_calls==2);
    }
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
