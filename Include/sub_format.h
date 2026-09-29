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
    int video_w, video_h;          /* on-screen subtitle canvas size (letterbox bars
                                     * included), known at open time, may be 0 if unknown
                                     * yet — backend must handle a later resize() call.
                                     * NOT the decoded video's own size -- see
                                     * real_video_w/h below. */
    int real_video_w, real_video_h;  /* NEW: decoded video's own coded pixel size --
                                     * fixed for the track's lifetime, never touched by
                                     * resize(). Only meaningful to the GFX backend
                                     * (PGS/VobSub bitmap coordinates are expressed in
                                     * this space -- see codec_ffsub.c); SSA/SRT backends
                                     * can ignore it. 0 if genuinely unknown, in which
                                     * case the backend should fall back to video_w/h. */
    int video_box_x, video_box_y;    /* NEW: where the video's own on-screen box sits
                                     * within the canvas (video_w x video_h) -- e.g. the
                                     * visible video rect when the canvas was extended to
                                     * absorb letterbox bars. Only meaningful to GFX; 0
                                     * (with video_box_w/h below also 0) means "not yet
                                     * known, assume the video fills the canvas 1:1". */
    int video_box_w, video_box_h;
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
 * resize()  — canvas dimensions changed (rotation, track switch). Despite
 *             the param names below (kept as video_w/video_h for now), this
 *             is always the on-screen subtitle canvas size -- see
 *             sub_engine_resize_canvas() in sub_engine.c, its only caller.
 * flush()   — seek occurred; discard buffered events.
 * close()   — full teardown.
 *
 * set_video_box() — OPTIONAL (NULL for SSA/SRT). Reports where the video's
 *                own on-screen box sits within the canvas, independent of
 *                resize(): the canvas can resize without the box changing
 *                shape (e.g. a symmetric screen resize) and the box can
 *                change without the canvas resizing (e.g. a margins
 *                preference toggle). Only the GFX backend currently
 *                implements this -- see sub_engine_set_video_box() in
 *                sub_engine.c.
 *
 * free_frame() — release a SUB_FRAME previously returned by render_at().
 *                Lets bitmap-backed backends (libass, VOBSUB, PGS) reuse
 *                internal buffers rather than churn malloc/free per call
 *                (addresses the per-frame alloc-thrash concern raised
 *                earlier with the Java-bitmap path).
 */
/* ------------------------------------------------------------------
 * TIMING CONTRACT -- the single definition of "which clock" for the whole
 * subtitle pipeline. Everything below the stream layer works in RST.
 *
 *   RST  real stream time: media position in ms, the domain of the UI seek
 *        bar and of any external .srt/.ass/.idx file. It does NOT move when
 *        playback speed changes.
 *   TS   the player's internal time-scaled domain (ts = rst / speed). Its
 *        anchor is rewritten by timeline_map_apply() on every speed change,
 *        so a cue converted to TS once and held (an external file is fed in
 *        full at open; libass keeps its events; VobSub keeps a deadline)
 *        would silently go stale. TS therefore never crosses the engine
 *        boundary.
 *
 *   clock      sub_engine_clock_fn returns TS_TO_RST_TIME(video_time) minus
 *              the user's subtitle delay, in rst ms, or < 0 for "no clock
 *              yet" (seek/init). The delay is applied here and nowhere else.
 *              For external TEXT tracks it is additionally scaled by the
 *              user's subtitle ratio (t * d / n), so those cues are fed at
 *              raw file times and a ratio change is live.
 *   feed*()    pts_ms / duration_ms are rst ms and UNDELAYED. Callers that
 *              hold TS values (stream_subtitle.c) convert at the call site,
 *              at feed time. duration_ms: > 0 finite; 0 = clear (GFX only);
 *              < 0 = unknown / until the next cue.
 *   render_at  pts_ms is the clock above; a cue is visible iff
 *              start <= pts < start + duration.
 *   schedule   get_schedule() reports WHEN output can next change on its
 *              own, as an absolute rst deadline. sub_engine_wait_event()
 *              converts that to a wall-clock wait (delta_wc == delta_ts ==
 *              RST_TO_TS_DELTA(delta_rst)) against the speed in force at
 *              wait time, so backends never see speed.
 *
 * A delay change or speed change therefore needs no re-feed: the clock moves,
 * the engine is woken (sub_engine_force_wake), and every cue is re-evaluated
 * declaratively against the new clock.
 * ------------------------------------------------------------------ */

/* Filled by get_schedule(). */
typedef struct {
    int64_t next_rst_ms;  /* absolute rst time of the next moment the rendered
                           * output changes by itself (a cue starts or ends);
                           * -1 = nothing pending, sleep until woken. Always
                           * strictly greater than the pts of the render_at()
                           * call it describes. */
    int     animating;    /* nonzero: output changes continuously while this
                           * holds (libass fade / karaoke / scroll), so the
                           * engine ticks at ~16 ms of WALL time regardless of
                           * playback speed. */
} SUB_SCHEDULE;

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
    int  (*set_video_box)(struct SUB_FORMAT_BACKEND *be, int x, int y, int w, int h); /* NEW, optional */
    int  (*flush)     (SUB_FORMAT_BACKEND *be);
    int  (*close)     (SUB_FORMAT_BACKEND *be);
    /* Reports what should wake the render thread next (see SUB_SCHEDULE). Called
     * with eng->lock held, right after render_at(); must be cheap and must not
     * block. pts_rst_ms is the current clock, for backends that want it. */
    void (*get_schedule)(struct SUB_FORMAT_BACKEND *be, int64_t pts_rst_ms, SUB_SCHEDULE *out);
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
