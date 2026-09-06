#include "sub_engine_registry.h"
#include <pthread.h>
#include <stddef.h>
#include <time.h>
#include <android/log.h>

#define REG_TAG "SubEngineRegistry"
#define REG_LOGW(...) __android_log_print(ANDROID_LOG_WARN, REG_TAG, __VA_ARGS__)

// Upper bound on how long retract() will block waiting for outstanding
// acquire()s to drain. In normal operation this is always fast (a STREAM
// close finishing its teardown), but this cap exists so that a caller-
// ordering mistake elsewhere in the codebase (e.g. something calling
// nativeDestroy() from the same thread a pending release() is queued behind)
// degrades to a loud, bounded stall instead of hanging the calling thread --
// typically the app's UI thread -- forever.
#define RETRACT_WAIT_TIMEOUT_MS 2000

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_drained = PTHREAD_COND_INITIALIZER;
static SUB_ENGINE      *s_current = NULL;   // currently published engine, or NULL
static int              s_refcount = 0;     // outstanding acquire()s on s_current
static SUB_ENGINE      *s_retracting = NULL; // engine currently draining in retract(), if any

void sub_engine_registry_publish(SUB_ENGINE *eng) {
    pthread_mutex_lock(&s_lock);
    // A new engine can only be published once the previous one has been
    // fully retracted (nativeDestroy() blocks on that before returning), so
    // s_current should already be NULL here in the intended usage. Assigning
    // over it unconditionally is still safe -- it just means whoever still
    // holds outstanding references to the OLD engine keeps them (their
    // release() calls decrement s_refcount as normal), while any brand-new
    // acquire() from this point on returns the new engine.
    s_current = eng;
    s_refcount = 0;
    pthread_mutex_unlock(&s_lock);
}

void sub_engine_registry_retract(SUB_ENGINE *eng) {
    pthread_mutex_lock(&s_lock);
    if (s_current == eng) {
        s_current = NULL;      // no NEW acquire() can hand this pointer out anymore
        s_retracting = eng;    // remember what we're draining, for the wait predicate below
        while (s_refcount > 0) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec  += RETRACT_WAIT_TIMEOUT_MS / 1000;
            deadline.tv_nsec += (RETRACT_WAIT_TIMEOUT_MS % 1000) * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

            // Every outstanding holder releases via sub_engine_registry_release(),
            // which signals s_drained. This blocks nativeDestroy() (and therefore
            // sub_engine_destroy()/free) until the last STREAM holding a reference
            // to THIS engine has let go of it -- up to the timeout above.
            int rc = pthread_cond_timedwait(&s_drained, &s_lock, &deadline);
            if (rc != 0 && s_refcount > 0) {
                // Timed out with references still outstanding: almost certainly a
                // caller-ordering bug (something acquired a reference and never
                // released it, or retract() got called from the same thread a
                // pending release was queued behind). Log loudly and proceed
                // anyway rather than hang forever -- a leaked/late release after
                // this point is a use-after-free risk in the old design too, so
                // this is strictly no worse, just no longer silent.
                REG_LOGW("retract: TIMED OUT after %dms waiting for %d outstanding reference(s) on eng=%p -- proceeding anyway, this is a caller bug",
                         RETRACT_WAIT_TIMEOUT_MS, s_refcount, (void*)eng);
                break;
            }
        }
        s_retracting = NULL;
    }
    // If s_current != eng, this engine was already superseded/retracted by a
    // prior call (or never published) -- nothing to do, safe no-op.
    pthread_mutex_unlock(&s_lock);
}

SUB_ENGINE *sub_engine_registry_acquire(void) {
    pthread_mutex_lock(&s_lock);
    SUB_ENGINE *eng = s_current;
    if (eng) s_refcount++;
    pthread_mutex_unlock(&s_lock);
    return eng;
}

void sub_engine_registry_release(SUB_ENGINE *eng) {
    if (!eng) return;
    pthread_mutex_lock(&s_lock);
    // Only decrement if this release corresponds to the engine currently
    // being tracked (either still published as s_current, or mid-retract as
    // s_retracting) -- guards against a stray double-release being confused
    // for a still-live reference on a totally unrelated engine instance.
    if (eng == s_current || eng == s_retracting) {
        if (s_refcount > 0) s_refcount--;
        if (s_refcount == 0) pthread_cond_broadcast(&s_drained);
    }
    pthread_mutex_unlock(&s_lock);
}
