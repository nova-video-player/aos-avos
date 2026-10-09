"""Exercise the production diagnostic lease and generation/pause publish guard."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_mode2_seek_startup import block


class PCMObserverTest(unittest.TestCase):
    def test_lease_expiry_and_stale_observations(self):
        source = (Path(__file__).resolve().parents[1] /
                  'Source/audio_interface_audiotrack_java.c').read_text()
        policy = source[source.index('#ifdef DEBUG_MSG\n// Explicit, bounded diagnostic lease.'):
                        source.index('// AC3-recode mode2 plain-policy gate')]
        command = block(source, 'static void audiotrack_pcm_observe_cmd(')
        publish = block(source, 'if (sample.passthrough == 0) {\n\t\t\t// Independent debug evidence')
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#define DEBUG_MSG 1
static int64_t wall_ms;
static int64_t atime64(void) { return wall_ms; }
static int messages, released;
#define serprintf_record(...) (++messages)
''' + policy + command + r'''
struct JNIFunctions;
typedef const struct JNIFunctions *JNIEnv;
static void release_ref(JNIEnv *env, void *track) { (void)env; (void)track; ++released; }
struct JNIFunctions { void (*DeleteGlobalRef)(JNIEnv *, void *); };
static const struct JNIFunctions functions = {release_ref};
static void publish(int stale_generation, int paused, int running) {
    struct {
        pthread_mutex_t presentation_mutex;
        uint64_t presentation_generation;
        int presentation_run, track_paused;
    } state = {PTHREAD_MUTEX_INITIALIZER, 2, running, paused}, *at = &state;
    struct { int passthrough; uint64_t playback_head_frames; } sample = {0, 48000};
    uint64_t generation = stale_generation ? 1 : 2;
    struct timespec pcm_query_start = {1, 0}, pcm_query_end = {1, 100};
    JNIEnv jni = &functions, *env = &jni;
    void *track = NULL;
    for (int once = 0; once < 1; ++once) {
''' + publish + r'''
    }
    pthread_mutex_destroy(&state.presentation_mutex);
}
int main(void) {
    wall_ms = INT64_C(5000000000); /* Long uptime, beyond signed 32-bit ms. */
    assert((uintptr_t)&audiotrack_pcm_observe_until_ms % 8 == 0);
    assert(!audiotrack_pcm_observe_active());
    messages = 0; publish(0, 0, 1);
    assert(messages == 0 && released == 1);
    char *enable[] = {"at_pcm_observe", "600"};
    audiotrack_pcm_observe_cmd(2, enable);
    assert(audiotrack_pcm_observe_active());
    messages = 0; publish(0, 0, 1);
    assert(messages == 1 && released == 2);
    messages = 0; publish(1, 0, 1); publish(0, 1, 1); publish(0, 0, 0);
    assert(messages == 0 && released == 5);
    char *invalid[] = {"at_pcm_observe", "999999999999999999999999"};
    int64_t original = audiotrack_pcm_observe_until_ms;
    audiotrack_pcm_observe_cmd(2, invalid);
    assert(audiotrack_pcm_observe_until_ms == original);
    wall_ms += 599999;
    assert(audiotrack_pcm_observe_active());
    ++wall_ms;
    assert(!audiotrack_pcm_observe_active());
    messages = 0; publish(0, 0, 1);
    assert(messages == 0 && released == 6);
    audiotrack_pcm_observe_cmd(2, enable);
    char *disable[] = {"at_pcm_observe", "0"};
    audiotrack_pcm_observe_cmd(2, disable);
    assert(!audiotrack_pcm_observe_active());
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'observer.c'
            binary = Path(tmp) / 'observer'
            c.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-pthread', '-o', str(binary), str(c)],
                           check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
