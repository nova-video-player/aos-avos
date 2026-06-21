/*
 * sub_format.h — Per-format backend interface.
 *
 * Every subtitle format (SRT, SSA/libass, VOBSUB, PGS) implements this
 * vtable. sub_engine.c dispatches to the right backend based on the
 * stream's detected format and drives it through the same lifecycle
 * regardless of format — mirrors the existing STREAM_DEC_SUB shape from
 * avos's old subtitle.c/stream_subtitle.c, but format backends now
 * produce SUB_FRAME instead of VIDEO_FRAME, decoupling them from the
 * old text/gfx VIDEO_FRAME dichotomy entirely.
 */

#pragma once

#include "sub_types.h"
#include "sub_style.h"

typedef struct SUB_FORMAT_BACKEND SUB_FORMAT_BACKEND;

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

typedef struct {
    int video_w, video_h;          /* known at open time, may be 0 if unknown
                                     * yet — backend must handle a later
                                     * resize() call                       */
    const uint8_t *codec_private;  /* e.g. ASS [Script Info]+[Styles] header,
                                     * or VOBSUB palette block               */
    int             codec_private_size;
    const SUB_USER_STYLE *user_style; /* read-only; backend snapshots what
                                        * it needs at open time, re-reads via
                                        * sub_style_snapshot() each frame if
                                        * it wants live updates              */
    int is_plain_text_format;        /* Tells the backend if this was converted from SRT/TXT */
} SUB_FORMAT_OPEN_PARAMS;

/*
 * open()    — one-time setup (e.g. ass_library_init / ass_new_track).
 * feed()    — push one demuxed subtitle packet. MUST NOT render or block
 *             on rendering; purely accumulates state.
 * render_at — produce a SUB_FRAME for the given PTS. Called from the
 *             render thread, potentially every display frame. Must be
 *             safe to call concurrently with feed() from another thread.
 * resize()  — video dimensions changed (rotation, track switch).
 * flush()   — seek occurred; discard buffered events.
 * close()   — full teardown.
 *
 * free_frame() — release a SUB_FRAME previously returned by render_at().
 *                Lets bitmap-backed backends (libass, VOBSUB, PGS) reuse
 *                internal buffers rather than churn malloc/free per call
 *                (addresses the per-frame alloc-thrash concern raised
 *                earlier with the Java-bitmap path).
 */
struct SUB_FORMAT_BACKEND {
    void *priv;

    int  (*open)      (SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *params);
    int  (*feed)       (SUB_FORMAT_BACKEND *be, const uint8_t *data, int size,
                        int64_t pts_ms, int64_t duration_ms);
    SUB_FRAME *(*render_at)(SUB_FORMAT_BACKEND *be, int64_t pts_ms);
    void (*free_frame)(SUB_FORMAT_BACKEND *be, SUB_FRAME *frame);
    int  (*resize)    (SUB_FORMAT_BACKEND *be, int video_w, int video_h);
    int  (*flush)     (SUB_FORMAT_BACKEND *be);
    int  (*close)     (SUB_FORMAT_BACKEND *be);
};

/* ------------------------------------------------------------------
 * Registration — each sub_format_*.c registers itself; sub_engine.c
 * picks the right one by SUB_FORMAT_ID at stream-open time. Mirrors
 * avos's existing STREAM_REGISTER_DEC_SUB macro pattern.
 * ------------------------------------------------------------------ */

typedef SUB_FORMAT_BACKEND *(*sub_format_factory_fn)(void);

void sub_format_register(SUB_FORMAT_ID id, sub_format_factory_fn factory, const char *name);
SUB_FORMAT_BACKEND *sub_format_create(SUB_FORMAT_ID id);

#define SUB_FORMAT_REGISTER(id, factory_fn, name) \
    /* call sub_format_register(id, factory_fn, name) from a constructor
     * or an explicit init list in sub_engine_init_formats.c — avoids
     * relying on __attribute__((constructor)) link-order surprises
     * across the four backend .c files */
