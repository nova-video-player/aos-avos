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

"""Profile selection and base-layer color compatibility replays."""
import unittest

from native_replay import ROOT, extract, run_c, source
from test_dovi_base_codec import PREAMBLE


VIDEO = PREAMBLE.replace('int format, fourcc, codec_id,',
                        'int dv_profile, dv_bitstream_profile, dv_bl_present_flag, '
                        'dv_bl_signal_compatibility_id, format, fourcc, codec_id,')
POLICY = '\n'.join(extract((ROOT / 'Include/video_dovi.h').read_text(), marker)
                   for marker in ('static inline int video_dovi_base_format(',
                                  'static inline int video_dovi_decode_format(',
                                  'static inline int video_dovi_colors_unsupported(',
                                  'static inline int video_dovi_use_base('))


class DolbyVisionPolicyTest(unittest.TestCase):
    def test_missing_profile_match_requires_explicit_force(self):
        guard = extract(source('codec_sfdec2.c'),
                        'if (!decoder_name && acodecs_get_dovi_mode() != 2)')
        run_c(VIDEO + r'''
static int mode;
static int acodecs_get_dovi_mode(void) { return mode; }
static int selection_allowed(const char *decoder_name) {
''' + guard + r'''
    return 1;
err:
    return 0;
}
int main(void) {
    for (mode = 0; mode <= 2; mode++) {
        assert(selection_allowed("matched.decoder"));
        assert(selection_allowed(NULL) == (mode == 2));
    }
    return 0;
}
''')

    def test_decode_fallback_is_separate_from_color_compatibility(self):
        run_c(VIDEO + POLICY + r'''
int main(void) {
    VIDEO_PROPERTIES v = { .format = VIDEO_FORMAT_DOLBY_VISION,
        .fourcc = VIDEO_FOURCC_DOLBY_VISION, .dv_bl_present_flag = 1,
        .codec_id = AV_CODEC_ID_AV1, .dv_bitstream_profile = 10 };
    for (int ccid = 0; ccid < 16; ccid++) {
        v.dv_bl_signal_compatibility_id = ccid;
        int compatible = ccid == 1 || ccid == 2 || ccid == 4 || ccid == 6;
        assert(video_dovi_base_format(&v) == (compatible ? VIDEO_FORMAT_AV1 : 0));
        v.format = VIDEO_FORMAT_DOLBY_VISION;
        assert(!video_dovi_colors_unsupported(&v));
        assert(video_dovi_use_base(&v));
        assert(video_dovi_colors_unsupported(&v) == !compatible);
    }
    v.format = VIDEO_FORMAT_DOLBY_VISION;
    v.dv_bl_signal_compatibility_id = 1;
    v.dv_bl_present_flag = 0;
    assert(!video_dovi_use_base(&v));
    v.dv_bl_present_flag = 1;
    assert(video_dovi_use_base(&v));
    assert(v.format == VIDEO_FORMAT_AV1 && v.fourcc == VIDEO_FOURCC_AV01);
    assert(v.codec_id == AV_CODEC_ID_AV1 && v.dv_bitstream_profile == 10);
    assert(!video_dovi_use_base(&v)); /* one-way, no retry loop */

    v.codec_id = AV_CODEC_ID_HEVC;
    for (int profile = 0; profile <= 10; profile++) {
        v.dv_bitstream_profile = profile;
        int compatible = profile == 4 || profile == 7 || profile == 8;
        assert(video_dovi_base_format(&v) == (compatible ? VIDEO_FORMAT_HEVC : 0));
    }
    v.dv_bitstream_profile = 7;
    v.dv_bl_signal_compatibility_id = 6;
    v.hvcc = 1; v.nal_unit_size = 4;
    v.format = VIDEO_FORMAT_DOLBY_VISION;
    assert(video_dovi_use_base(&v));
    assert(v.format == VIDEO_FORMAT_HEVC && v.fourcc == VIDEO_FOURCC_HEVC);
    assert(v.hvcc == 1 && v.nal_unit_size == 4);
    assert(!video_dovi_colors_unsupported(&v));
    v.dv_bitstream_profile = 5;
    v.dv_bl_signal_compatibility_id = 0;
    v.format = VIDEO_FORMAT_DOLBY_VISION;
    assert(video_dovi_use_base(&v));
    assert(v.format == VIDEO_FORMAT_HEVC && video_dovi_colors_unsupported(&v));
    assert(!video_dovi_use_base(&v));
    v.dv_bitstream_profile = 0;
    assert(!video_dovi_colors_unsupported(&v));
    return 0;
}
''')

    def test_parser_preserves_phh_profile_selection_and_user_modes(self):
        parser = source('stream_parser_ffmpeg.c')
        start = parser.index('                        video->dv_profile = 0;')
        end = parser.index('\n\t\t\t}\n\t\t} else if', start)
        selection = parser[start:end]
        run_c(VIDEO + POLICY + r'''
#include <stddef.h>
#include <stdint.h>
#define CONFIG_ANDROID
#undef serprintf
static void serprintf(const char *format, ...) { (void)format; }
typedef struct {
    uint8_t dv_profile, dv_level, rpu_present_flag, el_present_flag;
    uint8_t bl_present_flag, dv_bl_signal_compatibility_id;
} AVDOVIDecoderConfigurationRecord;
static int matches, queried_profile, queries, display_caps = -1;
static int acodecs_get_display_hdr_capabilities(void) { return display_caps; }
static const char *acodecs_get_for_profile(const char *mime, int profile) {
    assert(!strcmp(mime, "video/dolby-vision"));
    queried_profile = profile; queries++;
    return matches ? strdup("profile.decoder") : NULL;
}
static VIDEO_PROPERTIES select_video(int base, int profile, int ccid, int mode,
                                     int supported, int match, size_t record_size) {
    VIDEO_PROPERTIES v = { .format = base, .codec_id = base == VIDEO_FORMAT_AV1 ?
        AV_CODEC_ID_AV1 : base == VIDEO_FORMAT_H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC }, *video = &v;
    AVDOVIDecoderConfigurationRecord record = {
        .dv_profile = profile, .dv_level = 6, .dv_bl_signal_compatibility_id = ccid,
        .rpu_present_flag = 1, .bl_present_flag = 1 };
    uint8_t *side_data = (uint8_t*)&record;
    size_t side_data_size = record_size;
    int dovi_codec_supported = supported, dovi_mode = mode;
    matches = match; queries = queried_profile = 0;
''' + selection + r'''
    return v;
}
int main(void) {
    VIDEO_PROPERTIES v;
    const size_t size = sizeof(AVDOVIDecoderConfigurationRecord);
    /* phh's profile lookup stays first choice for AV1 and HEVC P8. */
    v = select_video(VIDEO_FORMAT_AV1, 10, 1, 0, 1, 1, size);
    assert(v.format == VIDEO_FORMAT_DOLBY_VISION && queried_profile == 1024);
    v = select_video(VIDEO_FORMAT_HEVC, 8, 1, 0, 1, 1, size);
    assert(v.format == VIDEO_FORMAT_DOLBY_VISION && queried_profile == 256);
    v = select_video(VIDEO_FORMAT_H264, 9, 2, 0, 1, 1, size);
    assert(v.format == VIDEO_FORMAT_DOLBY_VISION && queried_profile == 512);
    assert(video_dovi_use_base(&v));
    assert(v.format == VIDEO_FORMAT_H264 && v.fourcc == VIDEO_FOURCC_H264);
    v = select_video(VIDEO_FORMAT_H264, 9, 2, 0, 1, 0, size);
    assert(v.format == VIDEO_FORMAT_H264);
    v = select_video(VIDEO_FORMAT_H264, 9, 0, 0, 1, 1, size);
    assert(video_dovi_use_base(&v) && video_dovi_colors_unsupported(&v));
    /* Profile 7 retains its existing HDR base preference, unless forced. */
    v = select_video(VIDEO_FORMAT_HEVC, 7, 6, 0, 1, 1, size);
    assert(v.format == VIDEO_FORMAT_HEVC && queried_profile == 256);
    v = select_video(VIDEO_FORMAT_HEVC, 7, 6, 2, 1, 1, size);
    assert(v.format == VIDEO_FORMAT_DOLBY_VISION && queried_profile == 256);
    /* Proprietary colors still prefer DV, but failed attempts can use the base. */
    v = select_video(VIDEO_FORMAT_AV1, 10, 1, 0, 1, 0, size);
    assert(v.format == VIDEO_FORMAT_AV1);
    v = select_video(VIDEO_FORMAT_AV1, 10, 4, 0, 0, 0, size);
    assert(v.format == VIDEO_FORMAT_AV1 && queries == 0);
    v = select_video(VIDEO_FORMAT_AV1, 10, 0, 0, 1, 0, size);
    assert(v.format == VIDEO_FORMAT_DOLBY_VISION && video_dovi_use_base(&v));
    assert(video_dovi_colors_unsupported(&v));
    v = select_video(VIDEO_FORMAT_HEVC, 5, 0, 0, 0, 0, size);
    assert(v.format == VIDEO_FORMAT_DOLBY_VISION && queries == 0);
    assert(video_dovi_use_base(&v) && video_dovi_colors_unsupported(&v));
    /* Off is still an explicit override, and Force still requests DV. */
    v = select_video(VIDEO_FORMAT_AV1, 10, 0, 1, 0, 0, size);
    assert(v.format == VIDEO_FORMAT_AV1 && queries == 0);
    v = select_video(VIDEO_FORMAT_AV1, 10, 1, 2, 1, 0, size);
    assert(v.format == VIDEO_FORMAT_DOLBY_VISION);
    display_caps = 1; // HDR10-only display prefers compatible base in Auto.
    v = select_video(VIDEO_FORMAT_AV1, 10, 1, 0, 1, 1, size);
    assert(v.format == VIDEO_FORMAT_AV1);
    v = select_video(VIDEO_FORMAT_AV1, 10, 1, 2, 1, 1, size);
    assert(v.format == VIDEO_FORMAT_DOLBY_VISION);
    v = select_video(VIDEO_FORMAT_HEVC, 5, 0, 0, 1, 1, size);
    assert(v.format == VIDEO_FORMAT_DOLBY_VISION);
    display_caps = 8;
    v = select_video(VIDEO_FORMAT_AV1, 10, 1, 0, 1, 1, size);
    assert(v.format == VIDEO_FORMAT_DOLBY_VISION);
    /* Truncated records cannot supply profile/compatibility evidence. */
    v = select_video(VIDEO_FORMAT_AV1, 10, 1, 0, 1, 1, 1);
    assert(v.format == VIDEO_FORMAT_AV1 && !v.dv_bitstream_profile && !queries);
    return 0;
}
''')



if __name__ == '__main__':
    unittest.main()
