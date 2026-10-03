/*
 * Subtitle "kind": the single, native-owned answer to "what sort of subtitle
 * track is this?". Java reads it (SubtitleManager.kindFromNative()) and never
 * re-derives it from a format label or a gfx flag.
 *
 * Values are wire values, sent in *_METADATA_SUBTITLE_TRACK_KIND and mirrored
 * by SubtitleManager.KIND_*. Never renumber; only append.
 */
#ifndef SUB_KIND_H
#define SUB_KIND_H

typedef enum {
	SUB_KIND_NONE        = 0, /* no track / field absent; never sent for a real track */
	SUB_KIND_SSA         = 1, /* SSA/ASS: authored styles and PlayRes, rendered by libass */
	SUB_KIND_PLAIN_TEXT  = 2, /* SRT/VTT/mov_text...: free-form text, user styling applies */
	SUB_KIND_GRAPHIC     = 3, /* PGS / DVD / VobSub bitmaps: baked-in position and size */
	SUB_KIND_UNSUPPORTED = 4, /* recognised track, but no renderer for it */
} SUB_KIND;

/* Derived from sub_fmt_from_format(), so there is still exactly one table. */
SUB_KIND sub_kind_from_format(int sub_format /* SUB_FORMAT_* from av.h */);

#endif /* SUB_KIND_H */
