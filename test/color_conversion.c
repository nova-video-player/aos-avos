/* Regression tests for the AVOS/swscale boundary. Each visible row ends at a
 * protected page: SIMD reads/writes must stay inside the adapter's buffers. */
#include "codec_utils.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libavutil/imgutils.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/log.h>

typedef struct {
    uint8_t *mapping, *data;
    size_t size, span;
    int stride, bytes, rows;
} Plane;

static Plane plane(int bytes, int rows)
{
    size_t page = sysconf(_SC_PAGESIZE);
    size_t span = (bytes + page - 1) / page * page;
    Plane p = { .size = (span + page) * rows, .span = span,
                .stride = span + page, .bytes = bytes, .rows = rows };
    p.mapping = mmap(NULL, p.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(p.mapping != MAP_FAILED);
    memset(p.mapping, 0xa5, p.size);
    for (int y = 0; y < rows; y++)
        assert(!mprotect(p.mapping + (size_t)y * p.stride + span, page, PROT_NONE));
    p.data = p.mapping + span - bytes;
    return p;
}

static void release(Plane *p)
{
    if (!p->mapping) return;
    for (int y = 0; y < p->rows; y++)
        for (size_t x = 0; x < p->span - p->bytes; x++)
            assert(p->mapping[(size_t)y * p->stride + x] == 0xa5);
    assert(!munmap(p->mapping, p->size));
}

static int luma(int x, int y) { return (x + y * 3) % 7 < 3 ? 235 : 16; }

static void close_to(int actual, int expected, int tolerance)
{
    if (abs(actual - expected) > tolerance) {
        fprintf(stderr, "value %d, expected %d (+/-%d)\n", actual, expected, tolerance);
        abort();
    }
}

static void boundary_case(void *ctx, int fmt, int out, int w, int h, int negative)
{
    Plane in[4] = {0}, dest[3] = {0};
    uint8_t *data[4] = {0};
    int strides[4] = {0}, bytes[4] = {0};
    assert(!av_image_fill_linesizes(bytes, fmt, w));
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    int ten = fmt == AV_PIX_FMT_YUV420P10LE || fmt == AV_PIX_FMT_P010LE || fmt == AV_PIX_FMT_P010BE;
    int p010 = fmt == AV_PIX_FMT_P010LE || fmt == AV_PIX_FMT_P010BE;
    int packed = fmt == AV_PIX_FMT_YUYV422 || fmt == AV_PIX_FMT_UYVY422;
    for (int i = 0; i < 4; i++) {
        if (!bytes[i]) continue;
        int rows = i == 1 || i == 2 ? (h + (1 << desc->log2_chroma_h) - 1) >> desc->log2_chroma_h : h;
        in[i] = plane(bytes[i], rows);
        data[i] = in[i].data;
        strides[i] = in[i].stride;
        for (int y = 0; y < rows; y++) {
            uint8_t *row = data[i] + (size_t)y * strides[i];
            if (packed) {
                for (int x = 0; x < bytes[i]; x += 4) {
                    int off = fmt == AV_PIX_FMT_UYVY422;
                    row[x + off] = luma(x / 2, y); row[x + 1 - off] = 128;
                    row[x + 2 + off] = luma(x / 2 + 1, y); row[x + 3 - off] = 128;
                }
            } else if (ten) {
                for (int x = 0; x < bytes[i] / 2; x++) {
                    int v = i ? 512 : luma(x, y) * 4;
                    if (p010) v <<= 6;
                    if (fmt == AV_PIX_FMT_P010BE) AV_WB16(row + 2 * x, v);
                    else AV_WL16(row + 2 * x, v);
                }
            } else {
                for (int x = 0; x < bytes[i]; x++) row[x] = i ? 128 : luma(x, y);
            }
        }
        if (negative) {
            data[i] += (size_t)(rows - 1) * strides[i];
            strides[i] = -strides[i];
        }
    }
    VIDEO_FRAME f = { .colorspace = out, .color_space = AVCOL_SPC_BT709, .color_range = AVCOL_RANGE_MPEG };
    int rgb = out == AV_IMAGE_BGRA_32 || out == AV_IMAGE_RGBX_32;
    int n = rgb || out == AV_IMAGE_YUV_422 ? 1 : out == AV_IMAGE_NV12 ? 2 : 3;
    for (int i = 0; i < n; i++) {
        int count = i ? (out == AV_IMAGE_NV12 ? ((w + 1) / 2) * 2 : (w + 1) / 2) :
                        rgb ? w * 4 : out == AV_IMAGE_YUV_422 ? ((w + 1) / 2) * 4 : w;
        dest[i] = plane(count, i ? (h + 1) / 2 : h);
        f.data[i] = dest[i].data;
        f.linestep[i] = dest[i].stride / (rgb ? 4 : 1);
        f.data_size[i] = (dest[i].rows - 1) * dest[i].stride + count;
    }
    assert(codec_frame_can_hold(&f, w, h));
    int ret = codec_convert_mt(ctx, fmt, data, strides, w, h, &f);
    if (ret) {
        fprintf(stderr, "conversion failed: fmt=%d out=%d %dx%d ret=%d\n", fmt, out, w, h, ret);
        abort();
    }
    for (int y = 0; y < h; y++) {
        uint8_t *row = f.data[0] + (size_t)y * dest[0].stride;
        for (int x = 0; x < w; x++) {
            int expected = luma(x, negative ? h - 1 - y : y);
            if (rgb) {
                expected = expected == 235 ? 255 : 0;
                for (int k = 0; k < 3; k++) close_to(row[4 * x + k], expected, 3);
                assert(row[4 * x + 3] == 255);
            } else if (out == AV_IMAGE_YUV_422) {
                close_to(row[2 * x + 1], expected, 1);
                close_to(row[2 * x], 128, 1);
            } else close_to(row[x], expected, 1);
        }
    }
    for (int i = 1; i < n; i++)
        for (int y = 0; y < dest[i].rows; y++)
            for (int x = 0; x < dest[i].bytes; x++) close_to(f.data[i][(size_t)y * dest[i].stride + x], 128, 1);
    for (int i = 0; i < 4; i++) release(&in[i]);
    for (int i = 0; i < 3; i++) release(&dest[i]);
}

static int clip(double x) { return x < 0 ? 0 : x > 255 ? 255 : (int)lrint(x); }

static void colors(void *ctx)
{
    /* Independent matrix reference: asymmetric U/V also detects swapped planes
     * and red/blue channels. Reuse one context while metadata changes. */
    int spaces[] = { AVCOL_SPC_SMPTE170M, AVCOL_SPC_BT709, AVCOL_SPC_BT2020_NCL };
    double kr[] = { .299, .2126, .2627 }, kb[] = { .114, .0722, .0593 };
    uint8_t y[4] = {100,100,100,100}, u[1] = {80}, v[1] = {180}, output[16];
    uint8_t *data[] = {y,u,v,NULL}; int stride[] = {2,1,1,0};
    for (int s = 0; s < 3; s++) for (int full = 0; full < 2; full++) for (int rgbx = 0; rgbx < 2; rgbx++) {
        VIDEO_FRAME f = { .colorspace = rgbx ? AV_IMAGE_RGBX_32 : AV_IMAGE_BGRA_32,
            .color_space = spaces[s], .color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG,
            .data = {output}, .linestep = {2}, .data_size = {sizeof(output)} };
        assert(!codec_convert_mt(ctx, AV_PIX_FMT_YUV420P, data, stride, 2, 2, &f));
        double yy = full ? 100 : (100 - 16) * 255.0 / 219;
        double uu = (80 - 128) * (full ? 1 : 255.0 / 224), vv = (180 - 128) * (full ? 1 : 255.0 / 224);
        int r = clip(yy + 2 * (1 - kr[s]) * vv), b = clip(yy + 2 * (1 - kb[s]) * uu);
        int g = clip(yy - 2 * kb[s] * (1 - kb[s]) / (1 - kr[s] - kb[s]) * uu -
                          2 * kr[s] * (1 - kr[s]) / (1 - kr[s] - kb[s]) * vv);
        close_to(output[rgbx ? 0 : 2], r, 3); close_to(output[1], g, 3); close_to(output[rgbx ? 2 : 0], b, 3);
    }
    uint8_t nv12[6] = {0};
    VIDEO_FRAME f = { .colorspace = AV_IMAGE_NV12, .color_space = AVCOL_SPC_BT709,
        .data = {nv12,nv12+4}, .linestep = {2,2}, .data_size = {4,2} };
    uint8_t *yv12[] = {y,v,u};
    assert(!codec_convert_mt(ctx, PIXFMT_YV12, yv12, stride, 2, 2, &f));
    assert(nv12[4] == 80 && nv12[5] == 180);
    uint8_t yv12out[6] = {0};
    f.colorspace = AV_IMAGE_YV12; f.data[0] = yv12out; f.data[1] = yv12out+4; f.data[2] = yv12out+5;
    f.linestep[1] = f.linestep[2] = 1; f.data_size[1] = f.data_size[2] = 1;
    assert(!codec_convert_mt(ctx, AV_PIX_FMT_YUV420P, data, stride, 2, 2, &f));
    assert(yv12out[4] == 180 && yv12out[5] == 80);
    f.data_size[0] = 3;
    assert(codec_convert_mt(ctx, AV_PIX_FMT_YUV420P, data, stride, 2, 2, &f) < 0 && f.error);
    f.data_size[0] = 4;
    stride[0] = 1;
    assert(codec_convert_mt(ctx, AV_PIX_FMT_YUV420P, data, stride, 2, 2, &f) < 0);
    stride[0] = 2;
    assert(codec_convert_mt(ctx, AV_PIX_FMT_NONE, data, stride, 2, 2, &f) < 0);
    assert(codec_convert_mt(ctx, AV_PIX_FMT_YUV420P, data, stride, 0, 2, &f) < 0);
    assert(codec_convert_mt(ctx, AV_PIX_FMT_YUV420P, data, stride, 2, 2, NULL) < 0);
    data[1] = NULL;
    assert(codec_convert_mt(ctx, AV_PIX_FMT_YUV420P, data, stride, 2, 2, &f) < 0);
}

static void preprocessing(void *ctx)
{
    /* Constant fields must survive RenderX, including odd chroma heights. */
    for (int h = 1; h <= 35; h++) {
        Plane p[3] = {plane(17,h), plane(9,(h+1)/2), plane(9,(h+1)/2)};
        Plane out = plane(17*4,h);
        uint8_t *data[4] = {p[0].data,p[1].data,p[2].data,NULL};
        int stride[4] = {p[0].stride,p[1].stride,p[2].stride,0};
        for (int i = 0; i < 3; i++) for (int y = 0; y < p[i].rows; y++)
            memset(data[i] + (size_t)y * stride[i], i ? 128 : 100, p[i].bytes);
        VIDEO_FRAME f = {.colorspace=AV_IMAGE_BGRA_32, .color_space=AVCOL_SPC_BT709,
            .color_range=AVCOL_RANGE_JPEG, .deinterlace=1,
            .data={out.data}, .linestep={out.stride/4}, .data_size={(h-1)*out.stride+out.bytes}};
        assert(!codec_convert_mt(ctx, AV_PIX_FMT_YUV420P, data, stride, 17, h, &f));
        for (int y = 0; y < h; y++) for (int x = 0; x < 17; x++)
            close_to(out.data[(size_t)y*out.stride+4*x],100,1);
        for (int i = 0; i < 3; i++) release(&p[i]);
        release(&out);
    }
    /* Explicit Qualcomm tile map, covering a partial tile and the final odd
     * tile row. Values vary in both axes so mistaken tile ordering is visible. */
    const int w=130, h=66;
    const int map[3][3] = {{0,1,6},{2,3,4},{8,9,10}};
    uint8_t *tiled = calloc(1,24576+16384); assert(tiled);
    for (int p=0; p<2; p++) for (int y=0; y<(p ? h/2:h); y++) for (int x=0; x<w; x++)
        tiled[(p ? 24576:0)+map[y/32][x/64]*2048+(y%32)*64+x%64] = (x+3*y+p*73)%256;
    Plane dest[2] = {plane(w,h),plane(w,h/2)};
    VIDEO_FRAME f = {.colorspace=AV_IMAGE_NV12, .color_space=AVCOL_SPC_BT709,
        .data={dest[0].data,dest[1].data}, .linestep={dest[0].stride,dest[1].stride},
        .data_size={(h-1)*dest[0].stride+w,(h/2-1)*dest[1].stride+w}};
    uint8_t *data[3] = {tiled,NULL,NULL}; int stride[3] = {0};
    assert(!codec_convert_mt(ctx, PIXFMT_QCOM_NV12_TILED,data,stride,w,h,&f));
    for (int p=0; p<2; p++) for (int y=0; y<dest[p].rows; y++) for (int x=0; x<w; x++)
        assert(dest[p].data[(size_t)y*dest[p].stride+x] == (x+3*y+p*73)%256);
    release(&dest[0]); release(&dest[1]); free(tiled);
}

static void *parallel(void *unused)
{
    void *ctx = unused ? unused : codec_convert_mt_init(2); assert(ctx);
    for (int i = 0; i < 5; i++) { colors(ctx); boundary_case(ctx, AV_PIX_FMT_P010BE, AV_IMAGE_RGBX_32, 65, 33, 0); }
    if (!unused) codec_convert_mt_exit(ctx);
    return NULL;
}

int main(void)
{
    av_log_set_level(AV_LOG_ERROR);
    int formats[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE, AV_PIX_FMT_P010BE, AV_PIX_FMT_YUYV422, AV_PIX_FMT_UYVY422};
    int outputs[] = {AV_IMAGE_BGRA_32, AV_IMAGE_RGBX_32, AV_IMAGE_NV12, AV_IMAGE_YV12, AV_IMAGE_YUV_422};
    int sizes[][2] = {{1,1},{2,2},{7,3},{8,4},{9,5},{15,7},{16,8},{17,9},{31,15},{32,16},{33,17},{63,31},{64,32},{65,33},{1919,1079},{1920,1080},{1921,1081}};
    void *ctx = codec_convert_mt_init(3); assert(ctx);
    int count = 0;
    for (unsigned s = 0; s < sizeof(sizes)/sizeof(*sizes); s++) {
        fprintf(stderr, "testing %dx%d\n", sizes[s][0], sizes[s][1]);
        for (unsigned f = 0; f < sizeof(formats)/sizeof(*formats); f++)
            for (unsigned o = 0; o < sizeof(outputs)/sizeof(*outputs); o++) {
                boundary_case(ctx, formats[f], outputs[o], sizes[s][0], sizes[s][1], s % 2);
                count++;
            }
    }
    colors(ctx);
    preprocessing(ctx);
    codec_convert_mt_exit(ctx);
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, parallel, NULL));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    ctx = codec_convert_mt_init(2); assert(ctx);
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, parallel, ctx));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    codec_convert_mt_exit(ctx);
    printf("PASS: %d guarded format/dimension cases, color/range/plane-order checks, invalid layouts, deinterlacing, detiling, concurrent and shared contexts\n", count);
    return 0;
}
