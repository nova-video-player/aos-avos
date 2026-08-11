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
 * One font extracted from a container attachment (e.g. an MKV
 * AVMEDIA_TYPE_ATTACHMENT stream carrying an embedded .ttf/.otf/.ttc --
 * see stream_parser_ffmpeg.c's AVMEDIA_TYPE_ATTACHMENT handling and
 * av.h's ATTACHED_FONT, which this is bridged from by stream_subtitle.c).
 *
 * Deliberately a SEPARATE type from av.h's ATTACHED_FONT rather than
 * reusing it directly: this header must not depend on anything
 * ffmpeg/demux-shaped, mirroring how SUB_PROPERTIES (av.h) and
 * SUB_FORMAT_OPEN_PARAMS (this file) are already two independent shapes
 * bridged by hand at the call site -- codec_private below is exactly the
 * same pattern, populated from SUB_PROPERTIES::extraData2 by whichever
 * caller builds these params.
 *
 * `data` is NOT owned by the backend and is only guaranteed valid for the
 * duration of open() -- it aliases the demuxer's own AVCodecParameters
 * buffer, the same lifetime contract SUB_PROPERTIES::extraData2 already
 * relies on for codec_private above. Nothing here needs its own teardown.
 * ------------------------------------------------------------------ */
typedef struct {
    const char    *name;   // attachment filename (e.g. "arial.ttf"), used as
                            // ass_add_font()'s label and for logging; may be
                            // NULL/empty if the container didn't tag one
    const uint8_t *data;
    int            size;
} SUB_EMBEDDED_FONT;

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

    /* --- Custom fonts folder (MX Player / mpv-android style third-party
     * fonts dir) --- Both NULL/empty by default, meaning "feature off,
     * behave exactly as before" (fontconfig-only resolution, hardcoded
     * "sans-serif" fallback). Only the SSA backend currently reads these
     * (see ssa_open() in sub_format_ssa.c); other backends may ignore them.
     */
    const char *fonts_dir;          /* folder to scan for .ttf/.otf/.ttc,
                                      * registered with libass via
                                      * ass_add_font() BEFORE fontconfig gets
                                      * a chance to resolve anything          */
    const char *default_font_name;  /* fallback family name libass uses when
                                      * nothing else names a font — notably
                                      * what plain-text (SRT/VTT) subtitles
                                      * render with, since they carry no font
                                      * info of their own                     */

    /* --- Container-embedded fonts (e.g. MKV AVMEDIA_TYPE_ATTACHMENT
     * streams) --- NULL/0 by default, meaning "none found/not applicable".
     * See SUB_EMBEDDED_FONT above for the array element shape and lifetime
     * contract. Only the SSA backend currently reads this (see
     * load_embedded_fonts() in sub_format_ssa.c); other backends may ignore
     * it. Registered with libass via ass_add_font() the same way fonts_dir
     * is, just from memory instead of a directory scan.
     */
    const SUB_EMBEDDED_FONT *embedded_fonts;
    int                      embedded_fonts_count;
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
    int (*feed_bitmap)(struct SUB_FORMAT_BACKEND *be, uint8_t *pixels, int width,
                       int height, int pitch, int colorspace, int x_offset, int y_offset,
                       int64_t pts_ms, int64_t duration_ms);
    SUB_FRAME *(*render_at)(SUB_FORMAT_BACKEND *be, int64_t pts_ms);
    void (*free_frame)(SUB_FORMAT_BACKEND *be, SUB_FRAME *frame);
    int  (*resize)    (SUB_FORMAT_BACKEND *be, int video_w, int video_h);
    int  (*flush)     (SUB_FORMAT_BACKEND *be);
    int  (*close)     (SUB_FORMAT_BACKEND *be);
    int (*get_timeout_ms)(struct SUB_FORMAT_BACKEND *be, int64_t pts_ms);
};

/* ------------------------------------------------------------------
 * Registration — each sub_format_*.c registers itself; sub_engine.c
 * picks the right one by SUB_FMT_ID (SUB_FMT_SRT/SSA/GFX — the engine
 * backend selector defined in sub_types.h, NOT av.h's 12-value
 * SUB_FORMAT_* codec/container enum) at stream-open time. Mirrors
 * avos's existing STREAM_REGISTER_DEC_SUB macro pattern.
 * ------------------------------------------------------------------ */

typedef SUB_FORMAT_BACKEND *(*sub_format_factory_fn)(void);

void sub_format_register(SUB_FMT_ID id, sub_format_factory_fn factory, const char *name);
SUB_FORMAT_BACKEND *sub_format_create(SUB_FMT_ID id);

/* ------------------------------------------------------------------
 * sub_fmt_from_format() — the SINGLE canonical mapping from a track's
 * demux/codec format (av.h's SUB_FORMAT_* — SUB_FORMAT_SSA,
 * SUB_FORMAT_PGS, SUB_FORMAT_WEBVTT, etc.) to the engine backend that
 * should render it (SUB_FMT_ID). Every call site that needs to turn a
 * SUB_FORMAT_* value into a SUB_FMT_ID — internal tracks, external
 * tracks, ffdec bitmap tracks — must go through this function instead
 * of re-deriving the mapping locally. Returns SUB_FMT_UNKNOWN for any
 * SUB_FORMAT_* value that isn't (yet) routed to the C engine.
 * ------------------------------------------------------------------ */

SUB_FMT_ID sub_fmt_from_format(int sub_format_id);

#define SUB_FORMAT_REGISTER(id, factory_fn, name) \
    /* call sub_format_register(id, factory_fn, name) from a constructor
     * or an explicit init list in sub_engine_init_formats.c — avoids
     * relying on __attribute__((constructor)) link-order surprises
     * across the four backend .c files */
