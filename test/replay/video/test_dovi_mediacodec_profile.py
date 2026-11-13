"""Replay the production DV profile selection and MediaFormat configuration."""
import unittest

from native_replay import ROOT, extract, run_c, source


class DolbyVisionMediaCodecProfileTest(unittest.TestCase):
    def test_profile_is_configured_only_for_dolby_vision(self):
        caller = source('codec_sfdec2.c')
        start = caller.index('int dovi_profile = effective_format')
        selection = caller[start:caller.index(';', start) + 1]
        backend = (ROOT / 'external/android/libsfdec/sfdec_ndkmediacodec.cpp').read_text()
        configure = extract(backend, 'if (codec == SFDEC_VIDEO_DOLBY_VISION && dovi_profile > 0)')
        run_c(r'''
#include <assert.h>
#include <string.h>
#define DBG if (0)
#define LOG(...) ((void)0)
enum { VIDEO_FORMAT_HEVC, VIDEO_FORMAT_H264, VIDEO_FORMAT_AV1, VIDEO_FORMAT_DOLBY_VISION };
enum { SFDEC_VIDEO_HEVC, SFDEC_VIDEO_H264, SFDEC_VIDEO_AV1, SFDEC_VIDEO_DOLBY_VISION };
static int writes, last_profile;
static void AMediaFormat_setInt32(void *format, const char *key, int value) {
    (void)format;
    assert(!strcmp(key, "profile"));
    writes++;
    last_profile = value;
}
static void backend_configure(int codec, int dovi_profile) {
    struct { void *mFormat; } instance = {0}, *sfdec = &instance;
''' + configure + r'''
}
static void open_decoder(int effective_format, int codec, int mapped_profile) {
    struct { int dv_profile; } props = {mapped_profile}, *video = &props;
''' + selection + r'''
    backend_configure(codec, dovi_profile);
}
int main(void) {
    /* Android enum values, not raw bitstream profile numbers. Include the
     * existing phh mapping for P7/P8 and the AVC/AV1 profiles. */
    const int profiles[] = {16, 32, 256, 512, 1024};
    for (unsigned i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        writes = last_profile = 0;
        open_decoder(VIDEO_FORMAT_DOLBY_VISION, SFDEC_VIDEO_DOLBY_VISION, profiles[i]);
        assert(writes == 1 && last_profile == profiles[i]);
        // Every new decoder candidate needs the same profile configuration.
        open_decoder(VIDEO_FORMAT_DOLBY_VISION, SFDEC_VIDEO_DOLBY_VISION, profiles[i]);
        assert(writes == 2 && last_profile == profiles[i]);
    }
    writes = 0;
    open_decoder(VIDEO_FORMAT_DOLBY_VISION, SFDEC_VIDEO_DOLBY_VISION, 0);
    open_decoder(VIDEO_FORMAT_DOLBY_VISION, SFDEC_VIDEO_DOLBY_VISION, -1);
    // DV metadata survives fallback, but must not become an HEVC/AVC/AV1 profile.
    open_decoder(VIDEO_FORMAT_HEVC, SFDEC_VIDEO_HEVC, 32);
    open_decoder(VIDEO_FORMAT_H264, SFDEC_VIDEO_H264, 512);
    open_decoder(VIDEO_FORMAT_AV1, SFDEC_VIDEO_AV1, 1024);
    open_decoder(VIDEO_FORMAT_HEVC, SFDEC_VIDEO_HEVC, 0);
    assert(writes == 0);
    // The backend also guards against a caller supplying a DV profile for HEVC.
    backend_configure(SFDEC_VIDEO_HEVC, 32);
    assert(writes == 0);
    return 0;
}
''')
