/*
 * stubs.c -- link-time stand-ins for the few symbols the subtitle engine pulls in from
 * elsewhere in the AVOS tree. Every definition here is WEAK: when a test also links the real
 * thing (sub_engine.c's sub_frame_unref(), debug.c's Debug[]), the real, strong definition
 * wins and nothing needs an #ifdef or a duplicate-symbol workaround.
 *
 * All of these are diagnostic logging or plain-free fallbacks and never affect the control
 * flow of the code under test, so a no-op is a behaviourally faithful stand-in, not just a
 * convenient one.
 *
 *   - Debug[]: real debug.c is normally linked (it is in the test tables). This copy is only
 *     a safety net; it is sized by debug.h's own DBG_* enum so it cannot drift out of sync.
 *
 *   - serprintf(): log.c is platform-split (log_avos.c wants a real TTY/console behind it,
 *     log_android.c needs the NDK), so neither is host-buildable. Always a no-op here.
 *
 *   - font_name_resolve_family(): the real one (font_name_parser.c) is FreeType-backed. It is
 *     only reached when SUB_FORMAT_OPEN_PARAMS::fonts_dir is set; the tests leave it NULL, so
 *     this exists purely to satisfy the linker and answers "not resolved", which is the right
 *     answer if it ever did run with no font folder behind it. Revisit if a test needs real
 *     font resolution.
 *
 *   - rst_to_ts_delta(): sub_engine.c's schedule_to_wall_ms() converts a backend's rst-time
 *     deadline into a wall-clock wait through the app's playback-speed mapping (util.h). The
 *     tests never change speed, so the identity (speed 1.0) is the right answer here; the
 *     speed conversion itself is NOT exercised. util.h is included so the compiler checks
 *     this definition against the real prototype.
 *
 *   - sub_frame_unref(): the real one lives in sub_engine.c and also knows the shared
 *     pixel_refs refcounting used by the GFX backend. The SSA backend never sets pixel_refs
 *     (plain malloc'd bitmaps per sub_types.h), so a plain free is faithful for the
 *     SSA-only test, which deliberately does not link sub_engine.c. This copy aborts if it is
 *     ever handed a pixel_refs-owned event instead of silently mishandling it; if SSA starts
 *     using pixel_refs, link the real sub_engine.c in that test.
 */
#include "sub_types.h"
#include "debug.h"
#include "util.h"   // rst_to_ts_delta()
#include <stdio.h>
#include <stdlib.h>

#define WEAK __attribute__((weak))

WEAK double rst_to_ts_delta(double rst_delta) {
    return rst_delta;
}

WEAK int Debug[DBG_MAX_ENTRIES];

WEAK int serprintf(const char *fmt, ...) {
    (void)fmt;
    return 0;
}

WEAK int font_name_resolve_family(const char *fonts_dir, const char *file_name,
                                  char *out_family, int out_family_size) {
    (void)fonts_dir; (void)file_name;
    if (out_family && out_family_size > 0) out_family[0] = '\0';
    return 0;
}

WEAK void sub_frame_unref(SUB_FRAME *frame) {
    if (!frame) return;
    for (SUB_EVENT *ev = frame->events; ev; ) {
        SUB_EVENT *next = ev->next;
        if (ev->kind == SUB_EVENT_BITMAP) {
            if (ev->pixel_refs) {
                fprintf(stderr,
                        "stubs.c: sub_frame_unref got a pixel_refs-owned event; this stand-in only "
                        "handles SSA's plain-malloc bitmaps. Link the real sub_engine.c instead.\n");
                abort();
            }
            free((void *)ev->data.bitmap.rgba);
        } else if (ev->kind == SUB_EVENT_TEXT) {
            free((void *)ev->data.text.utf8_text);
        }
        free(ev);
        ev = next;
    }
    free(frame);
}
