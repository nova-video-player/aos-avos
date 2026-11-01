"""Replay Dolby Vision framing and decoder fallback without an Android device."""
import unittest

from native_replay import extract, run_c, source


PREAMBLE = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
enum { AV_CODEC_ID_HEVC = 1, AV_CODEC_ID_AV1 };
enum { VIDEO_FORMAT_UNKNOWN, VIDEO_FORMAT_HEVC, VIDEO_FORMAT_AV1,
       VIDEO_FORMAT_DOLBY_VISION };
enum { VIDEO_FOURCC_HEVC = 1, VIDEO_FOURCC_AV01, VIDEO_FOURCC_DOLBY_VISION };
typedef unsigned char UCHAR;
typedef struct {
    int format, fourcc, codec_id, hvcc, nal_unit_size, extraDataSize, extraDataSize2;
    UCHAR extraData[512], *extraData2;
} VIDEO_PROPERTIES;
#define serprintf(...) ((void)0)
#define CLOG(...) ((void)0)
#define DBG if (0)
#define DBGS if (0)
#define amalloc malloc
#define afree free
'''


class DolbyVisionBaseCodecTest(unittest.TestCase):
    def test_av1_headers_stay_intact_and_hevc_still_converts(self):
        hevc = source('hevc.c')
        conversion = hevc[hevc.index('static const UCHAR sync_word'):
                          hevc.index('static void _end_NAL')]
        guard = extract(source('stream_video.c'),
                        'if( (s->video->fourcc == VIDEO_FOURCC_HEVC')
        run_c(PREAMBLE + r'''
#include <stdio.h>
#undef serprintf
#define serprintf printf
''' + conversion + r'''
static void prepare(VIDEO_PROPERTIES *video) {
    struct { VIDEO_PROPERTIES *video; } stream = { video }, *s = &stream;
''' + guard + r'''
}
int main(void) {
    /* The sequence header and start of the Dolby metadata OBU from #2011.
     * The old HEVC path treats byte 22 as num_arrays=0, clears the extradata,
     * and treats the first packet's 0a0f as a 2575-byte NAL length. */
    UCHAR av1[172] = { 0x0a,0x0f,0,0,0,0x62,0xeb,0xbf,0xf2,0x19,0xd5,0xf3,
                      0xa1,0x22,0x01,0x23,0x80,0x2a,0x98,0x01,0x04,0xb5,0 };
    VIDEO_PROPERTIES video = { .format = VIDEO_FORMAT_DOLBY_VISION,
        .fourcc = VIDEO_FOURCC_DOLBY_VISION, .codec_id = AV_CODEC_ID_AV1,
        .extraDataSize = sizeof(av1) };
    memcpy(video.extraData, av1, sizeof(av1));
    prepare(&video);
    assert(!video.hvcc && !video.nal_unit_size);
    assert(video.extraDataSize == sizeof(av1));
    assert(!memcmp(video.extraData, av1, sizeof(av1)));

    /* Ordinary AV1 must also bypass conversion. */
    video.format = VIDEO_FORMAT_AV1;
    video.fourcc = VIDEO_FOURCC_AV01;
    prepare(&video);
    assert(!video.hvcc && video.extraDataSize == sizeof(av1));

    /* One synthetic HEVC parameter set in an hvcC record. */
    UCHAR hvcc[31] = { 1 };
    hvcc[21] = 3;
    hvcc[22] = 1;
    hvcc[23] = 33;
    hvcc[25] = 1;
    hvcc[27] = 3;
    hvcc[28] = 0x42; hvcc[29] = 1; hvcc[30] = 1;
    const UCHAR annexb[] = { 0,0,0,1,0x42,1,1 };
    for (int dv = 0; dv <= 1; dv++) {
        memset(&video, 0, sizeof(video));
        video.codec_id = AV_CODEC_ID_HEVC;
        video.format = dv ? VIDEO_FORMAT_DOLBY_VISION : VIDEO_FORMAT_HEVC;
        video.fourcc = dv ? VIDEO_FOURCC_DOLBY_VISION : VIDEO_FOURCC_HEVC;
        video.extraDataSize = sizeof(hvcc);
        memcpy(video.extraData, hvcc, sizeof(hvcc));
        prepare(&video);
        assert(video.hvcc && video.nal_unit_size == 4);
        assert(video.extraDataSize == sizeof(annexb));
        assert(!memcmp(video.extraData, annexb, sizeof(annexb)));
    }
    return 0;
}
''')

    def test_av1_config_and_base_codec_retries(self):
        decoder = source('codec_sfdec2.c')
        setup = decoder[decoder.index('// Match the ordinary AV1 path:'):
                        decoder.index('retry_decoder_open:')]
        init_failure = extract(decoder, 'if (!p->sfdec) {')
        start_failure = extract(decoder, 'if (sfdec_start(p->sfdec) != 0) {')
        run_c(PREAMBLE + r'''
static int deleted;
static int sfdec_start(void *codec) { assert(codec); return -1; }
static void sfdec_delete(void *codec) { assert(codec); deleted++; }
static int open_failure(int codec_id, int start_failure, int *fourcc) {
    VIDEO_PROPERTIES props = { .format = VIDEO_FORMAT_DOLBY_VISION,
        .fourcc = VIDEO_FOURCC_DOLBY_VISION, .codec_id = codec_id }, *video = &props;
    void *extradata = props.extraData;
    size_t extradata_size = 172;
    struct { void *sfdec; } state = { start_failure ? &props : NULL }, *p = &state;
''' + setup + r'''
    if (codec_id == AV_CODEC_ID_AV1) {
        assert(!extradata && !extradata_size);
    } else {
        assert(extradata == props.extraData && extradata_size == 172);
    }
''' + init_failure + '\n' + start_failure + r'''
    assert(!"expected decoder failure");
err:
    return VIDEO_FORMAT_UNKNOWN;
retry_decoder_open:
    assert(tried_base_fallback);
    assert(!p->sfdec);
    *fourcc = effective_fourcc;
    return effective_format;
}
int main(void) {
    for (int start_failure = 0; start_failure <= 1; start_failure++) {
        int fourcc = 0;
        assert(open_failure(AV_CODEC_ID_AV1, start_failure, &fourcc) == VIDEO_FORMAT_AV1);
        assert(fourcc == VIDEO_FOURCC_AV01);
        assert(open_failure(AV_CODEC_ID_HEVC, start_failure, &fourcc) == VIDEO_FORMAT_HEVC);
        assert(fourcc == VIDEO_FOURCC_HEVC);
        assert(open_failure(0, start_failure, &fourcc) == VIDEO_FORMAT_UNKNOWN);
    }
    assert(deleted == 2);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
