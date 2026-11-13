"""Replay reset and continuous AudioTrack counters across a flush."""
import unittest

from native_replay import extract, run_c, source


class AudioTrackObserverEpochTest(unittest.TestCase):
    def test_reset_origin_does_not_reactivate_at_old_offset(self):
        code = source('audio_interface_audiotrack_java.c')
        run_c(r'''
#include <assert.h>
#include <stdint.h>
''' + extract(code, 'static uint64_t audiotrack_observer_counter(') + r'''
int main(void) {
    const uint64_t old_base = 5995008;
    const int64_t start = 1000000000;
    uint64_t head = old_base, timestamp = old_base;
    // A cached DAC timestamp behind the flush playhead is not a reset.
    assert(!audiotrack_observer_counter(old_base-24000, start-1, start, &timestamp));
    assert(timestamp == old_base);
    // Continuous-counter devices keep their existing subtraction.
    assert(audiotrack_observer_counter(old_base+4800, start+1, start, &timestamp)==4800);
    assert(timestamp == old_base);
    // The playhead can reset before the timestamp, independently.
    assert(!audiotrack_observer_counter(0, start, start, &head));
    assert(head == 0 && timestamp == old_base);
    assert(audiotrack_observer_counter(1024, start+2, start, &timestamp)==1024);
    assert(timestamp == 0);
    // Bravia failure: crossing the previous base must remain continuous.
    assert(audiotrack_observer_counter(5989825, start+3, start, &timestamp)==5989825);
    assert(audiotrack_observer_counter(old_base, start+4, start, &timestamp)==old_base);
    assert(audiotrack_observer_counter(old_base+18790, start+5, start, &timestamp)==6013798);
    assert(audiotrack_observer_counter(old_base+48000, start+6, start, &head)==6043008);
    // A delayed old timestamp cannot undo that decision.
    assert(!audiotrack_observer_counter(old_base+100, start-1, start, &timestamp));
    assert(timestamp == 0);
    // A new generation re-arms the base; 64-bit unwrapped counters stay intact.
    head = UINT32_MAX-1024ULL;
    assert(audiotrack_observer_counter((1ULL<<32)+1024, start, start, &head)==2049);
    timestamp = 0;
    assert(audiotrack_observer_counter(4800, start, start, &timestamp)==4800);
    return 0;
}
''')

    def test_origins_are_generation_scoped_and_queries_guarded(self):
        code = source('audio_interface_audiotrack_java.c')
        thread = extract(code, 'static void *audiotrack_presentation_thread(void *arg)\n{')
        reset = extract(thread, 'if (playhead_generation != generation)')
        self.assertIn('playhead_offset = epoch_offset;', reset)
        self.assertIn('timestamp_offset = epoch_offset;', reset)
        self.assertIn('if (playhead_ok)\n\t\t\tsample.playback_head_frames = audiotrack_observer_counter(', thread)
        self.assertIn('if (timestamp_position > 0)\n\t\t\t\t\tsample.timestamp_frames = audiotrack_observer_counter(', thread)
        self.assertIn('generation == at->presentation_generation', thread)
        timing = extract(code, 'static void audiotrack_reset_timing(audio_ctx_t *at)\n{')
        self.assertIn('clock_gettime(CLOCK_MONOTONIC, &epoch_start)', timing)
        self.assertIn('at->presentation_epoch_start_ns =', timing)


if __name__ == '__main__':
    unittest.main()
