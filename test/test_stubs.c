/*
 * test_stubs.c — link-time stand-ins for the handful of symbols
 * sub_format_ssa.c pulls in from elsewhere in the AVOS tree.
 *
 *   - Debug[] / serprintf()   : REAL debug.c + log.c are used instead
 *     whenever run-ssa-style-cache.sh finds them in Source/ (the normal
 *     case) -- this file's no-op versions, guarded by
 *     SSA_TEST_STUB_SKIP_DEBUG_LOG, only kick in as a fallback if those
 *     files aren't present. Either way these are diagnostic logging only
 *     and never affect sub_format_ssa.c's control flow, so a no-op is a
 *     behaviorally faithful fallback, not just a convenient one.
 *
 *   - font_name_resolve_family() : real implementation lives in
 *     font_name_parser.c (FreeType-backed). Only called when
 *     SUB_FORMAT_OPEN_PARAMS::fonts_dir is set (see ssa_open()); this test
 *     leaves fonts_dir NULL, so the call is never reached at runtime --
 *     this stub exists purely to satisfy the linker and always reports
 *     "not resolved", which is the correct behavior if it ever WERE called
 *     with no real font folder behind it. Not switched to a real/stub
 *     toggle like Debug[]/serprintf above because font_name_parser.c pulls
 *     in FreeType, a much heavier dependency for a code path this test
 *     deliberately never exercises -- revisit if a future test needs the
 *     real font-resolution behavior.
 *
 *   - sub_frame_unref() : real implementation lives in sub_engine.c and
 *     additionally knows about the shared pixel_refs refcounting scheme
 *     (see sub_types.h's SUB_EVENT::pixel_refs doc comment) used by the GFX
 *     backend. sub_format_ssa.c's frames never set pixel_refs (SSA bitmaps
 *     are always plain malloc'd per-event, per sub_types.h), so a
 *     plain-free unref is faithful for exactly what this test exercises --
 *     it deliberately does NOT implement the shared-refcount path, so if a
 *     future change makes the SSA backend start using pixel_refs, this
 *     stub's simplifications will need revisiting (or this test should link
 *     the real sub_frame_unref() instead). Not switched to a real/stub
 *     toggle because sub_engine.c is a large file that would pull this
 *     backend-isolation test into the rest of the engine's dependencies.
 *
 * Everything here is intentionally the simplest correct implementation for
 * THIS test's call pattern, not a general-purpose fake of the real
 * subsystems.
 */
#include "sub_types.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Debug[] and serprintf() are tracked independently now, since this tree's
 * debug.c and log.c don't necessarily arrive together (log.c here is
 * platform-split into log_avos.c/log_android.c, and only the latter is
 * usually a fit for a host build -- see run-ssa-style-cache.sh's log_src
 * comment for why serprintf() defaults to this stub even when debug.c is
 * real). Either way, sub_format_ssa.c's DBG serprintf(...) calls are
 * logging only and never gate control flow, so the stub versions are
 * behaviorally equivalent to the real ones for what this test checks. */
#ifndef SSA_TEST_STUB_SKIP_DEBUG_DEF
/* debug.h declares this as `extern int Debug[DBG_MAX_ENTRIES]` (non-release)
 * -- sized by whatever debug.h's DBG_* enum currently has, terminated by
 * DBG_MAX_ENTRIES. We don't have (and don't want) a dependency on that
 * exact enum here, but debug.h is on the include path when sub_format_ssa.c
 * compiles, so let it size the array for us instead of guessing a number
 * that could silently drift out of sync with a future debug.h edit. */
#include "debug.h"
int Debug[DBG_MAX_ENTRIES];
#endif /* SSA_TEST_STUB_SKIP_DEBUG_DEF */

#ifndef SSA_TEST_STUB_SKIP_SERPRINTF
int serprintf(const char *fmt, ...) {
    (void)fmt;
    return 0;
}
#endif /* SSA_TEST_STUB_SKIP_SERPRINTF */

int font_name_resolve_family(const char *fonts_dir, const char *file_name,
                             char *out_family, int out_family_size) {
    (void)fonts_dir; (void)file_name;
    if (out_family && out_family_size > 0) out_family[0] = '\0';
    return 0; /* "not resolved" -- correct answer if this ever actually ran */
                             }

                             void sub_frame_unref(SUB_FRAME *frame) {
                                 if (!frame) return;
                                 /* SSA frames never set pixel_refs (see file header comment above) --
                                  * assert that assumption instead of silently mishandling it if it
                                  * ever stops being true. */
                                 for (SUB_EVENT *ev = frame->events; ev; ) {
                                     SUB_EVENT *next = ev->next;
                                     if (ev->kind == SUB_EVENT_BITMAP) {
                                         if (ev->pixel_refs) {
                                             fprintf(stderr,
                                                     "test_stubs: sub_frame_unref got a pixel_refs-owned event; "
                                                     "this stub only handles SSA's plain-malloc bitmaps. Link "
                                                     "the real sub_engine.c sub_frame_unref() instead.\n");
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
