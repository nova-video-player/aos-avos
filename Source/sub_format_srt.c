#include "sub_format.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <android/log.h>

#define LOG_TAG "SubFormatSRT"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

extern SUB_FORMAT_BACKEND *sub_format_ssa_create(void);

typedef struct {
    SUB_FORMAT_BACKEND *ssa_backend;
    int read_order;
} SRT_BACKEND;

static char* generate_dynamic_ass_header(const SUB_USER_STYLE *style, int video_w, int video_h) {
    char *header = malloc(2048);

    double aspect = 16.0 / 9.0;
    if (video_w > 0 && video_h > 0) {
        aspect = (double)video_w / (double)video_h;
    }

    int playres_y = 720;
    int playres_x = (int)(playres_y * aspect);

    LOGD("SUB_SURFACE: Generated ASS header with PlayResX: %d, PlayResY: %d (Aspect: %f, Surface: %dx%d)",
         playres_x, playres_y, aspect, video_w, video_h);

    int font_size = 40;
    int margin_v = 10;
    int margin_h = 20;

    // Alignment 2 = Bottom-Center. BorderStyle 3 = Tight Box.
    snprintf(header, 2048,
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: %d\n"
        "PlayResY: %d\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,sans-serif,%d,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,3,0,0,2,%d,%d,%d,1\n",
        playres_x, playres_y, font_size, margin_h, margin_h, margin_v);

    return header;
}

static int srt_open(SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *params) {
    SRT_BACKEND *ctx = calloc(1, sizeof(SRT_BACKEND));
    if (!ctx) return -1;

    ctx->ssa_backend = sub_format_ssa_create();
    if (!ctx->ssa_backend) {
        free(ctx);
        return -1;
    }

    char *synthetic_header = generate_dynamic_ass_header(params->user_style, params->video_w, params->video_h);
    if (!synthetic_header) {
        free(ctx->ssa_backend);
        free(ctx);
        return -1;
    }

    SUB_FORMAT_OPEN_PARAMS ssa_params = *params;
    ssa_params.codec_private = (const uint8_t*)synthetic_header;
    ssa_params.codec_private_size = strlen(synthetic_header);

    int ret = ctx->ssa_backend->open(ctx->ssa_backend, &ssa_params);
    free(synthetic_header);

    if (ret != 0) {
        // ssa_open failed — it may have partially constructed its ctx.
        // Call close() to drain whatever was allocated before freeing the shell.
        // We do NOT set be->priv so if sub_engine_open_track calls our srt_close
        // via its failure path, the NULL ctx guard below makes it a safe no-op.
        if (ctx->ssa_backend->close) ctx->ssa_backend->close(ctx->ssa_backend);
        free(ctx->ssa_backend);
        free(ctx);
        return ret;
    }

    be->priv = ctx;
    return 0;
}

// srt_text_to_ass — the SHARED/COMMON-tag layer.
//
// Contract (mirrors the old subtitle_clean_formatter clean_tags split, but
// enforced structurally instead of via a flag):
//
//   1. Format-specific syntax (VTT <v Speaker>/<c.class>, SMI <P Class=...>,
//      etc.) is NOT this function's job. Each parser (subtitle_vtt.c,
//      subtitle_smi.c, ...) must translate its own dialect -- into either
//      plain text or real ASS override blocks ("{\...}") -- BEFORE the text
//      reaches feed()/sub_engine_feed(). By the time text lands here, no
//      format-specific tags should remain.
//
//   2. Any "{\...}" ASS override block already present in the input (put
//      there by an upstream parser, e.g. a translated <v> or a passthrough
//      of native ASS syntax) is passed through byte-for-byte, untouched.
//      This function must never mangle or strip real ASS tags -- only
//      HTML-ish "<...>" tags are its concern.
//
//   3. Within "<...>" tags, only the small common set below -- the styling
//      markup that shows up across multiple plain-text formats (SRT, VTT,
//      SMI, SUB, MPL2) and has no format-specific meaning -- is translated:
//        <b>...</b>             -> {\b1}...{\b0}
//        <i>...</i>              -> {\i1}...{\i0}
//        <u>...</u>              -> {\u1}...{\u0}
//        <font color="#RRGGBB">  -> {\c&HBBGGRR&}   (HTML RGB -> ASS BGR)
//        </font>                 -> {\c}             (reset to style default)
//
//   4. ANY other "<...>" tag -- unknown, malformed, or a format-specific tag
//      that slipped through because its parser didn't handle it -- is
//      dropped silently. This is the safety net: we never want to leak a
//      raw "<...>" onto the screen as literal text, and we never want to
//      guess at semantics we don't own here.
//
// `in` is the cue text after any format-specific translation upstream (may
// still contain embedded '\n' from multi-line cues and/or literal "{\...}"
// blocks). `out` must be at least in_size * 6 + 1 bytes (worst case: every
// input byte becomes part of a "{\c&HBBGGRR&}"-sized expansion; 6x is
// generous headroom since color tags are the biggest expansion and operate
// on whole tags, not per byte). Returns the number of bytes written to
// `out`, not including the terminating '\0'.
static int srt_text_to_ass(const char *in, int in_size, char *out) {
    int i = 0, j = 0;

    while (i < in_size && in[i] != '\0') {
        if (in[i] == '\r') {
            i++;
            continue; // skip Windows line endings
        }
        if (in[i] == '\n') {
            out[j++] = '\\';
            out[j++] = 'N';
            i++;
            continue;
        }

        // Rule 2: an ASS override block already in the input (from an
        // upstream format-specific translation) passes through verbatim.
        // We must not treat its '{' / '\' / '}' as ordinary text to mangle,
        // and must not accidentally match it against the '<' tag logic
        // below (it can't, since it starts with '{' not '<', but the guard
        // is kept explicit here so this contract can't be broken by a
        // future edit that reorders the checks).
        if (in[i] == '{') {
            int k = i + 1;
            while (k < in_size && in[k] != '\0' && in[k] != '}') k++;
            if (k < in_size && in[k] == '}') {
                int block_len = k - i + 1;
                memcpy(out + j, in + i, block_len);
                j += block_len;
                i = k + 1;
                continue;
            }
            // Unterminated '{' — fall through and copy it as a literal char
            // rather than losing it; matches the unterminated-tag handling
            // for '<' below.
            out[j++] = in[i++];
            continue;
        }

        if (in[i] != '<') {
            out[j++] = in[i++];
            continue;
        }

        // We're at a '<' — find the matching '>' before deciding what to do.
        int tag_start = i;
        int k = i + 1;
        while (k < in_size && in[k] != '\0' && in[k] != '>') k++;
        if (k >= in_size || in[k] != '>') {
            // No closing '>' in this chunk — treat the rest as plain text
            // rather than silently eating an unterminated tag's tail.
            out[j++] = in[i++];
            continue;
        }
        int tag_len = k - tag_start + 1; // includes '<' and '>'
        const char *tag = in + tag_start;

        if (tag_len >= 3 && (tag[1] == 'b' || tag[1] == 'B') && tag[2] == '>') {
            memcpy(out + j, "{\\b1}", 5); j += 5;
        } else if (tag_len >= 4 && tag[1] == '/' && (tag[2] == 'b' || tag[2] == 'B') && tag[3] == '>') {
            memcpy(out + j, "{\\b0}", 5); j += 5;
        } else if (tag_len >= 3 && (tag[1] == 'i' || tag[1] == 'I') && tag[2] == '>') {
            memcpy(out + j, "{\\i1}", 5); j += 5;
        } else if (tag_len >= 4 && tag[1] == '/' && (tag[2] == 'i' || tag[2] == 'I') && tag[3] == '>') {
            memcpy(out + j, "{\\i0}", 5); j += 5;
        } else if (tag_len >= 3 && (tag[1] == 'u' || tag[1] == 'U') && tag[2] == '>') {
            memcpy(out + j, "{\\u1}", 5); j += 5;
        } else if (tag_len >= 4 && tag[1] == '/' && (tag[2] == 'u' || tag[2] == 'U') && tag[3] == '>') {
            memcpy(out + j, "{\\u0}", 5); j += 5;
        } else if (tag_len >= 7 && strncasecmp(tag + 1, "font", 4) == 0 &&
                   (tag[5] == '>' || tag[5] == ' ')) {
            // <font ... color="#RRGGBB" ...> — pull the first hex color found
            // between here and '>'. Anything we can't parse just emits no
            // override (falls back to the style's own PrimaryColour), rather
            // than aborting the whole tag translation.
            const char *search = tag;
            const char *hash = memchr(search, '#', tag_len);
            unsigned int rr = 0, gg = 0, bb = 0;
            int parsed = 0;
            if (hash && (hash - tag) < tag_len - 6) {
                parsed = sscanf(hash + 1, "%2x%2x%2x", &rr, &gg, &bb) == 3;
            }
            if (parsed) {
                // HTML #RRGGBB -> ASS &HBBGGRR& (byte order reversed)
                j += sprintf(out + j, "{\\c&H%02X%02X%02X&}", bb, gg, rr);
            }
            // no recognizable color -> emit nothing, just drop the tag
        } else if (tag_len >= 7 && tag[1] == '/' && strncasecmp(tag + 2, "font", 4) == 0 && tag[6] == '>') {
            memcpy(out + j, "{\\c}", 4); j += 4;
        }
        // Rule 4: anything else -- unrecognized common tag, or a format-
        // specific tag (VTT <v>/<c>, SMI <P Class=...>, ...) that its own
        // parser should have translated already but didn't -- is dropped.
        // We deliberately do NOT try to guess its semantics here.

        i = k + 1;
    }
    out[j] = '\0';
    return j;
}

static int srt_feed(SUB_FORMAT_BACKEND *be, const uint8_t *data, int size, int64_t pts_ms, int64_t duration_ms) {
    SRT_BACKEND *ctx = (SRT_BACKEND *)be->priv;
    const char *text = (const char *)data;

    // Prefix "N,0,Default,,0,0,0,," (read_order can grow to several digits
    // over a long track) + translated text, worst case ~6x input size (see
    // srt_text_to_ass() comment) + NUL.
    char *ass_payload = malloc(size * 6 + 160);
    int offset = sprintf(ass_payload, "%d,0,Default,,0,0,0,,", ctx->read_order++);

    offset += srt_text_to_ass(text, size, ass_payload + offset);

    // Send the ASS-tagged string down into the SSA backend with the correctly extracted duration!
    int ret = ctx->ssa_backend->feed(ctx->ssa_backend, (const uint8_t*)ass_payload, offset, pts_ms, duration_ms);

    free(ass_payload);
    return ret;
}

static SUB_FRAME *srt_render_at(SUB_FORMAT_BACKEND *be, int64_t pts_ms) { return ((SRT_BACKEND *)be->priv)->ssa_backend->render_at(((SRT_BACKEND *)be->priv)->ssa_backend, pts_ms); }
static void srt_free_frame(SUB_FORMAT_BACKEND *be, SUB_FRAME *frame) { ((SRT_BACKEND *)be->priv)->ssa_backend->free_frame(((SRT_BACKEND *)be->priv)->ssa_backend, frame); }
static int srt_resize(SUB_FORMAT_BACKEND *be, int video_w, int video_h) { return ((SRT_BACKEND *)be->priv)->ssa_backend->resize(((SRT_BACKEND *)be->priv)->ssa_backend, video_w, video_h); }
static int srt_flush(SUB_FORMAT_BACKEND *be) { return ((SRT_BACKEND *)be->priv)->ssa_backend->flush(((SRT_BACKEND *)be->priv)->ssa_backend); }
static int srt_close(SUB_FORMAT_BACKEND *be) {
    SRT_BACKEND *ctx = (SRT_BACKEND *)be->priv;
    if (!ctx) return 0; // srt_open failed before setting be->priv — safe no-op
    if (ctx->ssa_backend) {
        ctx->ssa_backend->close(ctx->ssa_backend);
        free(ctx->ssa_backend);
    }
    free(ctx);
    return 0;
}

SUB_FORMAT_BACKEND *sub_format_srt_create(void) {
    SUB_FORMAT_BACKEND *be = calloc(1, sizeof(SUB_FORMAT_BACKEND));
    be->open = srt_open;
    be->feed = srt_feed;
    be->render_at = srt_render_at;
    be->free_frame = srt_free_frame;
    be->resize = srt_resize;
    be->flush = srt_flush;
    be->close = srt_close;
    return be;
}
