"""Replay public-window ownership and gralloc acquisition failure cleanup."""
from pathlib import Path
import unittest

from native_replay import ROOT, extract, run_c

C_SOURCE = ROOT / 'external/android/libavos_android/android_buffer.c'


class AndroidPublicBufferTest(unittest.TestCase):
    def test_cancel_reuse_and_failure_cleanup(self):
        source = C_SOURCE.read_text()
        functions = [
            'static int android_buffer_size(',
            'static int android_public_buffer_lock(',
            'static int android_public_buffer_release(',
            'void android_surface_destroy(',
            'int android_buffer_close(',
            'static int android_buffer_lock_data(',
            'static int android_buffer_unlock_data(',
            'static int android_gralloc_buffer_acquire(',
            'int android_buffer_dequeue(',
            'int android_buffer_dequeue_with_buffer(',
            'int android_buffer_queue(',
            'int android_buffer_alloc(',
            'int android_buffer_cancel(',
        ]
        run_c(r'''
#include <assert.h>
#include <limits.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "''' + str(ROOT / 'external/android/libavos_android/include/android_buffer.h') + r'''"
#include "''' + str(ROOT / 'external/android/include/21/system/core/include/system/graphics.h') + r'''"
#define AVOSLOG(...) ((void)0)
#define NO_ERROR 0
#define CHECK_ERR() do { if (err != NO_ERROR) return -1; } while (0)
#define NATIVE_WINDOW_API_MEDIA 2
typedef int status_t;
typedef struct { void *handle; int width, height, stride, format; } ANativeWindowBuffer_t;
typedef struct { void *bits; int width, height, stride, format; } ANativeWindow_Buffer;
typedef struct ANativeWindow {
    int (*dequeueBuffer_DEPRECATED)(struct ANativeWindow *, ANativeWindowBuffer_t **);
    int (*lockBuffer_DEPRECATED)(struct ANativeWindow *, ANativeWindowBuffer_t *);
    int (*queueBuffer_DEPRECATED)(struct ANativeWindow *, ANativeWindowBuffer_t *);
    int (*cancelBuffer_DEPRECATED)(struct ANativeWindow *, ANativeWindowBuffer_t *);
} ANativeWindow;
typedef struct gralloc_module {
    int (*lock)(const struct gralloc_module *, void *, int, int, int, int, int, void **);
    int (*unlock)(const struct gralloc_module *, void *);
} gralloc_module_t;
static int locks, posts, held, lock_error, post_error, allocations, fail_alloc;
static unsigned char pixels[4096];
static int width = 16, height = 16, stride = 16, format = HAL_PIXEL_FORMAT_RGBX_8888;
static ANativeWindowBuffer_t native_buffer;
static int gralloc_owned, cpu_locked, fail_stage, cancel_error;
static int dequeues, legacy_locks, cpu_locks, cpu_unlocks, cancels, queues;
static int mock_dequeue(ANativeWindow *window, ANativeWindowBuffer_t **out) {
    assert(window && out && !gralloc_owned && !cpu_locked);
    dequeues++;
    if (fail_stage == 1) return -11;
    gralloc_owned = 1; *out = &native_buffer; return 0;
}
static int mock_legacy_lock(ANativeWindow *window, ANativeWindowBuffer_t *buffer) {
    assert(window && buffer == &native_buffer && gralloc_owned && !cpu_locked);
    legacy_locks++;
    return fail_stage == 2 ? -22 : 0;
}
static int mock_cpu_lock(const gralloc_module_t *module, void *handle, int usage,
                         int x, int y, int w, int h, void **data) {
    (void)usage;
    assert(module && handle == native_buffer.handle && gralloc_owned && !cpu_locked);
    assert(!x && !y && w == native_buffer.width && h == native_buffer.height && data);
    cpu_locks++;
    if (fail_stage == 3) return -5;
    cpu_locked = 1; *data = pixels; return 0;
}
static int mock_cpu_unlock(const gralloc_module_t *module, void *handle) {
    assert(module && handle == native_buffer.handle && gralloc_owned && cpu_locked);
    cpu_unlocks++; cpu_locked = 0; return 0;
}
static int mock_cancel(ANativeWindow *window, ANativeWindowBuffer_t *buffer) {
    assert(window && buffer == &native_buffer && gralloc_owned && !cpu_locked);
    cancels++;
    if (cancel_error) return -5;
    gralloc_owned = 0; return 0;
}
static int mock_queue(ANativeWindow *window, ANativeWindowBuffer_t *buffer) {
    assert(window && buffer == &native_buffer && gralloc_owned && !cpu_locked);
    queues++; gralloc_owned = 0; return 0;
}

static int ANativeWindow_lock(ANativeWindow *window, ANativeWindow_Buffer *buffer, void *dirty) {
    assert(window && buffer && !dirty && !held);
    locks++;
    if (lock_error) return -1;
    held = 1;
    *buffer = (ANativeWindow_Buffer){pixels, width, height, stride, format};
    return 0;
}
static int ANativeWindow_unlockAndPost(ANativeWindow *window) {
    assert(window && held);
    held = 0;
    posts++;
    return post_error ? -1 : 0;
}
static void native_window_api_disconnect(ANativeWindow *window, int api) {
    (void)window; (void)api; abort();
}
static void *tracked_calloc(size_t count, size_t size) {
    if (fail_alloc) return NULL;
    void *p = calloc(count, size);
    assert(p); allocations++;
    return p;
}
static void tracked_free(void *p) {
    if (p) allocations--;
    free(p);
}
#define calloc tracked_calloc
#define free tracked_free
''' + extract(source, 'struct android_surface {') + ';\n' +
              '\n'.join(extract(source, marker) for marker in functions) + r'''
int main(void) {
    ANativeWindow window = {0};
    android_surface_t *as = calloc(1, sizeof(*as));
    as->anw = &window;
    android_buffer_t buffer = {0};
    fail_alloc = 1;
    assert(android_buffer_alloc(as, &buffer) == -1);
    assert(locks == 0 && allocations == 1);
    fail_alloc = 0;
    assert(!android_buffer_alloc(as, &buffer));
    void *handle = buffer.handle, *next = NULL;
    assert(handle && allocations == 2);
    assert(buffer.size == 1024 && buffer.hal_format == HAL_PIXEL_FORMAT_RGBX_8888);
    assert(!android_buffer_cancel(as, handle));
    assert(as->anbp == handle && allocations == 2 && posts == 1);
    /* A second cancel must neither post nor free the retained descriptor. */
    assert(android_buffer_cancel(as, handle) == -1 && posts == 1);
    for (int i = 0; i < 10; i++) {
        assert(!android_buffer_dequeue_with_buffer(as, &next, &buffer));
        assert(next == handle && buffer.data == pixels);
        int before = locks;
        assert(android_buffer_dequeue(as, &next) == -1 && locks == before);
        assert(android_buffer_cancel(as, &window) == -1 && held);
        assert(!(i % 2 ? android_buffer_queue(as, handle) : android_buffer_cancel(as, handle)));
    }
    lock_error = 1;
    assert(android_buffer_dequeue(as, &next) == -1);
    assert(!held && !as->public_buffer_locked);
    int before = posts;
    assert(android_buffer_cancel(as, handle) == -1 && posts == before);
    lock_error = 0;
    assert(!android_buffer_dequeue(as, &next));
    post_error = 1;
    assert(android_buffer_queue(as, next) == -1);
    before = posts;
    assert(android_buffer_cancel(as, next) == -1 && posts == before);
    post_error = 0;
    assert(!android_buffer_dequeue(as, &next) && next == handle);
    assert(!android_buffer_close(as) && !held);
    before = posts;
    assert(!android_buffer_close(as) && posts == before);
    assert(allocations == 2);
    android_surface_destroy(&as);
    assert(!as && allocations == 0 && posts == before);
    /* Exercise both size-reporting entry points with padded rows. */
    as = calloc(1, sizeof(*as)); as->anw = &window;
    int formats[] = {HAL_PIXEL_FORMAT_RGBA_8888, HAL_PIXEL_FORMAT_RGBX_8888,
        HAL_PIXEL_FORMAT_BGRA_8888, HAL_PIXEL_FORMAT_RGB_888, HAL_PIXEL_FORMAT_RGB_565,
        HAL_PIXEL_FORMAT_YV12, HAL_PIXEL_FORMAT_YCrCb_420_SP};
    size_t sizes[] = {2560, 2560, 2560, 1920, 1280, 1024, 960};
    width = 30; height = 8; stride = 80;
    for (unsigned i = 0; i < sizeof(formats)/sizeof(*formats); i++) {
        format = formats[i];
        assert(!android_buffer_alloc(as, &buffer));
        assert(buffer.size == sizes[i] && buffer.hal_format == format);
        assert(!android_buffer_queue(as, buffer.handle));
        assert(!android_buffer_dequeue_with_buffer(as, &next, &buffer));
        assert(buffer.size == sizes[i] && buffer.hal_format == format);
        assert(!android_buffer_cancel(as, next));
    }
    /* Reject invalid metadata after releasing the acquired window lock. */
    format = HAL_PIXEL_FORMAT_YV12; height = 7;
    assert(android_buffer_alloc(as, &buffer) == -1 && !held && !buffer.handle);
    height = 8; format = 0x7fffffff;
    assert(android_buffer_dequeue_with_buffer(as, &next, &buffer) == -1);
    assert(!held && !next && !buffer.data && !buffer.size);
    format = HAL_PIXEL_FORMAT_RGBX_8888;
    width = height = stride = INT_MAX;
    assert(android_buffer_alloc(as, &buffer) == -1 && !held);
    size_t size;
    assert(android_buffer_size(-1, 8, 80, format, 0, &size) == -1);
    assert(android_buffer_size(80, 8, 30, format, 0, &size) == -1);
    assert(android_buffer_size(30, 8, 80, 0x7fffffff, 1, &size) == 0 && size == 960);
    assert(android_buffer_size(INT_MAX, INT_MAX, INT_MAX, 0x7fffffff, 1, &size) == -1);
    assert(android_buffer_size(30, 8, 30, HAL_PIXEL_FORMAT_YV12, 0, &size) == -1);
    width = height = stride = 16;
    assert(!android_buffer_alloc(as, &buffer));
    assert(!android_buffer_queue(as, buffer.handle));
    android_surface_destroy(&as);
    assert(!as && !held && allocations == 0);
    /* Partial-open cleanup must release an outstanding lock exactly once. */
    as = calloc(1, sizeof(*as)); as->anw = &window;
    assert(!android_buffer_alloc(as, &buffer));
    before = posts;
    android_surface_destroy(&as);
    assert(!as && !held && allocations == 0 && posts == before + 1);

    /* All three gralloc entry points must release a dequeued buffer if either
       lock fails, without unlocking an unacquired CPU mapping. */
    gralloc_module_t gralloc = { mock_cpu_lock, mock_cpu_unlock };
    ANativeWindow gralloc_window = { mock_dequeue, mock_legacy_lock, mock_queue, mock_cancel };
    android_surface_t gralloc_surface = { .anw = &gralloc_window, .gralloc = &gralloc,
                                         .buffer_type = BUFFER_TYPE_SW };
    native_buffer = (ANativeWindowBuffer_t){pixels, 16, 16, 16, HAL_PIXEL_FORMAT_RGBX_8888};
    for (int entry = 0; entry < 3; entry++) {
        for (int stage = 1; stage <= 3; stage++) {
            fail_stage = stage;
            int dq = dequeues, ll = legacy_locks, cl = cpu_locks;
            int ca = cancels, cu = cpu_unlocks, qu = queues;
            next = &window;
            memset(&buffer, 0, sizeof(buffer));
            int result = entry == 0 ? android_buffer_alloc(&gralloc_surface, &buffer) :
                         entry == 1 ? android_buffer_dequeue(&gralloc_surface, &next) :
                         android_buffer_dequeue_with_buffer(&gralloc_surface, &next, &buffer);
            assert(result == -1 && !gralloc_owned && !cpu_locked);
            assert(dequeues == dq + 1 && legacy_locks == ll + (stage >= 2));
            assert(cpu_locks == cl + (stage >= 3));
            assert(cancels == ca + (stage >= 2) && cpu_unlocks == cu && queues == qu);
            assert(entry == 0 ? (!buffer.handle && !buffer.data) : !next);
            /* Immediately reuse the same one-buffer queue after each failure. */
            fail_stage = 0;
            result = entry == 0 ? android_buffer_alloc(&gralloc_surface, &buffer) :
                     entry == 1 ? android_buffer_dequeue(&gralloc_surface, &next) :
                     android_buffer_dequeue_with_buffer(&gralloc_surface, &next, &buffer);
            assert(!result && gralloc_owned && cpu_locked);
            void *acquired = entry == 0 ? buffer.handle : next;
            assert(acquired == &native_buffer);
            if (entry == 0) assert(buffer.data == pixels && buffer.size == 1024);
            assert(!android_buffer_queue(&gralloc_surface, acquired));
            assert(!gralloc_owned && !cpu_locked && cpu_unlocks == cu + 1 && queues == qu + 1);
        }
    }
    /* A failing cancel is attempted once, without hiding the acquisition error
       or publishing the unusable handle. The backend still owns the failure. */
    for (int entry = 0; entry < 3; entry++) {
        fail_stage = 3; cancel_error = 1;
        int ca = cancels, cu = cpu_unlocks;
        next = &window;
        memset(&buffer, 0, sizeof(buffer));
        int result = entry == 0 ? android_buffer_alloc(&gralloc_surface, &buffer) :
                     entry == 1 ? android_buffer_dequeue(&gralloc_surface, &next) :
                     android_buffer_dequeue_with_buffer(&gralloc_surface, &next, &buffer);
        assert(result == -1 && cancels == ca + 1 && cpu_unlocks == cu && !cpu_locked);
        assert(entry == 0 ? !buffer.handle : !next);
        gralloc_owned = 0; /* Simulate backend teardown after its cancel failed. */
    }
    fail_stage = cancel_error = 0;
    /* Failure after successful CPU locking must unlock, then cancel once. */
    native_buffer.width = 17; native_buffer.stride = 16;
    int ca = cancels, cu = cpu_unlocks;
    assert(android_buffer_alloc(&gralloc_surface, &buffer) == -1);
    assert(!buffer.handle && !buffer.data && !gralloc_owned && !cpu_locked);
    assert(cancels == ca + 1 && cpu_unlocks == cu + 1);
    native_buffer.width = 16;
    /* Hardware buffers do not acquire or release a CPU mapping. */
    gralloc_surface.buffer_type = BUFFER_TYPE_HW;
    int cl = cpu_locks; cu = cpu_unlocks;
    assert(!android_buffer_alloc(&gralloc_surface, &buffer));
    assert(!buffer.data && !cpu_locked && gralloc_owned);
    assert(!android_buffer_cancel(&gralloc_surface, buffer.handle));
    assert(!gralloc_owned && cpu_locks == cl && cpu_unlocks == cu);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
