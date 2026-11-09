"""Replay the read-only Dolby Vision RPU probe: NAL lookup and libdovi parsing.

The libdovi case needs a host build of libdovi (LIBDOVI_HOST_DIR containing
include/libdovi/rpu_parser.h and lib/libdovi.a), ffmpeg, and the workspace
Profile 5 sample; it is skipped when any of them is missing.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from native_replay import ROOT, run_c, source

SAMPLE = ROOT.parents[1] / 'DolbyVision_Profile5_GlassBlowing.mp4'


def production():
    text = source('video_dovi_rpu.c')
    header = (ROOT / 'Include/video_dovi_rpu.h').read_text()
    defines = header[header.index('#define VIDEO_DOVI_NAL_RPU'):header.index('/* Find the first')]
    return defines, text[text.index('int video_dovi_find_hevc_nal('):]


PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define AV_CODEC_ID_HEVC 173
#define serprintf printf
typedef struct {
    int codec_id, dv_bitstream_profile, dv_rpu_probe_count, dv_rpu_profile, dv_rpu_el_type;
} VIDEO_PROPERTIES;
'''


class DolbyVisionRpuProbeTest(unittest.TestCase):
    def test_hevc_nal_lookup(self):
        defines, body = production()
        run_c(PRELUDE + defines + body + r'''
static const unsigned char vps[] = {0x40, 0x01, 0x0c};
static const unsigned char rpu[] = {0x7c, 0x01, 0x19, 0x08, 0x80};
static const unsigned char el[] = {0x7e, 0x01, 0x55};
static int prefixed(unsigned char *out, int prefix, const unsigned char *const *nals, const int *sizes, int count) {
    int pos = 0;
    for (int i = 0; i < count; i++) {
        for (int b = prefix - 1; b >= 0; b--) out[pos++] = (unsigned char)(sizes[i] >> (8 * b));
        memcpy(out + pos, nals[i], sizes[i]);
        pos += sizes[i];
    }
    return pos;
}
int main(void) {
    const unsigned char *nal = NULL;
    int size = 0;
    /* Annex B with 4- and 3-byte start codes and trailing zero bytes. */
    unsigned char annexb[] = {0, 0, 0, 1, 0x40, 0x01, 0x0c, 0, 0, 1, 0x7c, 0x01, 0x19, 0x08, 0x80, 0, 0,
                              0, 0, 1, 0x7e, 0x01, 0x55};
    assert(video_dovi_find_hevc_nal(annexb, sizeof(annexb), 0, VIDEO_DOVI_NAL_RPU, &nal, &size));
    assert(size == (int)sizeof(rpu) && !memcmp(nal, rpu, sizeof(rpu)));
    assert(video_dovi_find_hevc_nal(annexb, sizeof(annexb), 0, VIDEO_DOVI_NAL_EL, &nal, &size));
    assert(size == (int)sizeof(el) && !memcmp(nal, el, sizeof(el)));
    assert(!video_dovi_find_hevc_nal(annexb, sizeof(annexb), 0, 39, &nal, &size));

    /* hvcC length prefixes of every legal size. */
    const unsigned char *nals[] = {vps, rpu};
    const int sizes[] = {sizeof(vps), sizeof(rpu)};
    for (int prefix = 1; prefix <= 4; prefix++) {
        unsigned char au[32];
        int len = prefixed(au, prefix, nals, sizes, 2);
        assert(video_dovi_find_hevc_nal(au, len, prefix, VIDEO_DOVI_NAL_RPU, &nal, &size));
        assert(size == (int)sizeof(rpu) && !memcmp(nal, rpu, sizeof(rpu)));
        assert(!video_dovi_find_hevc_nal(au, len, prefix, VIDEO_DOVI_NAL_EL, &nal, &size));
        /* A length running past the packet stops the walk. */
        assert(!video_dovi_find_hevc_nal(au, len - 1, prefix, VIDEO_DOVI_NAL_RPU, &nal, &size));
    }
    unsigned char au[32];
    int len = prefixed(au, 4, nals, sizes, 2);
    assert(!video_dovi_find_hevc_nal(au, len, 5, VIDEO_DOVI_NAL_RPU, &nal, &size));
    assert(!video_dovi_find_hevc_nal(NULL, len, 4, VIDEO_DOVI_NAL_RPU, &nal, &size));
    assert(!video_dovi_find_hevc_nal(au, 0, 4, VIDEO_DOVI_NAL_RPU, &nal, &size));
    return 0;
}
''')

    def test_probe_without_libdovi_is_a_no_op(self):
        defines, body = production()
        run_c(PRELUDE + defines + body + r'''
int main(void) {
    unsigned char au[] = {0, 0, 1, 0x7c, 0x01, 0x80};
    VIDEO_PROPERTIES video = {AV_CODEC_ID_HEVC, 5, 0, 0, 0};
    video_dovi_rpu_probe(&video, au, sizeof(au), 0);
    assert(!video.dv_rpu_probe_count && !video.dv_rpu_profile && !video.dv_rpu_el_type);
    return 0;
}
''')

    def test_probe_parses_profile5_sample(self):
        libdovi = os.environ.get('LIBDOVI_HOST_DIR')
        if not libdovi or not (Path(libdovi) / 'lib/libdovi.a').is_file():
            self.skipTest('LIBDOVI_HOST_DIR with a host libdovi build is not set')
        if not shutil.which('ffmpeg') or not SAMPLE.is_file():
            self.skipTest('ffmpeg or the Profile 5 sample is unavailable')
        defines, body = production()
        with tempfile.TemporaryDirectory(prefix='avos-dovi-') as directory:
            directory = Path(directory)
            access_units = directory / 'p5.hevc'
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(SAMPLE), '-map', '0:v:0',
                            '-c', 'copy', '-bsf:v', 'hevc_mp4toannexb', '-frames:v', '3',
                            '-f', 'hevc', str(access_units)], check=True)
            replay = directory / 'replay.c'
            binary = directory / 'replay'
            replay.write_text('#define CONFIG_LIBDOVI 1\n' + PRELUDE +
                              '#include <stdlib.h>\n#include <libdovi/rpu_parser.h>\n' +
                              defines + body + r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    static unsigned char data[1 << 22];
    int size = (int)fread(data, 1, sizeof(data), f);
    fclose(f);
    assert(size > 0 && size < (int)sizeof(data));
    VIDEO_PROPERTIES video = {AV_CODEC_ID_HEVC, 5, 0, 0, 0};
    video_dovi_rpu_probe(&video, data, size, 0);
    assert(video.dv_rpu_profile == 5);
    assert(video.dv_rpu_el_type == VIDEO_DOVI_EL_NONE);
    assert(video.dv_rpu_probe_count == VIDEO_DOVI_RPU_PROBE_PACKETS);
    /* Probing stops once an RPU has been parsed. */
    video.dv_rpu_profile = 0;
    video_dovi_rpu_probe(&video, data, size, 0);
    assert(video.dv_rpu_profile == 0);
    /* Non-DV and non-HEVC streams are ignored. */
    VIDEO_PROPERTIES hevc = {AV_CODEC_ID_HEVC, 0, 0, 0, 0};
    video_dovi_rpu_probe(&hevc, data, size, 0);
    assert(!hevc.dv_rpu_probe_count);
    VIDEO_PROPERTIES other = {27, 5, 0, 0, 0};
    video_dovi_rpu_probe(&other, data, size, 0);
    assert(!other.dv_rpu_probe_count);
    return 0;
}
''')
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(Path(libdovi) / 'include'), str(replay),
                            str(Path(libdovi) / 'lib/libdovi.a'), '-o', str(binary)], check=True)
            subprocess.run([str(binary), str(access_units)], check=True)


if __name__ == '__main__':
    unittest.main()
