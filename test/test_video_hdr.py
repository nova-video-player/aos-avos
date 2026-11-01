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

"""Replay MediaCodec color mapping and CTA HDR metadata serialization."""
import unittest
from native_replay import ROOT, extract, run_c

class VideoHdrTest(unittest.TestCase):
    def test_color_mapping_preserves_explicit_primaries_and_unknown_values(self):
        text = (ROOT / 'external/android/libsfdec/sfdec_ndkmediacodec.cpp').read_text()
        functions = '\n'.join(extract(text, 'static int ' + name + '(') for name in
            ('map_color_transfer', 'map_color_standard', 'map_color_range'))
        run_c('#include <assert.h>\n' + functions + r'''
int main(void) {
    assert(map_color_standard(5, 5) == 2);  // PAL
    assert(map_color_standard(6, 6) == 4);  // NTSC
    assert(map_color_standard(1, 9) == 1);  // explicit BT.709 wins
    assert(map_color_standard(9, 1) == 6);  // explicit BT.2020 wins
    assert(map_color_standard(2, 5) == 2);  // infer only if unspecified
    assert(map_color_standard(2, 9) == 6);
    assert(map_color_standard(12, 9) == 0); // unsupported primaries
    assert(map_color_standard(2, 10) == 0); // constant-luminance not representable
    assert(map_color_standard(2, 2) == 0);
    assert(map_color_transfer(8) == 1);
    assert(map_color_transfer(14) == 3 && map_color_transfer(15) == 3);
    assert(map_color_transfer(16) == 6 && map_color_transfer(18) == 7);
    assert(map_color_transfer(2) == 0);
    assert(map_color_range(1) == 2 && map_color_range(2) == 1);
    assert(map_color_range(0) == 0);
    return 0;
}
''')

    def test_hdr_descriptor_units_byte_order_absence_and_malformed_values(self):
        text = (ROOT / 'Include/video_hdr.h').read_text()
        functions = extract(text, 'static inline int video_hdr_scaled(') + extract(text, 'static inline int video_hdr_static_info(')
        run_c(r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
typedef struct { int num, den; } AVRational;
typedef struct {
    AVRational display_primaries[3][2], white_point[2], min_luminance, max_luminance;
    int has_primaries, has_luminance;
} AVMasteringDisplayMetadata;
typedef struct { unsigned MaxCLL, MaxFALL; } AVContentLightMetadata;
''' + functions + r'''
static int word(unsigned char *out, int index) {
    return out[1 + index*2] | out[2 + index*2] << 8;
}
int main(void) {
    unsigned char out[25];
    memset(out, 255, sizeof(out));
    assert(video_hdr_static_info(out, NULL, NULL) == 0);
    for (int i=0; i<25; i++) assert(out[i] == 0);
    AVMasteringDisplayMetadata m = {
        .display_primaries = {{{680,1000},{320,1000}},{{265,1000},{690,1000}},{{150,1000},{60,1000}}},
        .white_point = {{3127,10000},{3290,10000}},
        .min_luminance = {1,200}, .max_luminance = {1000,1},
        .has_primaries = 1, .has_luminance = 1 };
    AVContentLightMetadata light = {283,196};
    assert(video_hdr_static_info(out, &m, &light) == 25 && out[0] == 0);
    assert(word(out,0) == 34000 && word(out,1) == 16000);
    assert(word(out,2) == 13250 && word(out,3) == 34500);
    assert(word(out,4) == 7500 && word(out,5) == 3000);
    assert(word(out,6) == 15635 && word(out,7) == 16450);
    assert(word(out,8) == 1000 && word(out,9) == 50);
    assert(word(out,10) == 283 && word(out,11) == 196);
    assert(out[21] == 0x1b && out[22] == 1);
    assert(video_hdr_static_info(out, &m, NULL) == 25 && word(out,10) == 0);
    assert(video_hdr_static_info(out, NULL, &light) == 25 && word(out,8) == 0);
    light.MaxCLL=light.MaxFALL=0; // present zero-valued metadata differs from absent
    assert(video_hdr_static_info(out, NULL, &light) == 25);
    m.display_primaries[0][0].den=0;
    m.min_luminance.num=-1;
    light.MaxCLL=UINT_MAX;
    assert(video_hdr_static_info(out, &m, &light) == 0);
    assert(video_hdr_scaled((AVRational){INT_MAX,1}, 50000, 50000) == -1);
    assert(video_hdr_scaled((AVRational){1,2}, 1, 65535) == 1);
    m.has_primaries=0; m.min_luminance=(AVRational){2,1}; m.max_luminance=(AVRational){1,1};
    assert(video_hdr_static_info(out, &m, NULL) == 0);
    return 0;
}
''')

    def test_api23_omits_new_color_and_hdr_keys(self):
        text = (ROOT / 'external/android/libsfdec/sfdec_ndkmediacodec.cpp').read_text()
        functions = '\n'.join(extract(text, 'static int ' + name + '(') for name in
            ('map_color_transfer', 'map_color_standard', 'map_color_range'))
        color = extract(text, 'if (api_level >= 24)')
        hdr = extract(text, 'if (api_level >= 24 && hdr_static_info')
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#define LOG(...) ((void)0)
static int colors, buffers;
static void AMediaFormat_setInt32(void *format, const char *key, int value) {
    (void)format; (void)key; assert(value > 0); colors++;
}
static void AMediaFormat_setBuffer(void *format, const char *key, const void *data, size_t size) {
    (void)format; (void)key; assert(data && size == 25); buffers++;
}
''' + functions + r'''
static void configure(int api_level, size_t hdr_static_info_size) {
    struct { void *mFormat; } codec={0}, *sfdec=&codec;
    int color_trc=16, color_primaries=9, color_space=9, color_range=1;
    unsigned char data[25]={0}; const void *hdr_static_info=data;
''' + color + hdr + r'''
}
int main(void) {
    configure(23,25); assert(colors==0 && buffers==0);
    configure(24,25); assert(colors==3 && buffers==1);
    configure(24,0); assert(colors==6 && buffers==1);
    configure(34,24); assert(colors==9 && buffers==1);
    return 0;
}
''')
