"""Replay the slow-operation timer with deterministic wall/CPU clocks."""
import unittest
from native_replay import run_c, source


class SlowCallDiagTest(unittest.TestCase):
    def test_threshold_errno_and_debug_gates(self):
        for enabled in (0, 1):
            with self.subTest(debug_logs=enabled):
                run_c(r'''
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <stdarg.h>
#include <assert.h>
#define _DEBUG_H_
#define DEBUG_MSG
#define CONFIG_ANDROID
#define DEBUG_LOGS ''' + str(enabled) + r'''
#define DBG_SINK 0
static int Debug[1];
static int clocks, logs;
static long long logged_wall, logged_cpu, logged_result, logged_bytes;
static int logged_audio, logged_video;
static int64_t wall=1000000000, cpu=200000000;
static int fake_clock(clockid_t id, struct timespec *t) {
    clocks++; int64_t ns=id==CLOCK_MONOTONIC?wall:cpu;
    t->tv_sec=ns/1000000000; t->tv_nsec=ns%1000000000; errno=EAGAIN; return 0;
}
static long fake_syscall(int id) { (void)id; return 123; }
static int serprintf_record(const char *fmt,...) {
    (void)fmt; va_list ap; va_start(ap,fmt);
    (void)va_arg(ap,const char *); assert(va_arg(ap,long)==123);
    long long start=va_arg(ap,long long), end=va_arg(ap,long long);
    logged_wall=va_arg(ap,long long); logged_cpu=va_arg(ap,long long);
    assert(end-start==logged_wall*1000);
    logged_result=va_arg(ap,long long); logged_bytes=va_arg(ap,long long);
    logged_audio=va_arg(ap,int); logged_video=va_arg(ap,int);
    va_end(ap); logs++; errno=ERANGE; return 0;
}
#undef SYS_gettid
#define SYS_gettid 0
#define clock_gettime fake_clock
#define syscall fake_syscall
#include "slow_call_diag.h"
int main(void) {
    // Keep stubs referenced even when the entire diagnostic compiles away.
    (void)fake_clock; (void)fake_syscall; (void)serprintf_record;
    Debug[0]=0; errno=EINTR;
    slow_call_diag_t d=slow_call_diag_begin(SLOW_CALL_DIAG_ENABLED);
    slow_call_diag_end(d,"disabled",0,0,-1,-1);
    assert(!clocks && !logs && errno==EINTR);
    Debug[0]=1; d=slow_call_diag_begin(SLOW_CALL_DIAG_ENABLED);
    assert(errno==EINTR);
    wall+=99999999; cpu+=1000000;
    slow_call_diag_end(d,"short",1,100,4,5);
    assert(!logs && errno==EINTR);
    wall++;
    slow_call_diag_end(d,"slow",-1,100,4,5);
    assert(logs==DEBUG_LOGS && errno==EINTR);
    if(DEBUG_LOGS) {
        assert(logged_wall==100000 && logged_cpu==1000);
        assert(logged_result==-1 && logged_bytes==100);
        assert(logged_audio==4 && logged_video==5);
    }
    if (!DEBUG_LOGS) assert(!clocks);
    d=slow_call_diag_begin(0); wall+=1000000000;
    slow_call_diag_end(d,"explicitly_disabled",0,0,-1,-1);
    assert(logs==DEBUG_LOGS);
    return 0;
}
''')

    def test_probes_do_not_replace_calls_or_read_clocks_from_new_sources(self):
        audio = source('audio_interface_audiotrack_java.c')
        parser = source('stream_parser_ffmpeg.c')
        video = source('stream_video.c')
        self.assertIn('slow_call_diag_end(write_diag, "audiotrack_write", ret, len_to_write', audio)
        self.assertIn('slow_call_diag_end(timestamp_diag, "getTimestamp", success', audio)
        self.assertIn('slow_call_diag_end(demux_diag, "av_read_frame", result', parser)
        self.assertIn('slow_call_diag_end(read_diag, "fd_pread", ret, bytes', parser)
        self.assertIn('slow_call_diag_end(sync_wait_diag, "video_audio_sync_wait", sync_wait_ms', video)


if __name__ == '__main__':
    unittest.main()
