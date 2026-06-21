#include "font_name_parser.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H  // FT_Get_MM_Var, FT_Set_Named_Instance support

#include <string.h>
#include <stdio.h>
#include "debug.h"

#define DBG if(Debug[DBG_SUB])

// RIBBI = Regular/Bold/Italic/Bold Italic. For these, bare `family` is
// already the font's own name -- appending the style would often produce
// a string the font doesn't actually have in its name table.
static int is_ribbi_style(const char *style) {
    if (!style || !style[0]) return 1;
    return strcasecmp(style, "Regular") == 0 ||
           strcasecmp(style, "Bold") == 0 ||
           strcasecmp(style, "Italic") == 0 ||
           strcasecmp(style, "Bold Italic") == 0;
}

// For a named (non-RIBBI) instance like "Medium" or "SemiBold", `family`
// alone (e.g. "Roboto") can collide with an unrelated system font of the
// same name, since ASS_Style has no way to request a specific weight.
// "family style" (e.g. "Roboto Medium") reconstructs the font's own
// legacy full name instead, which is unique to this file.
static void build_requested_family(const char *family, const char *style, char *out, size_t out_cap) {
    if (is_ribbi_style(style)) {
        size_t len = strlen(family);
        if (len >= out_cap) len = out_cap - 1;
        memcpy(out, family, len);
        out[len] = '\0';
        return;
    }
    int n = snprintf(out, out_cap, "%s %s", family, style);
    if (n < 0) out[0] = '\0';
}

// Was previously duplicated near-verbatim as a static resolve_real_family_name()
// in both sub_format_ssa.c and sub_format_srt.c -- consolidated here since it's
// pure font_name_parser-facing utility logic (path/selector parsing + a
// font_name_parse_file() call) with no dependency on either backend's state.
int font_name_resolve_family(const char *fonts_dir, const char *stored_value, char *out, size_t out_cap) {
    if (!fonts_dir || !fonts_dir[0] || !stored_value || !stored_value[0]) return 0;

    // Split stored_value into filename + optional "faceIndex.namedInstance"
    // selector at the last '#' -- filenames themselves can't contain '#',
    // and Java's FontNameParser.Entry.encodeSelector() only ever appends one
    // '#', so finding the last one and treating everything after it as the
    // selector is unambiguous.
    char filename[512];
    int wanted_face = 0, wanted_instance = 0;
    const char *hash = strrchr(stored_value, '#');
    if (hash) {
        size_t name_len = (size_t)(hash - stored_value);
        if (name_len >= sizeof(filename)) name_len = sizeof(filename) - 1;
        memcpy(filename, stored_value, name_len);
        filename[name_len] = '\0';
        // Format after '#' is "faceIndex.namedInstance" -- tolerate a
        // malformed/unparseable suffix by falling back to (0, 0) rather
        // than failing resolution entirely over a cosmetic parse issue.
        sscanf(hash + 1, "%d.%d", &wanted_face, &wanted_instance);
    } else {
        size_t name_len = strlen(stored_value);
        if (name_len >= sizeof(filename)) name_len = sizeof(filename) - 1;
        memcpy(filename, stored_value, name_len);
        filename[name_len] = '\0';
    }

    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", fonts_dir, filename);
    if (n <= 0 || (size_t)n >= sizeof(path)) return 0;

    FONT_NAME_RESULT result;
    FONT_NAME_STATUS status = font_name_parse_file(path, &result);
    if (status != FONT_NAME_OK || result.count == 0) {
        DBG serprintf("SUB_FONTS: could not resolve real family name for '%s': %s\n",
             filename, font_name_status_string(status));
        return 0;
    }

    // Look for the exact (face_index, named_instance) the selector asked
    // for. If found, that's authoritative -- it's exactly what the user
    // saw and picked in the Settings list.
    for (int i = 0; i < result.count; i++) {
        if (result.entries[i].face_index == wanted_face &&
            result.entries[i].named_instance == wanted_instance &&
            result.entries[i].family[0] != '\0') {
            build_requested_family(result.entries[i].family, result.entries[i].style, out, out_cap);
            DBG serprintf("SUB_FONTS: resolved '%s' (face=%d instance=%d) -> real family '%s' (family='%s' style='%s')\n",
                 filename, wanted_face, wanted_instance, out, result.entries[i].family, result.entries[i].style);
            return 1;
        }
    }

    // Requested selector wasn't found (e.g. the font file was replaced with
    // a different one that no longer has that exact instance) -- fall back
    // to the first available entry rather than fail outright, since some
    // resolved font is better than silently reverting to the OS default for
    // what's likely a stale-but-close-enough stored value.
    DBG serprintf("SUB_FONTS: selector face=%d instance=%d not found in '%s' (has %d entries), "
         "falling back to first available entry\n", wanted_face, wanted_instance, filename, result.count);
    for (int i = 0; i < result.count; i++) {
        if (result.entries[i].family[0] != '\0') {
            build_requested_family(result.entries[i].family, result.entries[i].style, out, out_cap);
            return 1;
        }
    }
    return 0;
}

const char *font_name_status_string(FONT_NAME_STATUS status) {
    switch (status) {
        case FONT_NAME_OK:            return "OK";
        case FONT_NAME_ERR_INIT:      return "FreeType init failed";
        case FONT_NAME_ERR_OPEN:      return "could not open/parse font file";
        case FONT_NAME_ERR_NO_FAMILY: return "no usable family name found";
        default:                     return "unknown error";
    }
}

// Copies a FreeType-owned string (family_name/style_name -- both plain
// NUL-terminated char* per FreeType's API, already whatever encoding the
// font's chosen name-table entry used, which in practice is always ASCII
// or UTF-8-safe Latin text for real-world font family/style names) into a
// fixed-size destination, truncating safely rather than overflowing.
static void copy_bounded(char *dst, size_t dst_cap, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_cap) n = dst_cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Appends one (family, style) entry for the CURRENTLY LOADED state of
// `face` (whatever face_index/named-instance is active on it right now) --
// caller is responsible for having already selected the right state before
// calling this. Silently drops the entry if `out` is already full
// (FONT_NAME_MAX_ENTRIES) rather than overflow -- a font reporting more
// than 32 distinct (family, style) pairs is not a case worth crashing over,
// and 32 comfortably covers every real-world font this feature will
// realistically see (bahnschrift.ttf's 15 instances included).
static void append_entry(FONT_NAME_RESULT *out, FT_Face face, int face_index, int named_instance) {
    if (out->count >= FONT_NAME_MAX_ENTRIES) return;
    if (!face->family_name || !face->family_name[0]) return; // nothing usable to report

    FONT_NAME_ENTRY *e = &out->entries[out->count];
    copy_bounded(e->family, sizeof(e->family), face->family_name);
    copy_bounded(e->style, sizeof(e->style), face->style_name);
    e->face_index = face_index;
    e->named_instance = named_instance;
    out->count++;
}

// Enumerates every named instance of `face` (a variable font, e.g.
// bahnschrift.ttf's 15 weight/width instances), re-loading the face once
// per instance so FreeType re-resolves family_name/style_name for that
// specific instance rather than only ever reporting the file's default.
// If `face` isn't a variable font (the common case), this is a no-op --
// the caller already recorded the face's default state before calling in,
// via append_entry() at named_instance 0.
//
// Takes an FT_Open_Args* (rather than a path) so the SAME enumeration code
// drives both file-backed and memory-backed parses -- see parse_open_args()
// below, which is the only caller. Only the encoded face_index changes
// between calls; `args` itself (whatever FT_OPEN_PATHNAME/FT_OPEN_MEMORY
// source it describes) is reused unmodified.
static void enumerate_named_instances(FT_Library ft, FT_Open_Args *args, FT_Face default_face,
                                       int face_index, const char *debug_name, FONT_NAME_RESULT *out) {
    // FT_Get_MM_Var / MM_Var->num_namedstyles tells us how many named
    // instances exist. Absence of this data, or num_namedstyles == 0, means
    // "not a variable font (or no named instances)" -- not an error, just
    // nothing further to enumerate beyond what append_entry() already
    // recorded for the face's default state.
    FT_MM_Var *mm_var = NULL;
    if (FT_Get_MM_Var(default_face, &mm_var) != 0 || !mm_var) return;
    FT_UInt num_instances = mm_var->num_namedstyles;
    FT_Done_MM_Var(ft, mm_var);
    if (num_instances == 0) return;

    // Named instance N is selected by encoding it into the high 16 bits of
    // the face_index passed to FT_Open_Face -- per FreeType's own documented
    // convention: (instance_index << 16) | face_index. Instance indices
    // are 1-based in this encoding (0 means "no specific instance / default").
    for (FT_UInt i = 1; i <= num_instances; i++) {
        FT_Face instance_face = NULL;
        FT_Long combined_index = ((FT_Long)i << 16) | (FT_Long)face_index;
        if (FT_Open_Face(ft, args, combined_index, &instance_face) != 0 || !instance_face) {
            serprintf("enumerate_named_instances: failed to load named instance %u of face %d in '%s'\n",
                 i, face_index, debug_name ? debug_name : "?");
            continue;
        }
        append_entry(out, instance_face, face_index, (int)i);
        FT_Done_Face(instance_face);
    }
}

// Shared core: given an already-open FT_Library and an FT_Open_Args
// describing EITHER a filesystem path (FT_OPEN_PATHNAME) or an in-memory
// buffer (FT_OPEN_MEMORY), enumerates every (family, style) pair across
// every .ttc sub-font and every variable-font named instance -- identical
// logic either way, since FT_Open_Face doesn't care which source `args`
// describes. font_name_parse_file() and font_name_parse_memory() are both
// thin wrappers that just build the right `args` and call this.
// `debug_name` is a path or a caller-supplied label, purely for logging.
static FONT_NAME_STATUS parse_open_args(FT_Library ft, FT_Open_Args *args,
                                         const char *debug_name, FONT_NAME_RESULT *out) {
    if (!debug_name) debug_name = "?";

    // First open with face_index = -1: for a .ttc collection this returns
    // face->num_faces (how many distinct sub-fonts are bundled in the file)
    // without fully loading any single face -- the standard FreeType idiom
    // for discovering collection size before iterating it. For a plain
    // .ttf/.otf this just reports num_faces == 1.
    FT_Face probe_face = NULL;
    if (FT_Open_Face(ft, args, -1, &probe_face) != 0 || !probe_face) {
        serprintf("font_name_parse: probe failed for '%s' -- not a font FreeType recognizes, or unreadable\n", debug_name);
        return FONT_NAME_ERR_OPEN;
    }
    FT_Long num_faces = probe_face->num_faces;
    FT_Done_Face(probe_face);

    for (FT_Long face_index = 0; face_index < num_faces; face_index++) {
        FT_Face face = NULL;
        if (FT_Open_Face(ft, args, face_index, &face) != 0 || !face) {
            serprintf("font_name_parse: failed to open face %ld in '%s'\n", face_index, debug_name);
            continue; // one bad sub-font in a .ttc shouldn't take out the rest
        }

        // Record the face's own default state first (named_instance = 0),
        // THEN check for and enumerate additional named instances -- this
        // ordering means a variable font's "plain" resolved name (whatever
        // FreeType picks as the default instance) is always entries[0] for
        // that face, with any further named instances appended after it.
        append_entry(out, face, (int)face_index, 0);
        enumerate_named_instances(ft, args, face, (int)face_index, debug_name, out);

        FT_Done_Face(face);
    }

    if (out->count == 0) {
        serprintf("font_name_parse: '%s' opened but yielded no usable family name\n", debug_name);
        return FONT_NAME_ERR_NO_FAMILY;
    }

    for (int i = 0; i < out->count; i++) {
        DBG serprintf("font_name_parse: '%s' -> face=%d instance=%d family='%s' style='%s'\n",
             debug_name, out->entries[i].face_index, out->entries[i].named_instance,
             out->entries[i].family, out->entries[i].style);
    }
    return FONT_NAME_OK;
}

FONT_NAME_STATUS font_name_parse_file(const char *path, FONT_NAME_RESULT *out) {
    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) return FONT_NAME_ERR_OPEN;

    FT_Library ft;
    if (FT_Init_FreeType(&ft) != 0) {
        serprintf("font_name_parse_file: FT_Init_FreeType failed\n");
        return FONT_NAME_ERR_INIT;
    }

    FT_Open_Args args;
    memset(&args, 0, sizeof(args));
    args.flags = FT_OPEN_PATHNAME;
    args.pathname = (FT_String *)path;

    FONT_NAME_STATUS status = parse_open_args(ft, &args, path, out);
    FT_Done_FreeType(ft);
    return status;
}

FONT_NAME_STATUS font_name_parse_memory(const uint8_t *data, int size, const char *debug_name, FONT_NAME_RESULT *out) {
    memset(out, 0, sizeof(*out));
    if (!data || size <= 0) return FONT_NAME_ERR_OPEN;

    FT_Library ft;
    if (FT_Init_FreeType(&ft) != 0) {
        serprintf("font_name_parse_memory: FT_Init_FreeType failed\n");
        return FONT_NAME_ERR_INIT;
    }

    FT_Open_Args args;
    memset(&args, 0, sizeof(args));
    args.flags = FT_OPEN_MEMORY;
    args.memory_base = (const FT_Byte *)data;
    args.memory_size = (FT_Long)size;

    FONT_NAME_STATUS status = parse_open_args(ft, &args, debug_name, out);
    FT_Done_FreeType(ft);
    return status;
}
