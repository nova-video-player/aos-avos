#
# Copyright 2026 Courville Software
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

"""Replay Dolby Vision framing and decoder fallback without an Android device."""
import unittest

from native_replay import extract, run_c, source


PREAMBLE = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
enum { AV_CODEC_ID_HEVC = 1, AV_CODEC_ID_AV1, AV_CODEC_ID_H264 };
enum { VIDEO_FORMAT_UNKNOWN, VIDEO_FORMAT_HEVC, VIDEO_FORMAT_AV1,
       VIDEO_FORMAT_DOLBY_VISION, VIDEO_FORMAT_H264 };
enum { VIDEO_FOURCC_HEVC = 1, VIDEO_FOURCC_AV01, VIDEO_FOURCC_DOLBY_VISION, VIDEO_FOURCC_H264 };
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

    def test_av1_config_stays_in_band(self):
        setup = extract(source('codec_sfdec2.c'),
                        'if (video->format == VIDEO_FORMAT_DOLBY_VISION && video->codec_id == AV_CODEC_ID_AV1)')
        run_c(PREAMBLE + r'''
static void check(int codec_id, int clear) {
    VIDEO_PROPERTIES props = { .format = VIDEO_FORMAT_DOLBY_VISION,
        .codec_id = codec_id }, *video = &props;
    void *extradata = props.extraData;
    size_t extradata_size = 172;
''' + setup + r'''
    assert((extradata == NULL) == clear);
    assert(extradata_size == (clear ? 0 : 172));
}
int main(void) {
    check(AV_CODEC_ID_AV1, 1);
    check(AV_CODEC_ID_HEVC, 0);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
