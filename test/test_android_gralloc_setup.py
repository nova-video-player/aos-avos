"""Replay gralloc setup query failures without an Android runtime."""
import unittest
from native_replay import ROOT, extract, run_c


class AndroidGrallocSetupTest(unittest.TestCase):
    def test_query_errors_and_public_setup(self):
        source = (ROOT / 'external/android/libavos_android/android_buffer.c').read_text()
        run_c(r'''
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#define NO_ERROR 0
#define CHECK_ERR() do { if (err != NO_ERROR) return -1; } while (0)
#define NATIVE_WINDOW_API_MEDIA 2
#define NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS 3
#define NATIVE_WINDOW_CONCRETE_TYPE 4
#define NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW 1
#define GRALLOC_USAGE_SW_READ_OFTEN 1
#define GRALLOC_USAGE_SW_WRITE_OFTEN 2
#define GRALLOC_USAGE_HW_RENDER 4
#define GRALLOC_USAGE_HW_TEXTURE 8
#define GRALLOC_USAGE_EXTERNAL_DISP 16
#define BUFFER_TYPE_SW 0
#define BUFFER_TYPE_HW 1
typedef int status_t;
typedef struct ANativeWindow {
    int (*query)(const struct ANativeWindow *, int, int *);
} ANativeWindow;
typedef struct { int unused; } ANativeWindow_Buffer;
typedef struct { int unused; } gralloc_module_t;
typedef struct android_surface android_surface_t;
static int min_error, type_error, queries, connect_calls, buffer_count_calls;
static int geometry_calls, geometry_error, negotiated_count, diagnostic_values;
static int query(const ANativeWindow *w, int what, int *value) {
    assert(w && value); queries++;
    if (what == NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS) {
        *value = min_error ? 999 : 2; /* Failed output must never be consumed. */
        return min_error;
    }
    assert(what == NATIVE_WINDOW_CONCRETE_TYPE);
    if (type_error) return type_error; /* Deliberately leave output untouched. */
    *value = 42;
    return 0;
}
static void log_message(const char *format, ...) {
    if (!strcmp(format, "NATIVE_WINDOW_CONCRETE_TYPE: %d")) {
        va_list args; va_start(args, format);
        assert(va_arg(args, int) == 42); va_end(args);
        diagnostic_values++;
    }
}
#define AVOSLOG(...) log_message(__VA_ARGS__)
static int native_window_api_connect(ANativeWindow *w, int api) {
    assert(w && api == NATIVE_WINDOW_API_MEDIA); connect_calls++; return 0;
}
#define native_window_set_usage(w, u) ((void)(w), (void)(u), 0)
#define native_window_set_buffers_format(w, f) ((void)(w), (void)(f), 0)
#define native_window_set_buffers_user_dimensions(w, x, y) ((void)(w), (void)(x), (void)(y), 0)
#define native_window_set_scaling_mode(w, m) ((void)(w), (void)(m), 0)
static int native_window_set_buffer_count(ANativeWindow *w, int count) {
    assert(w); buffer_count_calls++; negotiated_count = count; return 0;
}
static int ANativeWindow_setBuffersGeometry(ANativeWindow *w, int width, int height, int format) {
    assert(w && width == 16 && height == 16 && format == 2);
    geometry_calls++; return geometry_error;
}
''' + extract(source, 'struct android_surface {') + ';\n' +
              extract(source, 'int android_buffer_setup(') + r'''
int main(void) {
    ANativeWindow window = { query };
    gralloc_module_t gralloc = { 0 };
    android_surface_t surface = { .anw = &window, .gralloc = &gralloc };
    for (int forced = 0; forced <= 3; forced += 3) {
        int count = 1, minimum = forced;
        min_error = -38;
        int before = queries, buffers_before = buffer_count_calls;
        assert(android_buffer_setup(&surface, 16, 16, BUFFER_TYPE_SW, 2, 0, &count, &minimum) == -1);
        assert(count == 1 && minimum == forced);
        assert(queries == before + 1 && buffer_count_calls == buffers_before);
        min_error = 0;
        assert(!android_buffer_setup(&surface, 16, 16, BUFFER_TYPE_SW, 2, 0, &count, &minimum));
        assert(minimum == (forced ? forced : 2));
        assert(count == 1 + minimum && negotiated_count == count);
    }
    int count = 1, minimum = 0, logged_before = diagnostic_values;
    type_error = -22;
    assert(!android_buffer_setup(&surface, 16, 16, BUFFER_TYPE_HW, 2, 0, &count, &minimum));
    assert(count == 3 && minimum == 2 && diagnostic_values == logged_before);
    /* Public-window setup never consults gralloc queries or buffer counts. */
    surface.gralloc = NULL;
    int queries_before = queries, connect_before = connect_calls;
    int buffers_before = buffer_count_calls;
    count = 1; minimum = 0; min_error = -38;
    assert(!android_buffer_setup(&surface, 16, 16, BUFFER_TYPE_SW, 2, 0, &count, &minimum));
    assert(count == 1 && minimum == 0 && geometry_calls == 1);
    geometry_error = -22;
    assert(android_buffer_setup(&surface, 16, 16, BUFFER_TYPE_SW, 2, 0, &count, &minimum) == -1);
    assert(queries == queries_before && connect_calls == connect_before && buffer_count_calls == buffers_before);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
