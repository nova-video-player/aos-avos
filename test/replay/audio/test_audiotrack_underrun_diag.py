"""PCM diagnostic queries must not alter the production timestamp/cache."""
import unittest
from native_replay import extract, run_c, source


class UnderrunDiagTest(unittest.TestCase):
    def test_counter_increase_reset_and_errors(self):
        run_c(r'''
#include <assert.h>
#include <limits.h>
''' + extract(source('audio_interface_audiotrack_java.c'), 'static int audiotrack_underrun_delta(') + r'''
int main(void) {
    int last=0;
    assert(audiotrack_underrun_delta(&last,0)==0);
    assert(audiotrack_underrun_delta(&last,3)==3);
    assert(audiotrack_underrun_delta(&last,3)==0);
    assert(audiotrack_underrun_delta(&last,-1)==0 && last==3);
    assert(audiotrack_underrun_delta(&last,1)==0 && last==1);
    assert(audiotrack_underrun_delta(&last,2)==1);
    assert(audiotrack_underrun_delta(&last,INT_MAX)==INT_MAX-2);
    assert(audiotrack_underrun_delta(&last,0)==0 && last==0);
    return 0;
}
''')

    def test_gating_polling_and_read_only_snapshots(self):
        text = source('audio_interface_audiotrack_java.c')
        diag = extract(text, 'static void audiotrack_pcm_underrun_diag(')
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
typedef int jint;
typedef int64_t jlong;
typedef void *jobject;
typedef void *jclass;
typedef const char *jmethodID;
typedef int jfieldID;
typedef const struct fake_jni *JNIEnv;
struct fake_jni {
    jobject (*NewObject)(JNIEnv *, jclass, jmethodID);
    int (*ExceptionCheck)(JNIEnv *);
    void (*ExceptionClear)(JNIEnv *);
    int (*CallBooleanMethod)(JNIEnv *, jobject, jmethodID, jobject);
    jlong (*GetLongField)(JNIEnv *, jobject, jfieldID);
    void (*DeleteLocalRef)(JNIEnv *, jobject);
};
typedef struct {
    int passthrough, underrun_diag_initialized, last_underrun_count;
    int64_t underrun_diag_last_ms;
    JNIEnv *env;
    jclass audiotrackClass, audioTimestampClass;
    jobject obj, audioTimestamp;
    jmethodID underrun_diag_count_method, underrun_diag_state_method;
    jmethodID underrun_diag_head_method, underrun_diag_timestamp_ctor, getTimestampMethodID;
    jfieldID framePositionFieldID, nanoTimeFieldID;
    pthread_mutex_t presentation_mutex;
    uint64_t presentation_generation, i_samples_written, timestamp_written_offset;
    uint64_t last_timestamp_frames, playhead_epoch_offset;
    int64_t last_timestamp_ns;
    int ts_use_timestamp, ts_cached_delay_ms, last_delay_ret, format, rate;
    char last_delay_src[32];
    size_t buf_size, frame_size;
} audio_ctx_t;
static int Debug[1], audiotrack_log_underruns, api=24;
#define DBG_SINK 0
static int64_t wall=1000;
static int queries, count, records, objects, deleted, fail_timestamp, exception;
static int device_get_android_api(void) { return api; }
static int64_t atime64(void) { return wall; }
static float get_effective_audio_speed(void) { return 1.0f; }
static void serprintf_record(const char *fmt, ...) { (void)fmt; records++; }
static jmethodID audiotrack_diag_method(JNIEnv *e,jclass c,const char *n,const char *s) {
    (void)e;(void)c;(void)s; return n;
}
static int audiotrack_diag_int(audio_ctx_t *a,jmethodID m,jint *v) {
    (void)a; queries++;
    *v=!strcmp(m,"getUnderrunCount") ? count : 123; return 1;
}
static jobject new_object(JNIEnv *e,jclass c,jmethodID m) {
    (void)e;(void)c;(void)m; objects++; return (void *)2;
}
static int check(JNIEnv *e) { (void)e; return exception; }
static void clear(JNIEnv *e) { (void)e; exception=0; }
static int timestamp(JNIEnv *e,jobject o,jmethodID m,jobject ts) {
    (void)e;(void)o;(void)m; assert(ts==(void *)2);
    exception=fail_timestamp; return 1;
}
static jlong field(JNIEnv *e,jobject o,jfieldID f) { (void)e;(void)o;(void)f; return 42; }
static void delete_ref(JNIEnv *e,jobject o) { (void)e; assert(o==(void *)2); deleted++; }
''' + extract(text, 'static int audiotrack_underrun_delta(') + diag + r'''
int main(void) {
    const struct fake_jni table={new_object,check,clear,timestamp,field,delete_ref};
    JNIEnv env=&table;
    audio_ctx_t a={.env=&env,.audioTimestampClass=(void *)1,.audioTimestamp=(void *)1,
        .getTimestampMethodID="timestamp",.framePositionFieldID=1,.nanoTimeFieldID=2,
        .last_timestamp_frames=100,.last_timestamp_ns=200,.ts_use_timestamp=1,
        .ts_cached_delay_ms=80,.last_delay_ret=70,.i_samples_written=300,
        .timestamp_written_offset=30,.playhead_epoch_offset=10};
    pthread_mutex_init(&a.presentation_mutex,NULL);
    audiotrack_pcm_underrun_diag(&a); assert(queries==0);
    Debug[0]=1; api=23;
    audiotrack_pcm_underrun_diag(&a); assert(queries==0);
    api=24; a.passthrough=2;
    audiotrack_pcm_underrun_diag(&a); assert(queries==0);
    a.passthrough=0;
    audiotrack_pcm_underrun_diag(&a); assert(queries==1 && records==0 && objects==0);
    count=1; wall=1099;
    audiotrack_pcm_underrun_diag(&a); assert(queries==1);
    wall=1100;
    audiotrack_pcm_underrun_diag(&a); assert(queries==4 && records==1 && deleted==1);
    wall=1200;
    audiotrack_pcm_underrun_diag(&a); assert(queries==5 && records==1);
    count=0; wall=1300;
    audiotrack_pcm_underrun_diag(&a); assert(records==1);
    count=2; wall=1400; fail_timestamp=1;
    audiotrack_pcm_underrun_diag(&a); assert(records==2 && !exception && deleted==2);
    assert(a.audioTimestamp==(void *)1 && a.last_timestamp_frames==100);
    assert(a.last_timestamp_ns==200 && a.ts_use_timestamp==1);
    assert(a.ts_cached_delay_ms==80 && a.last_delay_ret==70);
    assert(a.i_samples_written==300 && a.timestamp_written_offset==30 && a.playhead_epoch_offset==10);
    Debug[0]=0; wall=1500;
    int before=queries;
    audiotrack_pcm_underrun_diag(&a); assert(queries==before);
    pthread_mutex_destroy(&a.presentation_mutex);
    return 0;
}
''')

    def test_write_gate_and_track_recreation(self):
        text = source('audio_interface_audiotrack_java.c')
        write = extract(text, 'static int audiotrack_write(')
        success = extract(write, 'if (ret > 0)')
        self.assertIn('#if DEBUG_LOGS\n\t\taudiotrack_pcm_underrun_diag(at);\n#endif', success)
        config = extract(text, 'static int audiotrack_set_output_params(')
        self.assertIn('at->last_underrun_count = 0;', config)
        self.assertIn('at->underrun_diag_last_ms = 0;', config)
        self.assertIn('#if DEBUG_LOGS\nstatic int audiotrack_underrun_delta(', text)


if __name__ == '__main__':
    unittest.main()
