#include "sub_format.h"
#include "sub_style.h"
#include <ass/ass.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "debug.h"

#define DBG if(Debug[DBG_SUB])

typedef struct {
    double FontSize;
    char *FontName;
    int Bold;
    uint32_t PrimaryColour;
    uint32_t OutlineColour;
    uint32_t BackColour;
    int BorderStyle;
    double Outline;
    double Shadow;
    int MarginV;
} ASS_Style_Backup;

typedef struct {
    ASS_Library    *library;
    ASS_Renderer   *renderer;
    ASS_Track      *track;
    pthread_mutex_t lock;
    int             video_w, video_h;  // What ass_set_frame_size() was called with. Java now
                                        // decides what this should be per-format (full surface
                                        // for plain text, tethered to the video's own on-screen
                                        // box for embedded ASS/SSA) via mSubtitleView's own
                                        // layout size/position -- resize() just applies whatever
                                        // it's given, always.

    // --- LIVE SYNC VARIABLES ---
    const SUB_USER_STYLE *user_style_ptr;
    int               is_plain_text;
    ASS_Style_Backup *backups;
    int               num_backups;
    uint32_t          last_serial;

    int               orig_playres_x;
    int               orig_playres_y;

    // Persisted from SUB_FORMAT_OPEN_PARAMS at open() time so sync_styles()
    // (which re-runs on every feed()/style-change, not just at open) can
    // keep using them. fonts_dir is a private owned copy (snapshotted the
    // same way params->fonts_dir itself is only guaranteed valid for the
    // duration of the open() call -- see sub_engine.c's open_track(), which
    // frees its own local copy right after open() returns). resolved_default_family
    // is the REAL family name (via FreeType, see font_name_resolve_family())
    // of whatever font the user picked as their fonts-folder default -- resolved
    // ONCE at open() time and cached here, since re-parsing the font file on
    // every single sync_styles() call (which can run once per subtitle line)
    // would be wasteful for something that cannot change mid-track.
    char             *fonts_dir;
    char              resolved_default_family[256];
} SSA_BACKEND;

// --- CUSTOM FONTS FOLDER (MX Player / mpv-android style) + MKV-EMBEDDED FONTS ---
//
// libass resolves fonts through whatever providers you register with it, tried
// in registration order: (1) fonts added via ass_add_font() -- matched by the
// font's own embedded family name -- then (2) the system fontconfig provider
// passed to ass_set_fonts() below. So "a third font folder libass checks
// before fontconfig" is just: read every .ttf/.otf/.ttc file in the user's
// folder into memory and ass_add_font() it on the SAME ASS_Library our
// renderer uses, before the first ass_set_fonts() call. No fontconfig
// XML/cache changes, no per-app font DB -- purely libass side, so it can be
// redone per track open with zero system-wide side effects.
//
// Fonts an MKV actually ships internally (AVMEDIA_TYPE_ATTACHMENT streams --
// harvested by stream_parser_ffmpeg.c, see av.h's ATTACHED_FONT) go through
// this exact same ass_add_font() registration path -- see
// load_embedded_fonts() below -- just sourced from memory (the demuxer
// already has the bytes) instead of a disk read. Both sources share the same
// register_font_blob() choke point so there's exactly one place that talks
// to ass_add_font() and to font_name_parser, regardless of where the font
// blob came from.
#include <dirent.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>

#include "font_name_parser.h"

static int has_font_ext(const char *name) {
    size_t len = strlen(name);
    const char *exts[] = { ".ttf", ".otf", ".ttc", ".TTF", ".OTF", ".TTC" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t elen = strlen(exts[i]);
        if (len > elen && strcmp(name + len - elen, exts[i]) == 0) return 1;
    }
    return 0;
}

// Real-family resolution for the custom-fonts-folder default-font selector
// now lives in font_name_parser.c as font_name_resolve_family() -- it used
// to be duplicated here nearly verbatim (and again in sub_format_srt.c).
// See font_name_parser.h for the full contract.

// Registers ONE in-memory font blob with libass. This is the single choke
// point both font sources funnel through -- load_fonts_dir() (disk files
// from the custom fonts folder) and load_embedded_fonts() (MKV attachment
// fonts, already in memory courtesy of stream_parser_ffmpeg.c -- see av.h's
// ATTACHED_FONT) -- so registration behavior/logging can't drift between
// the two call sites.
//
// Does NOT run the blob through font_name_parser.h here: an earlier version
// did, purely for logD-level diagnostic visibility, but that meant a second
// full FreeType parse of every single font on every single ssa_open() (every
// video load, every track switch) for a value ass_msg_cb() below already
// surfaces for free -- libass does its OWN independent FreeType parse the
// first time it actually needs to match a family, and ass_msg_cb() was
// widened specifically to log that (look for "LIBASS[..] SUB_FONTS:" once
// rendering starts). Re-deriving the same family/style ourselves here was
// pure redundant CPU work on the track-open critical path for no additional
// information.
static void register_font_blob(ASS_Library *lib, const char *label, const uint8_t *data, int size) {
    if (!lib || !data || size <= 0) return;
    // ass_add_font() copies the data internally, so the caller's buffer can
    // be freed/reused immediately after this call regardless of source.
    ass_add_font(lib, label, (char *)data, size);
    DBG serprintf("SUB_FONTS: registered '%s' (%d bytes) with libass\n", label, size);
}

// --- In-process cache of the custom fonts folder's file contents ---------
//
// ass_library_init() runs fresh in every single ssa_open() call (see
// ctx->library below) -- every video load, every track switch, every
// SRT-wrapped open included -- so ass_add_font() genuinely has to run again
// every time; there's no way to skip that part short of keeping one
// long-lived ASS_Library across track opens, a much bigger change. What
// doesn't need to rerun every time is the DISK I/O: without this cache,
// load_fonts_dir() did a full opendir()/readdir()/fopen()/fread() pass over
// every file in the folder on every single open, even when nothing in the
// folder had changed since the last one -- real, avoidable latency sitting
// directly on the track-open critical path (i.e. on the "video takes a
// moment to actually start playing" path), for a folder that in the common
// case never changes between one video and the next.
//
// This cache holds the last-read file contents in memory (this process's
// lifetime only -- not persisted, not shared with the Java-side
// SubtitleFontsFolderSync cache dir, just a copy of its contents) and is
// reused as long as a cheap directory signature (entry count + summed
// mtime+size) still matches what's on disk -- same idea
// VideoPreferencesCommon.needsResync() uses on the SAF side, applied here
// to the native side's own repeat-open case.
#define FONTS_DIR_CACHE_MAX 64

typedef struct {
    char     name[256];
    uint8_t *data;
    int      size;
} CACHED_FONT_FILE;

static pthread_mutex_t   s_fonts_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static char              s_fonts_cache_dir[1024];
static long long         s_fonts_cache_signature;
static int               s_fonts_cache_valid;
static CACHED_FONT_FILE  s_fonts_cache[FONTS_DIR_CACHE_MAX];
static int               s_fonts_cache_count;

static void fonts_cache_clear_locked(void) {
    for (int i = 0; i < s_fonts_cache_count; i++) {
        free(s_fonts_cache[i].data);
        s_fonts_cache[i].data = NULL;
    }
    s_fonts_cache_count = 0;
    s_fonts_cache_valid = 0;
}

// Entry count + summed (mtime + size) across every font file in `dir` --
// cheap (one readdir() + stat() pass, no file content reads), good enough
// to catch the common cases (font added/removed/replaced) without hashing
// contents. Returns -1 if `dir` can't be opened at all.
static long long compute_dir_signature(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;

    long long sig = 0;
    struct dirent *entry;
    char path[1024];
    struct stat st;

    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!has_font_ext(entry->d_name)) continue;
        int n = snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;
        if (stat(path, &st) != 0) continue;
        sig += (long long)st.st_mtime + (long long)st.st_size;
    }
    closedir(d);
    return sig;
}

// Reads every font file in `dir` into memory (or reuses the cache from a
// previous call if the directory signature hasn't changed) and registers
// each one via register_font_blob(). Returns the number of fonts
// registered. Best-effort: unreadable files are skipped, not fatal -- a
// single corrupt or permission-denied font shouldn't take out every other
// font in the folder or the whole track open.
static int load_fonts_dir(ASS_Library *lib, const char *dir) {
    if (!lib || !dir || !dir[0]) return 0;

    pthread_mutex_lock(&s_fonts_cache_lock);

    long long sig = compute_dir_signature(dir);
    int cache_hit = s_fonts_cache_valid && sig >= 0 &&
                     sig == s_fonts_cache_signature &&
                     strcmp(s_fonts_cache_dir, dir) == 0;

    if (!cache_hit) {
        // Cache miss (first call for this dir, dir changed, or folder
        // contents changed since last time) -- do the real disk scan and
        // repopulate the cache, same logic the old unconditional version
        // always ran.
        fonts_cache_clear_locked();

        DIR *d = opendir(dir);
        if (!d) {
            DBG serprintf("SUB_FONTS: could not open fonts dir '%s'\n", dir);
            pthread_mutex_unlock(&s_fonts_cache_lock);
            return 0;
        }

        struct dirent *entry;
        char path[1024];

        while ((entry = readdir(d)) != NULL && s_fonts_cache_count < FONTS_DIR_CACHE_MAX) {
            if (entry->d_name[0] == '.') continue;           // skip ".", "..", hidden files
            if (!has_font_ext(entry->d_name)) continue;

            int n = snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
            if (n <= 0 || (size_t)n >= sizeof(path)) continue;

            FILE *f = fopen(path, "rb");
            if (!f) {
                DBG serprintf("SUB_FONTS: failed to open '%s'\n", path);
                continue;
            }

            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (size <= 0) { fclose(f); continue; }

            uint8_t *buf = malloc((size_t)size);
            if (!buf) { fclose(f); continue; }

            size_t rd = fread(buf, 1, (size_t)size, f);
            fclose(f);
            if (rd != (size_t)size) { free(buf); continue; }

            CACHED_FONT_FILE *slot = &s_fonts_cache[s_fonts_cache_count++];
            strncpy(slot->name, entry->d_name, sizeof(slot->name) - 1);
            slot->name[sizeof(slot->name) - 1] = '\0';
            slot->data = buf;
            slot->size = (int)size;
        }
        closedir(d);

        strncpy(s_fonts_cache_dir, dir, sizeof(s_fonts_cache_dir) - 1);
        s_fonts_cache_dir[sizeof(s_fonts_cache_dir) - 1] = '\0';
        s_fonts_cache_signature = sig;
        s_fonts_cache_valid = 1;

        DBG serprintf("SUB_FONTS: (re)scanned '%s' from disk -- %d font file(s) cached\n", dir, s_fonts_cache_count);
    }

    int loaded = 0;
    for (int i = 0; i < s_fonts_cache_count; i++) {
        register_font_blob(lib, s_fonts_cache[i].name, s_fonts_cache[i].data, s_fonts_cache[i].size);
        loaded++;
    }

    pthread_mutex_unlock(&s_fonts_cache_lock);

    DBG serprintf("SUB_FONTS: loaded %d font(s) from custom fonts folder '%s' (%s)\n",
         loaded, dir, cache_hit ? "cache hit, no disk I/O" : "freshly scanned");
    return loaded;
}

// Registers every font extracted from container attachments (e.g. MKV
// AVMEDIA_TYPE_ATTACHMENT streams collected by stream_parser_ffmpeg.c's
// _parse_format() into av.h's AV_PROPERTIES::font[], and bridged down to
// here via SUB_FORMAT_OPEN_PARAMS::embedded_fonts by stream_subtitle.c and
// sub_engine.c) with libass, through the SAME register_font_blob() path
// load_fonts_dir() uses above.
//
// Unlike load_fonts_dir(), there is no disk I/O here -- the data is already
// resident in memory (aliased from the demuxer's own AVCodecParameters
// buffer; see SUB_EMBEDDED_FONT's doc comment in sub_format.h for the
// lifetime contract this relies on), so entries are handed straight to
// register_font_blob() with no read/buffer/free step of our own.
static int load_embedded_fonts(ASS_Library *lib, const SUB_EMBEDDED_FONT *fonts, int count) {
    if (!lib || !fonts || count <= 0) return 0;

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        if (!fonts[i].data || fonts[i].size <= 0) continue;
        const char *label = (fonts[i].name && fonts[i].name[0]) ? fonts[i].name : "embedded-font";
        register_font_blob(lib, label, fonts[i].data, fonts[i].size);
        loaded++;
    }
    DBG serprintf("SUB_FONTS: loaded %d font(s) embedded in container attachments\n", loaded);
    return loaded;
}

// Synchronizes Java UI changes safely without corrupting the original ASS track!
static void sync_styles(SSA_BACKEND *ctx) {
    if (!ctx->user_style_ptr || !ctx->track) return;

    SUB_USER_STYLE u;
    memset(&u, 0, sizeof(SUB_USER_STYLE));
    sub_style_snapshot(ctx->user_style_ptr, &u);
    // sub_style_snapshot() deep-copies font_family into a fresh strdup'd buffer that this
    // function now owns — every exit path below must free(u.font_family) exactly once.
    // See sub_style.c's sub_style_snapshot() doc comment for why a shallow pointer copy
    // isn't safe here (use-after-free window against a concurrent setter call).

    if (u.serial == ctx->last_serial) {
        free(u.font_family);
        return;
    }
    ctx->last_serial = u.serial;

    // 1. Create Backups of the Original ASS Styles.
    // IMPORTANT: this must run on every call, not just the first, because ass_process_data()
    // (called from ssa_feed() for every incoming line) can itself add NEW style definitions
    // to the track mid-stream if the source delivers its [V4+ Styles] section incrementally
    // or the container hands off the header in fragments. If we only ever snapshotted once
    // (the old behavior), any style that appeared after that first snapshot had no backup
    // entry: the restore loop below would correctly skip it (bounded by num_backups) but
    // the apply loop further down is bounded by the CURRENT n_styles, so such a style would
    // keep receiving the user's forced overrides forever with no way to restore its actual
    // authored values — e.g. switching to Embedded mode later would silently fail to revert
    // that particular style.
    if (ctx->track->n_styles > ctx->num_backups) {
        int old_count = ctx->num_backups;
        int new_count = ctx->track->n_styles;
        ctx->backups = realloc(ctx->backups, new_count * sizeof(ASS_Style_Backup));
        memset(ctx->backups + old_count, 0, (new_count - old_count) * sizeof(ASS_Style_Backup));
        for (int i = old_count; i < new_count; i++) {
            ASS_Style *s = &ctx->track->styles[i];
            ctx->backups[i].FontSize = s->FontSize;
            ctx->backups[i].FontName = s->FontName ? strdup(s->FontName) : NULL;
            ctx->backups[i].Bold = s->Bold;
            ctx->backups[i].PrimaryColour = s->PrimaryColour;
            ctx->backups[i].OutlineColour = s->OutlineColour;
            ctx->backups[i].BackColour = s->BackColour;
            ctx->backups[i].BorderStyle = s->BorderStyle;
            ctx->backups[i].Outline = s->Outline;
            ctx->backups[i].Shadow = s->Shadow;
            ctx->backups[i].MarginV = s->MarginV;
        }
        ctx->num_backups = new_count;
    }

    if (ctx->orig_playres_x == 0 && ctx->track->PlayResX > 0) {
        ctx->orig_playres_x = ctx->track->PlayResX;
        ctx->orig_playres_y = ctx->track->PlayResY;
    }

    // 2. Restore everything to original ASS baseline first
    if (ctx->backups) {
        for (int i = 0; i < ctx->num_backups && i < ctx->track->n_styles; i++) {
            ASS_Style *s = &ctx->track->styles[i];
            s->FontSize = ctx->backups[i].FontSize;
            if (s->FontName) free(s->FontName);
            s->FontName = ctx->backups[i].FontName ? strdup(ctx->backups[i].FontName) : NULL;
            s->Bold = ctx->backups[i].Bold;
            s->PrimaryColour = ctx->backups[i].PrimaryColour;
            s->OutlineColour = ctx->backups[i].OutlineColour;
            s->BackColour = ctx->backups[i].BackColour;
            s->BorderStyle = ctx->backups[i].BorderStyle;
            s->Outline = ctx->backups[i].Outline;
            s->Shadow = ctx->backups[i].Shadow;
            s->MarginV = ctx->backups[i].MarginV;
        }

        if (ctx->orig_playres_x > 0) {
            ctx->track->PlayResX = ctx->orig_playres_x;
            ctx->track->PlayResY = ctx->orig_playres_y;
        }
    }

    // 3. Apply the Java Overrides!
    // 0 = ASS_OVERRIDE_NO, 1 = ASS_OVERRIDE_FORCE, 2 = ASS_OVERRIDE_SCALE
    int force_all = ctx->is_plain_text || (u.override_mode == 1);

    if (force_all || u.override_mode == 2) {
        for (int i = 0; i < ctx->track->n_styles; i++) {
            ASS_Style *style = &ctx->track->styles[i];

            if (force_all) {
                float font_res_scale = (ctx->orig_playres_y > 0) ? ((float)ctx->orig_playres_y / 720.0f) : 1.0f;
                if (u.font_size > 0) {
                    style->FontSize = u.font_size * font_res_scale;
                }
                if (u.font_family && u.font_family[0] != '\0') {
                    // If the user has an explicit fonts-folder default font AND
                    // u.font_family is still exactly sub_style_create()'s hardcoded
                    // factory default ("roboto medium"), prefer the resolved fonts-
                    // folder default instead. This is the fix for a confirmed bug
                    // (via LIBASS SUB_FONTS logcat output): u.font_family is NEVER
                    // empty by construction, so it always won this force-apply,
                    // which meant a fonts-folder default font could never actually
                    // take effect as FontName no matter what the user picked --
                    // only ass_set_fonts()'s fallback ever saw it, and that fallback
                    // is never consulted once a style already names a font. This
                    // check specifically distinguishes "user never touched the
                    // general font picker" (still holding the exact factory value)
                    // from "user explicitly chose roboto medium via that picker" --
                    // an actual explicit choice, even of the same string, still
                    // wins here since we can't (and shouldn't) tell those apart
                    // from the value alone; this only helps the untouched-default
                    // case, which is what every fresh install/profile starts in.
                    const char *effective_font_name = u.font_family;
                    if (ctx->resolved_default_family[0] != '\0' &&
                        strcmp(u.font_family, "roboto medium") == 0) {
                        effective_font_name = ctx->resolved_default_family;
                    }
                    if (style->FontName) free(style->FontName);
                    style->FontName = strdup(effective_font_name);
                }

                style->Bold = u.is_bold ? -1 : 0;
                style->PrimaryColour = u.text_color;

                if (u.bg_mode == 1) {
                    // PER-LINE BOX (Legacy CC style)
                    // IMPORTANT libass quirk: for BorderStyle=3, Outline is genuine uniform
                    // box padding (all 4 sides), not a stroke width around glyphs. Shadow is
                    // a real offset drop-shadow of the box and is intentionally left at 0.
                    // Padding is now user-adjustable via the dedicated UI control (touch
                    // dialog's boxed_line_padding_row / TV menu's Boxed-Line-only Padding
                    // row), so 0 is a legitimate user choice (a tight box hugging the text)
                    // rather than an unreachable default that needed a >0 safety fallback.
                    style->BorderStyle = 3;
                    style->BackColour = u.bg_color;
                    // Match OutlineColour to BackColour so the padding shares the exact alpha transparency
                    style->OutlineColour = u.bg_color;
                    style->Outline = u.outline_width * font_res_scale;
                    style->Shadow = 0;

                } else if (u.bg_mode == 2) {
                    // UNIFIED BOX (The Libass Secret Weapon)
                    style->BorderStyle = 4;
                    style->OutlineColour = u.outline_color;
                    style->BackColour = u.bg_color;
                    style->Outline = u.outline_width * font_res_scale;
                    style->Shadow = u.shadow_width * font_res_scale; // Hijacked for Padding!

                } else {
                    // STANDARD OUTLINE + SHADOW
                    style->BorderStyle = 1;
                    style->OutlineColour = u.outline_color;
                    style->BackColour = u.shadow_color;
                    style->Outline = u.outline_width * font_res_scale;
                    style->Shadow = u.shadow_width * font_res_scale;
                }
                // Apply custom vertical offset only to bottom-aligned subtitles (numpad layout 1, 2, 3).
                // MarginV is measured from the top for top-aligned styles (7/8/9) — applying it
                // unconditionally pushed those styles' text down from the top instead of up from
                // the bottom, which is what the vertical-offset slider visually looked like.
                if (u.margin_bottom > 0) {
                    if (style->Alignment >= 1 && style->Alignment <= 3) {
                        if (ctx->video_h > 0 && ctx->track->PlayResY > 0) {
                            float scale_ratio = (float)ctx->track->PlayResY / (float)ctx->video_h;
                            style->MarginV = (int)(u.margin_bottom * scale_ratio);
                            DBG serprintf("SUB_SURFACE: Margin translation: UI sent %d physical px -> libass mapped to %d logical px (Scale: %f, PlayResY: %d, SurfaceH: %d)\n",
                                 u.margin_bottom, style->MarginV, scale_ratio, ctx->track->PlayResY, ctx->video_h);
                        } else {
                            // Fallback just in case
                            style->MarginV = u.margin_bottom;
                        }
                    }
                }


            } else if (u.override_mode == 2) {
                // SCALE ONLY MODE: Divide resolution by scale to enlarge everything proportionally
                if (u.font_scale > 0 && u.font_scale != 1.0f) {
                    if (ctx->orig_playres_x > 0 && ctx->orig_playres_y > 0) {
                        ctx->track->PlayResX = (int)(ctx->orig_playres_x / u.font_scale);
                        ctx->track->PlayResY = (int)(ctx->orig_playres_y / u.font_scale);
                    }
                }
            }
        }
    }

    free(u.font_family);
}

// libass message verbosity levels (from ass_types.h): 0=FATAL 1=ERR 2=WARN
// 3=INFO 4=V 5=DBG2 6=... Font family resolution and fallback decisions --
// "did libass find/use font X for family Y" -- are logged by libass itself
// at MSGL_V (4) and MSGL_INFO-adjacent levels, NOT in the 0-3 FATAL/ERR/WARN
// band this callback used to stop at. That's why registering fonts via
// ass_add_font() previously had no visible confirmation from libass's own
// side: the messages were being generated and silently dropped right here.
//
// We widen the ceiling to 6 rather than passing everything through, and
// filter by content instead of leaving it fully open: at level 5-6 libass
// also emits high-frequency per-glyph/per-frame rasterization trace that
// would drown out everything else in logcat. Font-selection messages are
// identifiable by content (contain "font", case-insensitively) regardless
// of exact level, which is more robust than hardcoding an exact level
// number that could shift between libass versions.
static void ass_msg_cb(int level, const char *fmt, va_list va, void *data) {
    if (level < 4) {
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, va);
        DBG serprintf("LIBASS[%d]: %s\n", level, buf);
        return;
    }
    if (level <= 6) {
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, va);
        // strcasestr isn't in every libc; do a manual case-insensitive
        // substring check rather than pull in a portability shim for one
        // log filter.
        int mentions_font = 0;
        for (const char *p = buf; *p; p++) {
            if ((p[0] == 'f' || p[0] == 'F') && (p[1] == 'o' || p[1] == 'O') &&
                (p[2] == 'n' || p[2] == 'N') && (p[3] == 't' || p[3] == 'T')) {
                mentions_font = 1;
                break;
            }
        }
        if (mentions_font) {
            DBG serprintf("LIBASS[%d] SUB_FONTS: %s\n", level, buf);
        }
    }
}

static int ssa_open(SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *params) {
    SSA_BACKEND *ctx = calloc(1, sizeof(SSA_BACKEND));
    if (!ctx) return -1;
    pthread_mutex_init(&ctx->lock, NULL);

    ctx->video_w = params->video_w;
    ctx->video_h = params->video_h;

    ctx->library = ass_library_init();
    if (!ctx->library) {
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        return -1;
    }
    ass_set_message_cb(ctx->library, ass_msg_cb, NULL);
    ass_set_extract_fonts(ctx->library, 1);

    ctx->renderer = ass_renderer_init(ctx->library);
    if (!ctx->renderer) {
        ass_library_done(ctx->library);
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        return -1;
    }

    int final_w = ctx->video_w > 0 ? ctx->video_w : 1920;
    int final_h = ctx->video_h > 0 ? ctx->video_h : 1080;
    DBG serprintf("SUB_SURFACE: Configured libass renderer frame size: %d x %d\n", final_w, final_h);
    ass_set_frame_size(ctx->renderer, final_w, final_h);

    ctx->track = ass_new_track(ctx->library);
    if (!ctx->track) {
        ass_renderer_done(ctx->renderer);
        ass_library_done(ctx->library);
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        return -1;
    }

    if (params->codec_private && params->codec_private_size > 0) {
        ass_process_codec_private(ctx->track, (char *)params->codec_private, params->codec_private_size);
    }

    // Register the user's custom fonts folder (if any) BEFORE ass_set_fonts()
    // below, so libass's font selector already knows about these families the
    // first time it's asked to resolve anything. Safe/no-op if the folder
    // wasn't set or doesn't exist.
    if (params->fonts_dir && params->fonts_dir[0]) {
        load_fonts_dir(ctx->library, params->fonts_dir);
    }

    // Then MKV/container-embedded fonts (AVMEDIA_TYPE_ATTACHMENT streams --
    // see av.h's ATTACHED_FONT and stream_parser_ffmpeg.c). Registered
    // AFTER the custom folder so an explicit user override in the custom
    // folder still wins on a family-name collision, but BEFORE fontconfig
    // gets a chance, same as the custom folder -- these are still fonts the
    // subtitle author actually shipped alongside the video, so they should
    // outrank generic system fonts. (Pipeline order here is a judgment
    // call, not a hard requirement -- swap the two load_* calls if the
    // custom folder should instead defer to what the container shipped.)
    if (params->embedded_fonts && params->embedded_fonts_count > 0) {
        load_embedded_fonts(ctx->library, params->embedded_fonts, params->embedded_fonts_count);
    }

    // Default fallback family, used whenever nothing more specific names a
    // font (plain SRT/VTT text with no style at all, or a style whose font
    // isn't found anywhere). If the user picked a default font from their
    // custom folder, resolve its REAL family name (via FreeType, reading the
    // font's own name table -- see font_name_resolve_family() in
    // font_name_parser.h) and use
    // that here instead of the generic "sans-serif" alias.
    //
    // Note this does NOT disable fontconfig: ASS_FONTPROVIDER_FONTCONFIG is
    // still passed as the provider for names that aren't in the custom
    // fonts folder (e.g. an embedded ASS track's own named style), so system
    // fonts keep working exactly as before for everything else.
    char default_font[256] = "sans-serif";
    int resolved_ok = 0;
    if (params->default_font_name && params->default_font_name[0] &&
        params->fonts_dir && params->fonts_dir[0]) {
        resolved_ok = font_name_resolve_family(params->fonts_dir, params->default_font_name,
                                                default_font, sizeof(default_font));
        if (!resolved_ok) {
            // Resolution failed (file gone, corrupt, or FreeType couldn't parse
            // it) -- fall back to "sans-serif" rather than passing the raw
            // filename through as a guess; a wrong-but-plausible-looking guess
            // silently renders with the wrong font, whereas falling back to
            // "sans-serif" at least behaves exactly like the feature being off,
            // which is a safer failure mode.
            strcpy(default_font, "sans-serif");
            DBG serprintf("SUB_FONTS: failed to resolve real family name for stored default '%s', "
                 "falling back to sans-serif\n", params->default_font_name);
        }
    }
    // Cache the resolved name on ctx so sync_styles() (called repeatedly --
    // once per style-change or new style definition, not just here at open)
    // can force-apply the REAL family name onto style->FontName without
    // re-parsing the font file on every single call. Left as "" (falsy) when
    // resolution didn't succeed or wasn't applicable, which sync_styles()
    // checks before using it.
    ctx->resolved_default_family[0] = '\0';
    if (resolved_ok) {
        strncpy(ctx->resolved_default_family, default_font, sizeof(ctx->resolved_default_family) - 1);
        ctx->resolved_default_family[sizeof(ctx->resolved_default_family) - 1] = '\0';
    }
    ctx->fonts_dir = (params->fonts_dir && params->fonts_dir[0]) ? strdup(params->fonts_dir) : NULL;

    DBG serprintf("SUB_FONTS: requesting default/fallback family '%s' from libass "
         "(custom fonts folder %s)\n", default_font,
         (params->fonts_dir && params->fonts_dir[0]) ? "ACTIVE" : "not set");
    ass_set_fonts(ctx->renderer, NULL, default_font, ASS_FONTPROVIDER_FONTCONFIG, NULL, 1);
    // ass_set_fonts() itself doesn't return whether default_font actually
    // resolved to something -- that only becomes visible the first time
    // libass tries to RENDER text and has to pick a face, at which point it
    // logs its own match/fallback decision through ass_msg_cb() above (now
    // widened to surface font-related messages -- look for lines tagged
    // "LIBASS[..] SUB_FONTS:" once rendering starts, e.g. after the first
    // feed()+render_at() call). A missing custom font typically shows up
    // there as libass silently substituting a system font for the
    // requested family rather than as an explicit error.

    // Save the global style pointers so we can sync them on the render thread
    ctx->user_style_ptr = params->user_style;
    ctx->is_plain_text  = params->is_plain_text_format;

    be->priv = ctx;
    return 0;
}

static int ssa_feed(SUB_FORMAT_BACKEND *be, const uint8_t *data, int size, int64_t pts_ms, int64_t duration_ms) {
    SSA_BACKEND *ctx = (SSA_BACKEND *)be->priv;

    // Strip UTF-8 BOM (EF BB BF) if present — external .ass/.ssa files commonly carry one
    // (e.g. files authored by Aegisub on Windows). Without this, the [Script Info] check
    // below fails and the entire script is incorrectly routed to ass_process_chunk().
    const uint8_t *payload = data;
    int            payload_size = size;
    if (payload_size >= 3 &&
        (unsigned char)payload[0] == 0xEF &&
        (unsigned char)payload[1] == 0xBB &&
        (unsigned char)payload[2] == 0xBF) {
        payload      += 3;
        payload_size -= 3;
    }

    pthread_mutex_lock(&ctx->lock);
    if (payload_size >= 13 && strncmp((const char*)payload, "[Script Info]", 13) == 0) {
        // Full ASS/SSA script buffer (external .ass/.ssa file via sub_engine_feed_raw).
        // ass_process_data() parses the complete script — [Script Info],
        // [V4+ Styles], and all [Events] — in one shot.
        ass_process_data(ctx->track, (char *)payload, payload_size);
    } else if (payload_size >= 10 && strncmp((const char*)payload, "Dialogue: ", 10) == 0) {
        // Single Dialogue line from an internal embedded SSA track.
        ass_process_data(ctx->track, (char *)payload, payload_size);
    } else {
        // Plain text chunk from SRT/VTT wrapper — wrap as an ASS event.
        int64_t final_dur = duration_ms > 0 ? duration_ms : 0;
        ass_process_chunk(ctx->track, (char *)payload, payload_size, pts_ms, final_dur);
    }
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static SUB_FRAME *ssa_render_at(SUB_FORMAT_BACKEND *be, int64_t pts_ms) {
    SSA_BACKEND *ctx = (SSA_BACKEND *)be->priv;
    pthread_mutex_lock(&ctx->lock);
    // --- APPLY LIVE SLIDER UPDATES ---
    sync_styles(ctx);

    int change = 0;
    ASS_Image *imgs = ass_render_frame(ctx->renderer, ctx->track, pts_ms, &change);

    if (!change && imgs != NULL) {
        pthread_mutex_unlock(&ctx->lock);
        return NULL;
    }

    SUB_FRAME *frame = calloc(1, sizeof(SUB_FRAME));
    frame->pts_ms = pts_ms;
    frame->video_w = ctx->video_w > 0 ? ctx->video_w : 1920;
    frame->video_h = ctx->video_h > 0 ? ctx->video_h : 1080;

    SUB_EVENT *last_ev = NULL;

    for (ASS_Image *img = imgs; img; img = img->next) {
        if (!img->w || !img->h) continue;
        uint8_t a = 255 - (img->color & 0xFF);
        if (a == 0) continue;

        uint8_t r = (img->color >> 24) & 0xFF;
        uint8_t g = (img->color >> 16) & 0xFF;
        uint8_t b = (img->color >>  8) & 0xFF;

        SUB_EVENT *ev = calloc(1, sizeof(SUB_EVENT));
        ev->kind = SUB_EVENT_BITMAP;
        ev->x = img->dst_x;
        ev->y = img->dst_y;
        ev->w = img->w;
        ev->h = img->h;

        uint8_t *rgba = malloc(img->w * img->h * 4);
        const uint8_t *src = img->bitmap;
        uint8_t *dst = rgba;

        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                uint8_t mask = src[x];
                uint8_t final_a = (mask * a) / 255;

                dst[0] = r;
                dst[1] = g;
                dst[2] = b;
                dst[3] = final_a;
                dst += 4;
            }
            src += img->stride;
        }

        ev->data.bitmap.rgba = rgba;
        ev->data.bitmap.stride = img->w * 4;

        if (!frame->events) frame->events = ev;
        else last_ev->next = ev;
        last_ev = ev;
    }

    pthread_mutex_unlock(&ctx->lock);
    return frame;
}

static void ssa_free_frame(SUB_FORMAT_BACKEND *be, SUB_FRAME *frame) {
    if (!frame) return;
    SUB_EVENT *ev = frame->events;
    while (ev) {
        SUB_EVENT *next = ev->next;
        if (ev->kind == SUB_EVENT_BITMAP && ev->data.bitmap.rgba) {
            free((void*)ev->data.bitmap.rgba);
        }
        free(ev);
        ev = next;
    }
    free(frame);
}

static int ssa_resize(SUB_FORMAT_BACKEND *be, int w, int h) {
    SSA_BACKEND *ctx = (SSA_BACKEND *)be->priv;
    pthread_mutex_lock(&ctx->lock);
    // Whatever size arrives here is now always correct for the current
    // format by construction: SurfaceController sizes mSubtitleView itself
    // per-format (full surface for plain text -- reaches the black bars --
    // or tethered exactly to the video's own on-screen box for embedded
    // ASS/SSA -- preserves the author's intended aspect/positioning). So
    // this can just be a direct, format-agnostic passthrough; no separate
    // destination-rect tracking needed.
    ctx->video_w = w;
    ctx->video_h = h;
    ass_set_frame_size(ctx->renderer, w, h);
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static int ssa_flush(SUB_FORMAT_BACKEND *be) {
    SSA_BACKEND *ctx = (SSA_BACKEND *)be->priv;
    pthread_mutex_lock(&ctx->lock);
    ass_flush_events(ctx->track);
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static int ssa_close(SUB_FORMAT_BACKEND *be) {
    SSA_BACKEND *ctx = (SSA_BACKEND *)be->priv;
    pthread_mutex_lock(&ctx->lock);
    // --- FREE THE BACKUPS ---
    if (ctx->backups) {
        for (int i = 0; i < ctx->num_backups; i++) {
            if (ctx->backups[i].FontName) free(ctx->backups[i].FontName);
        }
        free(ctx->backups);
    }
    if (ctx->track) ass_free_track(ctx->track);
    if (ctx->renderer) ass_renderer_done(ctx->renderer);
    if (ctx->library) ass_library_done(ctx->library);
    free(ctx->fonts_dir);
    pthread_mutex_unlock(&ctx->lock);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
    return 0;
}

SUB_FORMAT_BACKEND *sub_format_ssa_create(void) {
    SUB_FORMAT_BACKEND *be = calloc(1, sizeof(SUB_FORMAT_BACKEND));
    be->open = ssa_open;
    be->feed = ssa_feed;
    be->render_at = ssa_render_at;
    be->free_frame = ssa_free_frame;
    be->resize = ssa_resize;
    be->flush = ssa_flush;
    be->close = ssa_close;
    return be;
}
