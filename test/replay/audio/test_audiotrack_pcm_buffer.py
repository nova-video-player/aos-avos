"""Exercise production PCM sizing without an Android runtime."""
import unittest
from native_replay import extract, run_c, source


class PcmBufferTest(unittest.TestCase):
    def test_headroom_and_jint_frame_limits(self):
        code = source('audio_interface_audiotrack_java.c')
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
''' + extract(code, 'static size_t audiotrack_pcm_buffer_size(') + r'''
int main(void) {
    assert(audiotrack_pcm_buffer_size(46176,48000,12,0)==92352);
    assert(audiotrack_pcm_buffer_size(7680,48000,4,0)==28800);
    assert(audiotrack_pcm_buffer_size(34560,48000,4,0)==48000);
    assert(audiotrack_pcm_buffer_size(57600,48000,4,0)==57600);
    assert(audiotrack_pcm_buffer_size(46176,48000,12,1)==138528);
    assert(audiotrack_pcm_buffer_size(7681,48000,4,1)==23044);
    assert(audiotrack_pcm_buffer_size(INT_MAX,48000,1,0)==INT_MAX);
    // No aligned jint can hold this minimum: explicitly reject, never shrink it.
    assert(audiotrack_pcm_buffer_size(INT_MAX,48000,12,0)==0);
    for (int hw=0;hw<=1;hw++) {
        size_t n=audiotrack_pcm_buffer_size(1024,INT_MAX,INT_MAX,hw);
        assert(n==INT_MAX);
        n=audiotrack_pcm_buffer_size(INT_MAX-31,INT_MAX,12,hw);
        assert(n>=INT_MAX-31 && n<=INT_MAX && n%12==0);
    }
    assert(!audiotrack_pcm_buffer_size(1024,0,4,0));
    assert(!audiotrack_pcm_buffer_size(1024,48000,0,0));
    assert(!audiotrack_pcm_buffer_size(1024,48000,SIZE_MAX,0));
    return 0;
}
''')

    def test_compressed_and_speed_policies_stay_separate(self):
        code = source('audio_interface_audiotrack_java.c')
        self.assertIn('at->buf_size = (min_buffer_size > 32768) ? min_buffer_size : 32768;', code)
        self.assertIn('sampleRateInHz, frame_size, buffer_scale == 3)', code)
        self.assertIn('buffer_scale = 3;', code)


if __name__ == '__main__':
    unittest.main()
