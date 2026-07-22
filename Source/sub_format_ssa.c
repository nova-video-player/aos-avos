#include "sub_format.h"
#include "sub_style.h"
#include <ass/ass.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <android/log.h>

#define LOG_TAG "SubFormatSSA"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

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
} SSA_BACKEND;

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
                    if (style->FontName) free(style->FontName);
                    style->FontName = strdup(u.font_family);
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
                            LOGD("SUB_SURFACE: Margin translation: UI sent %d physical px -> libass mapped to %d logical px (Scale: %f, PlayResY: %d, SurfaceH: %d)",
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

static void ass_msg_cb(int level, const char *fmt, va_list va, void *data) {
    if (level < 4) {
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, va);
        LOGD("LIBASS[%d]: %s", level, buf);
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
    LOGD("SUB_SURFACE: Configured libass renderer frame size: %d x %d", final_w, final_h);
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

    // Rely entirely on the global Fontconfig XML!
    ass_set_fonts(ctx->renderer, NULL, "sans-serif", ASS_FONTPROVIDER_FONTCONFIG, NULL, 1);

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
