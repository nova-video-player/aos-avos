#include "sub_format.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern SUB_FORMAT_BACKEND *sub_format_ssa_create(void);

typedef struct {
    SUB_FORMAT_BACKEND *ssa_backend;
    int read_order;
} SRT_BACKEND;

static char* generate_dynamic_ass_header(const SUB_USER_STYLE *style, int video_w, int video_h) {
    char *header = malloc(2048);
    if (video_w <= 0) video_w = 1920;
    if (video_h <= 0) video_h = 1080;

    int font_size = (int)(video_h * 0.055);
    int margin_v = (int)(video_h * 0.06);
    int margin_h = (int)(video_w * 0.05);

    // Alignment 2 = Bottom-Center. BorderStyle 3 = Tight Box.
    snprintf(header, 2048,
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: %d\n"
        "PlayResY: %d\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,sans-serif,%d,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,3,0,0,2,%d,%d,%d,1\n",
        video_w, video_h, font_size, margin_h, margin_h, margin_v);

    return header;
}

static int srt_open(SUB_FORMAT_BACKEND *be, const SUB_FORMAT_OPEN_PARAMS *params) {
    SRT_BACKEND *ctx = calloc(1, sizeof(SRT_BACKEND));
    ctx->ssa_backend = sub_format_ssa_create();

    char *synthetic_header = generate_dynamic_ass_header(params->user_style, params->video_w, params->video_h);

    SUB_FORMAT_OPEN_PARAMS ssa_params = *params;
    ssa_params.codec_private = (const uint8_t*)synthetic_header;
    ssa_params.codec_private_size = strlen(synthetic_header);

    int ret = ctx->ssa_backend->open(ctx->ssa_backend, &ssa_params);
    free(synthetic_header);
    be->priv = ctx;
    return ret;
}

static int srt_feed(SUB_FORMAT_BACKEND *be, const uint8_t *data, int size, int64_t pts_ms, int64_t duration_ms) {
    SRT_BACKEND *ctx = (SRT_BACKEND *)be->priv;
    const char *text = (const char *)data;

    char *ass_payload = malloc(size * 2 + 128);
    int offset = sprintf(ass_payload, "%d,0,Default,,0,0,0,,", ctx->read_order++);

    int i = 0, j = offset;
    int in_tag = 0;
    while (i < size && text[i] != '\0') {
        if (text[i] == '<') in_tag = 1;
        else if (text[i] == '>') in_tag = 0;
        else if (!in_tag) {
            if (text[i] == '\r') { /* skip Windows returns */ }
            else if (text[i] == '\n') {
                ass_payload[j++] = '\\';
                ass_payload[j++] = 'N';
            } else {
                ass_payload[j++] = text[i];
            }
        }
        i++;
    }
    ass_payload[j] = '\0';

    // Send pure string down into the SSA backend with the correctly extracted duration!
    int ret = ctx->ssa_backend->feed(ctx->ssa_backend, (const uint8_t*)ass_payload, strlen(ass_payload), pts_ms, duration_ms);

    free(ass_payload);
    return ret;
}

static SUB_FRAME *srt_render_at(SUB_FORMAT_BACKEND *be, int64_t pts_ms) { return ((SRT_BACKEND *)be->priv)->ssa_backend->render_at(((SRT_BACKEND *)be->priv)->ssa_backend, pts_ms); }
static void srt_free_frame(SUB_FORMAT_BACKEND *be, SUB_FRAME *frame) { ((SRT_BACKEND *)be->priv)->ssa_backend->free_frame(((SRT_BACKEND *)be->priv)->ssa_backend, frame); }
static int srt_resize(SUB_FORMAT_BACKEND *be, int video_w, int video_h) { return ((SRT_BACKEND *)be->priv)->ssa_backend->resize(((SRT_BACKEND *)be->priv)->ssa_backend, video_w, video_h); }
static int srt_flush(SUB_FORMAT_BACKEND *be) { return ((SRT_BACKEND *)be->priv)->ssa_backend->flush(((SRT_BACKEND *)be->priv)->ssa_backend); }
static int srt_close(SUB_FORMAT_BACKEND *be) {
    SRT_BACKEND *ctx = (SRT_BACKEND *)be->priv;
    ctx->ssa_backend->close(ctx->ssa_backend);
    free(ctx->ssa_backend);
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
