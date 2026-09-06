/*
 * sub_types.h — Core data types shared across the entire subtitle engine.
 *
 * Every format backend (SRT, SSA/libass, VOBSUB, PGS) normalizes its
 * output into SUB_FRAME before handing it to the renderer. This is the
 * single seam the whole redesign pivots on: one renderer, N format
 * backends, all producing the same intermediate shape.
 */

#pragma once

#include <stdint.h>
#include <stdatomic.h>

/* ------------------------------------------------------------------
 * Color / style primitives
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t r, g, b, a;
} SUB_COLOR;

/* Text styling — applies to glyph-run events (SRT, plain SSA dialogue
 * when not using libass's own override tags; also the basis for user
 * override settings that get merged on top of whatever the format
 * specifies, see sub_style.h). */
typedef struct {
    SUB_COLOR   fg;             /* glyph fill color                     */
    SUB_COLOR   bg;             /* background box color (if bg_enabled) */
    SUB_COLOR   outline;        /* outline/border color                 */
    float       font_size_pt;   /* nominal size, scaled by renderer      */
    float       outline_width;  /* px, 0 = no outline                    */
    float       bg_opacity;     /* 0..1, independent of bg.a for UI convenience */
    int         bg_enabled;     /* draw background box behind text       */
    int         bold;
    int         italic;
    char        font_family[64];/* used by text backends; ignored by libass
                                  * (libass owns its own font selection) */
} SUB_STYLE;

/* ------------------------------------------------------------------
 * SUB_EVENT — one renderable "thing" at a point in time.
 *
 * Two kinds:
 *   SUB_EVENT_TEXT   — glyph run(s) to be shaped/rasterized by the
 *                       renderer itself (SRT, plain-text SSA fallback).
 *                       The renderer owns font shaping for this kind.
 *   SUB_EVENT_BITMAP — pre-rendered RGBA pixels, already positioned.
 *                       Used by libass (ass_render_frame output),
 *                       VOBSUB, and PGS. The renderer just uploads and
 *                       draws a textured quad — no shaping involved.
 * ------------------------------------------------------------------ */

typedef enum {
    SUB_EVENT_TEXT   = 0,
    SUB_EVENT_BITMAP = 1,
} SUB_EVENT_KIND;

typedef struct SUB_EVENT {
    SUB_EVENT_KIND kind;

    /* Placement, in video-frame pixel space (renderer maps to surface space) */
    int x, y, w, h;

    union {
        struct {
            const char *utf8_text;   /* owned by the event, freed with it */
            SUB_STYLE   style;       /* resolved style: format defaults
                                       * merged with user settings        */
        } text;

        struct {
            const uint8_t *rgba;     /* tightly packed, w*h*4 bytes        */
            int             stride;   /* bytes per row (may be > w*4)      */
            /* Ownership: renderer uploads synchronously and does not
             * retain this pointer past the call to sub_engine_submit_frame(). */
        } bitmap;
    } data;

    struct SUB_EVENT *next;  /* multiple simultaneous events per frame (e.g.
                               * top+bottom lines, or multi-region VOBSUB) */
} SUB_EVENT;

/* ------------------------------------------------------------------
 * SUB_FRAME — everything to display at a given PTS.
 * Produced by a format backend, consumed by the renderer.
 * ------------------------------------------------------------------ */

typedef struct {
    _Atomic int refcount;
    int64_t     pts_ms;
    int64_t     duration_ms;   /* renderer hint only; for animated formats
                                 * (libass) this may be a short "valid until
                                 * next submit" window rather than the cue's
                                 * full duration                            */
    int         video_w;       /* reference frame size events are placed in */
    int         video_h;
    SUB_EVENT  *events;        /* linked list, NULL = nothing to show       */
} SUB_FRAME;

/* ------------------------------------------------------------------
 * SUB_FMT_ID — engine backend selector. NOT the same thing as av.h's
 * SUB_FORMAT_* (SUB_FORMAT_SSA, SUB_FORMAT_PGS, SUB_FORMAT_WEBVTT, ...),
 * which identifies the on-disk/container codec format (12 values, used
 * for demux dispatch and codec_ffsub decoder selection). SUB_FMT_ID is
 * the much smaller set of engine backends those 12 formats collapse
 * onto (3 values) — e.g. SUB_FORMAT_WEBVTT, SUB_FORMAT_MOV_TEXT, and
 * SUB_FORMAT_TEXT are all plain text and all map to SUB_FMT_SRT.
 * There is no numeric relationship between the two enums; the mapping
 * from SUB_FORMAT_* to SUB_FMT_ID is semantic and lives in exactly one
 * place: sub_fmt_from_format() in sub_format.h/.c. Do not re-derive it
 * ad hoc at call sites.
 * ------------------------------------------------------------------ */

typedef enum {
    SUB_FMT_SRT     = 0,
    SUB_FMT_SSA     = 1,   /* via libass */
    SUB_FMT_GFX     = 2,   /* NEW: Universal OpenGL Bitmap Backend */
    SUB_FMT_UNKNOWN = -1,
} SUB_FMT_ID;
