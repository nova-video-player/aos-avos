/*
 * font_name_parser.h — extracts real font family names via FreeType, the
 * same library libass uses internally for font loading and matching.
 *
 * WHY THIS EXISTS: a font's filename has no reliable relationship to its
 * actual family name. Confirmed via fc-scan on two real test fonts:
 *
 *   bahnschrift.ttf   -> family "Bahnschrift", but the file is a variable
 *                        font containing 15 DISTINCT NAMED INSTANCES under
 *                        that one family (Regular, Light, SemiBold, Bold,
 *                        several Condensed/SemiCondensed combinations...).
 *                        There is no string transform of "bahnschrift.ttf"
 *                        that enumerates those.
 *   Roboto-Medium.ttf -> real family is "Roboto" / "Roboto Medium" (name ID
 *                        16 vs name ID 1 disagree) -- "Roboto-Medium" (the
 *                        literal filename minus extension) is not a family
 *                        name this font, or libass, will ever match.
 *
 * Using FreeType instead of guessing means: (1) we read the SAME data
 * libass's own font backend reads (libass uses FreeType for font loading),
 * so a family name this module reports is guaranteed to be one libass's
 * fontselect can actually resolve; (2) FreeType already handles every
 * malformed/exotic real-world font libass itself has to tolerate, rather
 * than a hand-rolled parser reinventing that robustness; (3) variable font
 * named instances (FT_Set_Named_Instance, FreeType >= 2.9) are enumerated
 * properly instead of only ever seeing the file's default instance.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One resolved (family, style) pair -- either a distinct sub-font of a
// .ttc collection, or a distinct named instance of a variable font (e.g.
// one of bahnschrift.ttf's 15 instances). `family` is what should be
// registered with ass_add_font() / matched against ass_set_fonts()'s
// fallback -- it is FreeType's own face->family_name, i.e. exactly what
// libass's own font backend will see when it loads this same file.
typedef struct {
    char family[256];     // FreeType's face->family_name, UTF-8, NUL-terminated
    char style[256];      // FreeType's face->style_name (e.g. "Bold", "SemiCondensed Light")
    int  face_index;      // FT_New_Face() face_index this came from (.ttc sub-font; 0 for plain .ttf/.otf)
    int  named_instance;  // 0 = the face's default instance; > 0 = a specific named
                          // instance index within a variable font (see FreeType docs
                          // on FT_Set_Named_Instance -- the high 16 bits of the face
                          // index passed to FT_New_Face select the instance)
} FONT_NAME_ENTRY;

#define FONT_NAME_MAX_ENTRIES 32

typedef struct {
    FONT_NAME_ENTRY entries[FONT_NAME_MAX_ENTRIES];
    int count;
} FONT_NAME_RESULT;

typedef enum {
    FONT_NAME_OK = 0,
    FONT_NAME_ERR_INIT,          // FT_Init_FreeType() itself failed
    FONT_NAME_ERR_OPEN,          // FT_New_Face() failed to open/parse the file at all
                                 // (not a font FreeType recognizes, or unreadable)
    FONT_NAME_ERR_NO_FAMILY,     // face(s) opened fine but reported no usable family name
                                 // (exceptionally rare -- would indicate a badly malformed font)
} FONT_NAME_STATUS;

// Parses the font file at `path` (a plain filesystem path) using FreeType
// and fills `out` with every distinct (family, style) pair found -- across
// every sub-font of a .ttc collection, and every named instance of a
// variable font. For an ordinary single-style .ttf/.otf this yields exactly
// one entry.
//
// Returns FONT_NAME_OK if at least one entry was extracted (out->count > 0).
// Any other return value means out->count is 0 -- always check the return
// value rather than count alone.
//
// Owns and tears down its own FT_Library/FT_Face internally -- callers do
// not need any FreeType state of their own to use this function.
FONT_NAME_STATUS font_name_parse_file(const char *path, FONT_NAME_RESULT *out);

// Same as font_name_parse_file(), but for a font that's already resident in
// memory instead of sitting on disk -- e.g. a font blob extracted from an
// MKV AVMEDIA_TYPE_ATTACHMENT stream by stream_parser_ffmpeg.c (see av.h's
// ATTACHED_FONT), which is never written to a file, or any other caller that
// already has the raw bytes. Shares every bit of enumeration logic with
// font_name_parse_file() internally (both are thin wrappers around the same
// FT_Open_Face-based core, differing only in how they fill FT_Open_Args) --
// no need to spool the buffer out to a temp file first just to reuse the
// path-based API.
//
// `data`/`size` only need to stay valid for the duration of this call;
// nothing is retained past return. `debug_name` is used only for logging
// (e.g. the attachment's filename/title metadata) -- pass NULL or "" if
// nothing meaningful is available.
FONT_NAME_STATUS font_name_parse_memory(const uint8_t *data, int size, const char *debug_name, FONT_NAME_RESULT *out);

// Human-readable string for a FONT_NAME_STATUS, for logging.
const char *font_name_status_string(FONT_NAME_STATUS status);

// Resolves the REAL family name of the font file+selector `stored_value`
// found inside `fonts_dir`, via font_name_parse_file() -- i.e. reads the
// font's actual FreeType-parsed name table instead of guessing from the
// filename (see this header's top-of-file comment for why that guess is
// unreliable). `stored_value` is either a plain filename (meaning "this
// file's default/only entry") or "filename#faceIndex.namedInstance" (a
// specific entry, format matches FontNameParser.Entry.encodeSelector() on
// the Java side) -- exactly what VideoPreferencesCommon's Settings UI stores
// for the custom-fonts-folder default-font ListPreference.
//
// On success, writes the resolved family name into `out` (capacity
// `out_cap`) and returns 1. On any failure (fonts_dir/stored_value unset,
// file not found, not a font FreeType recognizes, or the selector doesn't
// match any entry the file actually contains -- in which case this falls
// back to the file's first available entry rather than fail outright)
// returns 0 and leaves `out` untouched; callers should fall back to a
// generic family like "sans-serif" rather than fail the whole track open
// over one unresolvable font.
//
// Shared by every backend that needs this (sub_format_ssa.c, sub_format_srt.c)
// instead of each keeping its own copy -- the logic is pure font_name_parser-
// facing utility code with no backend-specific state.
int font_name_resolve_family(const char *fonts_dir, const char *stored_value, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
