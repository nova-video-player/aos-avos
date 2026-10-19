"""Replay the real compressed writer with diagnostics enabled and disabled."""
import re
import unittest

from native_replay import extract, run_c, source
from analyze_stress import analyze, config, compressed_wait, records, write_gap_evidence
from test_analyze_stress import fixture


class CompressedWaitTests(unittest.TestCase):
    def test_direct_wait_matches_buffered_write_thread(self):
        # Bravia capture: writes are emitted by log_thread (31559), while the
        # recovered-wait record is emitted directly by the audio thread (31792).
        before = ('1792417364.325 31506 31559 D avos_player: '
                  'audiotrack_write: wrote 1024 out of 2560 bytes')
        after = ('1792417364.634 31506 31559 D avos_player: '
                 'audiotrack_write: wrote 1536 out of 1536 bytes')
        wait = ('1792417364.634 31506 31792 D avos_player: compressed_write_wait: '
                'epoch=3 fmt=4747 last_progress_ms=21048137 start_ms=21048137 '
                'end_ms=21048463 blocked_ms=326 retries=155 interrupted=0 '
                'accepted=1024 unit_bytes=2560 written=1536 complete=1')

        def evidence(diagnostic, boundary=None):
            rows = records('\n'.join([before, diagnostic, after] + ([boundary] if boundary else [])))
            writes = [r for r in rows if 'audiotrack_write:' in r[1]]
            return write_gap_evidence(rows, writes, writes[-1][0], 250, 3)

        self.assertEqual(evidence(wait), dict(backpressure_write_gaps=1,
                         incomplete_backpressure_write_gaps=0, unexplained_write_gaps=0))
        for other in (wait.replace('31506 31792', '31507 31792'),
                      wait.replace('31506 31792 D avos_player: ', ''),
                      wait.replace('epoch=3', 'epoch=4')):
            self.assertEqual(evidence(other)['unexplained_write_gaps'], 1)
        pause = '1792417364.400 31506 31559 D avos_player: android_sync: pause start'
        self.assertEqual(evidence(wait, pause)['unexplained_write_gaps'], 1)
        partial = wait.replace('written=1536 complete=1', 'written=1536 complete=0').replace(
            'unit_bytes=2560', 'unit_bytes=4096')
        self.assertEqual(evidence(partial)['incomplete_backpressure_write_gaps'], 1)

    def test_debug_gate_does_not_change_writes(self):
        audio = source('stream_audio.c')
        enum = re.search(r'typedef enum \{\s*COMPRESSED_WRITE_COMPLETE.*?compressed_write_result_t;',
                         audio, re.S).group()
        for compiled in (0, 1):
            run_c(r'''
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#define DBG if(0)
''' + f'#define DBG2 if({compiled} && logs_on)\n' + r'''
#define COMPRESSED_WRITE_STALL_MS 3000
#define AUDIO_WRITE_RETRY -2
static int logs_on, stream_audio_paused, emitted, calls, retry_first;
static int64_t now, ready_at;
static char record[1024];
typedef struct { unsigned char *data; int size, format; } AUDIO_FRAME;
typedef struct STREAM STREAM;
typedef struct { int (*can_write)(STREAM *,int); int (*write)(STREAM *,AUDIO_FRAME *); } SINK;
struct STREAM { int audio_pause_requested, paused, play_n_audio_frames, seek_epoch; SINK *audio_sink; };
static int64_t atime64(void) { return now; }
static void stream_yield_RT(void) { now+=10; }
static int _abort(STREAM *s) { (void)s; return 0; }
static void serprintf(const char *fmt,...) { (void)fmt; }
static void serprintf_record(const char *fmt,...) {
    va_list args; va_start(args,fmt); vsnprintf(record,sizeof(record),fmt,args); va_end(args); emitted++;
}
static int can_write(STREAM *s,int size) { (void)s; (void)size; return 1; }
static int write_data(STREAM *s,AUDIO_FRAME *f) {
    (void)s; calls++;
    if(calls==1) return 1024;
    if(now<ready_at) return calls==2 && retry_first ? AUDIO_WRITE_RETRY : 0;
    return f->size;
}
''' + enum + '\n' + extract(audio, 'static compressed_write_result_t _stream_write_compressed_unit(') + r'''
int main(void) {
    SINK sink={can_write,write_data}; STREAM s={.seek_epoch=3,.audio_sink=&sink};
    unsigned char data[2560]; AUDIO_FRAME frame={data,sizeof(data),0x4747};
    for(int enabled=0;enabled<2;enabled++) {
        for(int interrupted=0;interrupted<2;interrupted++) {
            now=1000; ready_at=1330; calls=emitted=0; logs_on=enabled; retry_first=interrupted;
            int accepted=0;
            assert(_stream_write_compressed_unit(&s,&frame,&accepted)==COMPRESSED_WRITE_COMPLETE);
            assert(accepted==2560 && calls==35 && now==1330);
''' + f'            assert(emitted==({compiled} && enabled));\n' + r'''
            if(emitted) {
                assert(strstr(record,"blocked_ms=330 retries=33"));
                assert(strstr(record,"accepted=1024 unit_bytes=2560 written=1536 complete=1"));
                assert(strstr(record,interrupted?"interrupted=1":"interrupted=0"));
            }
        }
    }
    now=1000; ready_at=1020; calls=emitted=0; logs_on=1;
    int accepted=0;
    assert(_stream_write_compressed_unit(&s,&frame,&accepted)==COMPRESSED_WRITE_COMPLETE);
    assert(!emitted && accepted==2560);
    return 0;
}
''')

    def test_backpressure_annotation_does_not_hide_faults(self):
        text = '\n'.join(line for line in fixture('seek').splitlines()
                         if not ('audiotrack_write:' in line and 1000.01 < float(line.split()[0]) < 1000.37))
        text = text.replace('1000.370 audiotrack_write: wrote 7680 out of 7680',
                            '1000.370 audiotrack_write: wrote 1536 out of 1536')
        wait = ('1000.370 compressed_write_wait: epoch=1 fmt=4747 last_progress_ms=1000 '
                'start_ms=1010 end_ms=1360 blocked_ms=350 retries=35 interrupted=0 '
                'accepted=1024 unit_bytes=2560 written=1536 complete=1')
        lines = text.splitlines() + [wait]
        lines.sort(key=lambda line: float(line.split()[0]))
        result = analyze('\n'.join(lines), 'seek', config({}))
        self.assertEqual(result['backpressure_write_gaps'], 1)
        self.assertEqual(result['unexplained_write_gaps'], 0)
        self.assertEqual(result['compressed_wait_completed_units'], 1)
        self.assertIn('audio_write_gap', result['reason'])
        self.assertEqual(result['verdict'], 'FAIL')
        for change in (wait.replace('epoch=1', 'epoch=2'),
                       wait.replace('interrupted=0', 'interrupted=1'),
                       wait.replace('1000.370', '1001.370'),
                       wait.replace('last_progress_ms=1000', 'last_progress_ms=1100')):
            rows = records(text + '\n' + change)
            writes = [r for r in rows if 'audiotrack_write:' in r[1]]
            detail = write_gap_evidence(rows,writes,1003020,250,1)
            self.assertEqual(detail['backpressure_write_gaps'], 0)
            self.assertEqual(detail['unexplained_write_gaps'], 1)
        self.assertIsNone(compressed_wait(wait.replace('complete=1', 'complete=0')))
        self.assertIsNone(compressed_wait(wait.replace('blocked_ms=350', 'blocked_ms=1')))
        incomplete = wait.replace('unit_bytes=2560', 'unit_bytes=4096').replace('complete=1', 'complete=0')
        rows = records(text + '\n' + incomplete)
        writes = [r for r in rows if 'audiotrack_write:' in r[1]]
        detail = write_gap_evidence(rows,writes,1003500,250,1)
        self.assertEqual(detail['incomplete_backpressure_write_gaps'], 1)
        self.assertEqual(detail['unexplained_write_gaps'], 1)  # Silent tail remains unexplained.
        rows = records(text.replace('1000.370 audiotrack_write: wrote 1536',
                                    '1000.370 audiotrack_write: wrote 512') + '\n' + wait)
        writes = [r for r in rows if 'audiotrack_write:' in r[1]]
        self.assertEqual(write_gap_evidence(rows,writes,1003020,250,1)['backpressure_write_gaps'], 0)


if __name__ == '__main__':
    unittest.main()
