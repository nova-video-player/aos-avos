"""Replay native-window validation without requiring an Android runtime."""
import unittest

from native_replay import ROOT, extract, run_c

BUFFER_SOURCE = ROOT / 'external/android/libavos_android/android_buffer.c'
WINDOW_SOURCE = ROOT / 'external/android/libavos_android/android_window.c'


class AndroidSurfaceValidationTest(unittest.TestCase):
    def test_invalid_windows_and_optional_gralloc(self):
        source = BUFFER_SOURCE.read_text()
        window = WINDOW_SOURCE.read_text()
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define ANDROID_NATIVE_WINDOW_MAGIC 0x5f776e64
#define GRALLOC_HARDWARE_MODULE_ID "gralloc"
#define HARDWARE_MODULE_TAG 0x48574d54
#define RTLD_DEFAULT NULL
#define AVOSLOG(...) ((void)0)
#define HAL_TRANSFORM_ROT_90 4
#define HAL_TRANSFORM_ROT_180 3
#define HAL_TRANSFORM_ROT_270 7
typedef struct { struct { int magic, version; } common; } ANativeWindow;
typedef struct { int unused; } ANativeWindow_Buffer;
typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    const char *id;
} hw_module_t;
typedef struct {
    hw_module_t common;
    int (*lock)(void);
    int (*unlock)(void);
} gralloc_module_t;
typedef struct android_surface android_surface_t;
static int allocations, fail_allocation, lookups, module_calls, probe_mode, rotations;
static uint32_t last_transform;
static int transform_error;
static int unused_callback(void) { assert(!"probe must not call lock/unlock"); return 0; }
static gralloc_module_t module = {
    .common = { HARDWARE_MODULE_TAG, 1, GRALLOC_HARDWARE_MODULE_ID },
    .lock = unused_callback, .unlock = unused_callback
};
static const hw_module_t *lookup_module = &module.common;
static void *counted_calloc(size_t count, size_t size) {
    allocations++;
    return fail_allocation ? NULL : calloc(count, size);
}
static const char *dlerror(void) { return "unavailable"; }
static int mock_hw_get_module(const char *id, const hw_module_t **out) {
    assert(!strcmp(id, GRALLOC_HARDWARE_MODULE_ID));
    module_calls++;
    if (probe_mode == 1) return -1;
    if (probe_mode == 3) return 0; /* Broken HAL: success without an output. */
    *out = lookup_module;
    return 0;
}
static void *dlsym(void *handle, const char *name) {
    assert(handle == RTLD_DEFAULT && !strcmp(name, "hw_get_module"));
    lookups++;
    return probe_mode == 0 ? NULL : (void *)mock_hw_get_module;
}
static int native_window_set_buffers_transform(ANativeWindow *window, uint32_t transform) {
    assert(window && window->common.magic == ANDROID_NATIVE_WINDOW_MAGIC);
    rotations++;
    if (!transform_error) last_transform = transform;
    return transform_error;
}
#define calloc counted_calloc
''' + extract(source, 'struct android_surface {') + ';\n' +
              extract(source, 'android_surface_t *android_surface_create(') + '\n' +
              extract(window, 'int android_window_set_buffers_rotation(') + r'''
int main(void) {
    ANativeWindow window = { .common = { 0, sizeof(ANativeWindow) } };
    assert(!android_surface_create(NULL));
    assert(android_window_set_buffers_rotation(NULL, 90) == -1);
    /* Matching structure size must not let an incorrect magic through. */
    assert(!android_surface_create(&window));
    assert(android_window_set_buffers_rotation(&window, 90) == -1);
    window.common.version = 0;
    assert(!android_surface_create(&window));
    assert(android_window_set_buffers_rotation(&window, 270) == -1);
    assert(!allocations && !lookups && !module_calls && !rotations);
    window.common.magic = ANDROID_NATIVE_WINDOW_MAGIC;
    fail_allocation = 1;
    assert(!android_surface_create(&window) && !lookups);
    fail_allocation = 0;
    /* Valid windows remain usable with missing symbols or failed HAL lookup.
       Their type check does not depend on our private-header structure size. */
    for (probe_mode = 0; probe_mode < 3; probe_mode++) {
        android_surface_t *as = android_surface_create(&window);
        assert(as && as->anw == &window);
        assert(as->gralloc == (probe_mode == 2 ? &module : NULL));
        free(as);
    }
    assert(allocations == 4 && lookups == 3 && module_calls == 2);
    /* Every rejected module preserves the valid public-window surface. */
    probe_mode = 3;
    android_surface_t *fallback = android_surface_create(&window);
    assert(fallback && fallback->anw == &window && !fallback->gralloc);
    free(fallback);
    probe_mode = 2;
    lookup_module = NULL;
    fallback = android_surface_create(&window);
    assert(fallback && !fallback->gralloc);
    free(fallback);
    lookup_module = &module.common;
    for (int invalid = 0; invalid < 5; invalid++) {
        module.common.tag = invalid == 0 ? 0 : HARDWARE_MODULE_TAG;
        module.common.id = invalid == 1 ? NULL :
                           invalid == 2 ? "not-gralloc" : GRALLOC_HARDWARE_MODULE_ID;
        module.lock = invalid == 3 ? NULL : unused_callback;
        module.unlock = invalid == 4 ? NULL : unused_callback;
        fallback = android_surface_create(&window);
        assert(fallback && fallback->anw == &window && !fallback->gralloc);
        free(fallback);
    }
    module.common.tag = HARDWARE_MODULE_TAG;
    module.common.id = GRALLOC_HARDWARE_MODULE_ID;
    module.lock = module.unlock = unused_callback;
    /* All legacy minor versions remain accepted, including version zero. */
    for (int minor = 0; minor <= 255; minor++) {
        module.common.module_api_version = minor;
        fallback = android_surface_create(&window);
        assert(fallback && fallback->gralloc == &module);
        free(fallback);
    }
    module.common.module_api_version = 1;
    /* A non-gralloc0 module has only a common header. Put it immediately before
       an inaccessible page: inspecting any gralloc0 callback would fault. */
    long page_size = sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    char *pages = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(pages != MAP_FAILED);
    assert(!mprotect(pages + page_size, page_size, PROT_NONE));
    hw_module_t *header = (hw_module_t *)(pages + page_size - sizeof(*header));
    *header = module.common;
    lookup_module = header;
    const uint16_t versions[] = { 0x0100, 0x0101, 0x0200, 0xffff };
    for (unsigned int i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        header->module_api_version = versions[i];
        fallback = android_surface_create(&window);
        assert(fallback && fallback->anw == &window && !fallback->gralloc);
        free(fallback);
    }
    lookup_module = &module.common;
    assert(!munmap(pages, page_size * 2));

    assert(!android_window_set_buffers_rotation(&window, 90));
    assert(rotations == 1 && last_transform == HAL_TRANSFORM_ROT_90);
    assert(!android_window_set_buffers_rotation(&window, 180));
    assert(rotations == 2 && last_transform == HAL_TRANSFORM_ROT_180);
    assert(!android_window_set_buffers_rotation(&window, 270));
    assert(rotations == 3 && last_transform == HAL_TRANSFORM_ROT_270);
    /* Reuse the same window: every rotated-to-zero transition must clear it. */
    const int angles[] = { 90, 180, 270 };
    const uint32_t transforms[] = {
        HAL_TRANSFORM_ROT_90, HAL_TRANSFORM_ROT_180, HAL_TRANSFORM_ROT_270
    };
    for (int i = 0; i < 3; i++) {
        int before = rotations;
        assert(!android_window_set_buffers_rotation(&window, angles[i]));
        assert(rotations == before + 1 && last_transform == transforms[i]);
        assert(!android_window_set_buffers_rotation(&window, 0));
        assert(rotations == before + 2 && last_transform == 0);
    }
    int before = rotations;
    assert(!android_window_set_buffers_rotation(&window, 0));
    assert(rotations == before + 1 && last_transform == 0);
    /* Both setting and clearing a transform must propagate window failures. */
    assert(!android_window_set_buffers_rotation(&window, 90));
    transform_error = -38;
    before = rotations;
    assert(android_window_set_buffers_rotation(&window, 180) == -38);
    assert(android_window_set_buffers_rotation(&window, 0) == -38);
    assert(rotations == before + 2 && last_transform == HAL_TRANSFORM_ROT_90);
    transform_error = 0;
    assert(!android_window_set_buffers_rotation(&window, 0));
    assert(last_transform == 0);
    /* Unsupported metadata keeps its existing no-op behavior. */
    before = rotations;
    assert(!android_window_set_buffers_rotation(&window, 45));
    assert(!android_window_set_buffers_rotation(&window, -90));
    assert(rotations == before);
    window.common.version = sizeof(ANativeWindow);
    android_surface_t *as = android_surface_create(&window);
    assert(as && as->gralloc == &module);
    free(as);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
