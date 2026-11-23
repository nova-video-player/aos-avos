"""Bound diagnostic output and preserve MediaCodec calls with tracing disabled."""
import unittest

from native_replay import extract, run_c, source, typedef


class VideoFlowDiagnosticsTest(unittest.TestCase):
    def test_aggregation_and_render_forwarding(self):
        code = source('codec_sfdec2.c')
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef int64_t INT64;
typedef int sfdec_t;
typedef int sfbuf_t;
#define NSEC_PER_SEC 1000000000LL
#define NSEC_PER_MSEC 1000000LL
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
static char record[2048];
static int emitted, clock_calls, releases, expected_render, release_result;
static INT64 expected_deadline;
static sfdec_t decoder;
static sfbuf_t buffer;
static void serprintf_record(const char *fmt, ...) {
    va_list args; va_start(args,fmt);
    vsnprintf(record,sizeof(record),fmt,args); va_end(args); emitted++;
}
static INT64 _get_monotonic_ns(void) {
    return (INT64)++clock_calls * 125000000;
}
static int sfdec_buf_render(sfdec_t *s, sfbuf_t *b, int render, int asap, INT64 deadline) {
    assert(s==&decoder && b==&buffer && render==expected_render);
    assert(asap==0 && deadline==expected_deadline);
    releases++; return release_result;
}
''' + typedef(code, 'video_flow_diag_t') + '\n' +
            extract(code, 'static void video_flow_diag_update(') + '\n' +
            extract(code, 'static int video_flow_render(') + r'''
int main(void) {
    video_flow_diag_t d={0};
    // High-frequency observations still emit just once per second, preserving
    // slow calls, gaps and queue extrema rather than printing every sample.
    for (int i=0;i<=1000;i++) {
        video_flow_diag_update(&d,"decode",1,(INT64)(i+1)*NSEC_PER_MSEC,
            i==500?125000000:1000,i==700?250000000:0,
            i%40==0,i,i%3,1,2,0);
        assert(emitted==(i==1000));
    }
    assert(strstr(record,"stage=decode") && strstr(record,"window_ms=1000"));
    assert(strstr(record,"outputs=26") && strstr(record,"slow_calls=1"));
    assert(strstr(record,"call_max_us=125000") && strstr(record,"output_gap_max_us=40000"));
    assert(strstr(record,"wait_max_us=250000") && strstr(record,"render_q_min=0 render_q_max=2"));
    // Preserve the gap across report boundaries, and reset across generations.
    video_flow_diag_update(&d,"decode",1,2001000000LL,0,0,1,42,1,0,0,0);
    assert(emitted==2 && strstr(record,"output_gap_max_us=1000000"));
    video_flow_diag_update(&d,"render",2,9000000000LL,0,0,0,43,0,0,0,1);
    assert(emitted==2 && d.last_output_ns==0);
    video_flow_diag_update(&d,"render",2,10000000000LL,0,0,1,44,1,0,0,1);
    assert(emitted==3 && strstr(record,"generation=2") && strstr(record,"paused=1"));
    assert(strstr(record,"output_gap_max_us=0"));
    // Tracing never changes render/drop arguments or error propagation; off
    // means no diagnostic clock reads, on measures exactly the release call.
    for (int trace=0;trace<=1;trace++) {
        for (int render=0;render<=1;render++) {
            for (int failure=0;failure<=1;failure++) {
                expected_render=render; expected_deadline=render?123456789:0;
                release_result=failure?-1:0;
                int old_clocks=clock_calls, old_releases=releases;
                INT64 duration=0;
                assert(video_flow_render(&decoder,&buffer,render,expected_deadline,trace,&duration)==release_result);
                assert(releases==old_releases+1 && clock_calls==old_clocks+2*trace);
                assert(duration==(trace?125000000:0));
            }
        }
    }
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
