"""Replay the Dolby Vision RPU probe and bitstream rewrite.

NAL lookup, HDR10+ SEI removal and option masking run everywhere. The libdovi
cases need a host build of libdovi (LIBDOVI_HOST_DIR containing
include/libdovi/rpu_parser.h and lib/libdovi.a) and are skipped otherwise; the
Profile 5 probe also needs ffmpeg and the workspace sample.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from native_replay import ROOT, run_c, source

SAMPLE = ROOT.parents[1] / 'DolbyVision_Profile5_GlassBlowing.mp4'
FIXTURES = ROOT / 'test/fixtures/dovi'


def production():
    text = source('video_dovi_rpu.c')
    header = (ROOT / 'Include/video_dovi_rpu.h').read_text()
    defines = header[header.index('#define VIDEO_DOVI_NAL_RPU'):header.index('/* Return the next HEVC NAL')]
    return defines, text[text.index('int video_dovi_next_hevc_nal('):]


PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define AV_CODEC_ID_HEVC 173
#define serprintf printf
typedef struct {
    int codec_id, dv_bitstream_profile, dv_rpu_probe_count, dv_rpu_profile, dv_rpu_el_type;
    int dv_nal_unit_size, dv_rewrite, dv_rewrite_logged, dv_rewrite_errors;
} VIDEO_PROPERTIES;
'''


def libdovi_dir(test):
    libdovi = os.environ.get('LIBDOVI_HOST_DIR')
    if not libdovi or not (Path(libdovi) / 'lib/libdovi.a').is_file():
        test.skipTest('LIBDOVI_HOST_DIR with a host libdovi build is not set')
    return Path(libdovi)


def run_libdovi(libdovi, code, *args):
    defines, body = production()
    with tempfile.TemporaryDirectory(prefix='avos-dovi-') as directory:
        replay = Path(directory) / 'replay.c'
        binary = Path(directory) / 'replay'
        replay.write_text('#define CONFIG_LIBDOVI 1\n' + PRELUDE +
                          '#include <libdovi/rpu_parser.h>\n' + defines + body + code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(libdovi / 'include'), str(replay),
                        str(libdovi / 'lib/libdovi.a'), '-o', str(binary)], check=True)
        subprocess.run([str(binary), *map(str, args)], check=True)


def unescape(payload):
    out, zeros = bytearray(), 0
    for byte in payload:
        if zeros >= 2 and byte == 3:
            zeros = 0
            continue
        out.append(byte)
        zeros = zeros + 1 if byte == 0 else 0
    return bytes(out)


def rpu_payload(name):
    """First RPU of a dovi_tool .bin file: start code + escaped payload, no NAL header."""
    data = (FIXTURES / name).read_bytes()
    assert data[:4] == b'\0\0\0\1', name
    end = data.find(b'\0\0\0\1', 4)
    return data[4:end if end > 0 else len(data)]


def nals(data, prefix):
    pos, out = 0, []
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + prefix], 'big')
        pos += prefix
        out.append(data[pos:pos + length])
        pos += length
    return out


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
        int pos = 0;
        while (video_dovi_next_hevc_nal(au, len - 1, prefix, &pos, &nal, &size) == 1) {}
        assert(video_dovi_next_hevc_nal(au, len - 1, prefix, &pos, &nal, &size) == -1);
    }
    unsigned char au[32];
    int len = prefixed(au, 4, nals, sizes, 2);
    assert(!video_dovi_find_hevc_nal(au, len, 5, VIDEO_DOVI_NAL_RPU, &nal, &size));
    assert(!video_dovi_find_hevc_nal(NULL, len, 4, VIDEO_DOVI_NAL_RPU, &nal, &size));
    assert(!video_dovi_find_hevc_nal(au, 0, 4, VIDEO_DOVI_NAL_RPU, &nal, &size));

    /* hvcC extradata selects the framing; anything else is Annex B. */
    unsigned char hvcc[23] = {1};
    hvcc[21] = 0xfc | 3;
    assert(video_dovi_hevc_nal_unit_size(hvcc, sizeof(hvcc)) == 4);
    hvcc[21] = 0xfc | 1;
    assert(video_dovi_hevc_nal_unit_size(hvcc, sizeof(hvcc)) == 2);
    assert(video_dovi_hevc_nal_unit_size(hvcc, 22) == 0);
    unsigned char annexb_extra[] = {0, 0, 0, 1, 0x40, 0x01};
    assert(video_dovi_hevc_nal_unit_size(annexb_extra, sizeof(annexb_extra)) == 0);
    return 0;
}
''')

    def test_probe_without_libdovi_is_a_no_op(self):
        defines, body = production()
        run_c(PRELUDE + defines + body + r'''
int main(void) {
    unsigned char au[] = {0, 0, 1, 0x7c, 0x01, 0x80};
    VIDEO_PROPERTIES video = {AV_CODEC_ID_HEVC, 5, 0, 0, 0, 0, 0, 0, 0};
    video_dovi_rpu_probe(&video, au, sizeof(au), 0);
    assert(!video.dv_rpu_probe_count && !video.dv_rpu_profile && !video.dv_rpu_el_type);
    return 0;
}
''')

    def test_hdr10plus_sei_removal_and_option_masking(self):
        defines, body = production()
        run_c(PRELUDE + defines + body + r'''
static int put(unsigned char *out, int pos, const unsigned char *nal, int len, int prefix) {
    if (prefix) {
        for (int b = prefix - 1; b >= 0; b--) out[pos++] = (unsigned char)(len >> (8 * b));
    } else {
        out[pos++] = 0; out[pos++] = 0; out[pos++] = 0; out[pos++] = 1;
    }
    memcpy(out + pos, nal, len);
    return pos + len;
}
int main(void) {
    static const unsigned char vps[] = {0x40, 0x01, 0x0c, 0x01};
    static const unsigned char el[] = {0x7e, 0x01, 0x55, 0x66};
    /* Prefix SEI: mastering display (137), HDR10+ T.35 (4), and user data
     * unregistered (5) whose payload needs emulation prevention. */
    unsigned char sei[80];
    int n = 0;
    sei[n++] = 0x4e; sei[n++] = 0x01;
    sei[n++] = 0x89; sei[n++] = 24;
    for (int i = 0; i < 24; i++) sei[n++] = 0x11;
    static const unsigned char hdr10plus[] = {0x04, 0x08, 0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04, 0x01, 0x40};
    memcpy(sei + n, hdr10plus, sizeof(hdr10plus));
    n += sizeof(hdr10plus);
    static const unsigned char unregistered[] = {0x05, 0x04, 0x00, 0x00, 0x03, 0x02, 0x22};
    memcpy(sei + n, unregistered, sizeof(unregistered));
    n += sizeof(unregistered);
    sei[n++] = 0x80;

    unsigned char expected[80];
    int e = 0;
    memcpy(expected, sei, 28);
    e = 28;
    memcpy(expected + e, unregistered, sizeof(unregistered));
    e += sizeof(unregistered);
    expected[e++] = 0x80;

    for (int prefix = 0; prefix <= 4; prefix += 2) {
        unsigned char au[256];
        int len = put(au, 0, vps, sizeof(vps), prefix);
        len = put(au, len, sei, n, prefix);
        len = put(au, len, el, sizeof(el), prefix);
        VIDEO_PROPERTIES video = {AV_CODEC_ID_HEVC, 8, 0, 0, 0, prefix, VIDEO_DOVI_REWRITE_STRIP_HDR10PLUS, 0, 0};
        int out_size = 0;
        unsigned char *out = video_dovi_rewrite_au(&video, au, len, prefix, &out_size);
        assert(out);
        unsigned char want[256];
        int want_len = put(want, 0, vps, sizeof(vps), prefix);
        want_len = put(want, want_len, expected, e, prefix);
        want_len = put(want, want_len, el, sizeof(el), prefix);
        assert(out_size == want_len && !memcmp(out, want, want_len));
        free(out);

        /* An SEI NAL that only carried HDR10+ is dropped. */
        unsigned char only[16] = {0x4e, 0x01};
        memcpy(only + 2, hdr10plus, sizeof(hdr10plus));
        only[2 + sizeof(hdr10plus)] = 0x80;
        len = put(au, 0, vps, sizeof(vps), prefix);
        len = put(au, len, only, 3 + sizeof(hdr10plus), prefix);
        out = video_dovi_rewrite_au(&video, au, len, prefix, &out_size);
        want_len = put(want, 0, vps, sizeof(vps), prefix);
        assert(out && out_size == want_len && !memcmp(out, want, want_len));
        free(out);

        /* No HDR10+ message: the packet is kept as is. */
        len = put(au, 0, vps, sizeof(vps), prefix);
        len = put(au, len, expected, e, prefix);
        assert(!video_dovi_rewrite_au(&video, au, len, prefix, &out_size) && !out_size);

        /* Without libdovi the Profile 7 option is unavailable, so the EL stays. */
        video.dv_rewrite = VIDEO_DOVI_REWRITE_P7_TO_P81;
        len = put(au, 0, vps, sizeof(vps), prefix);
        len = put(au, len, el, sizeof(el), prefix);
        assert(!video_dovi_rewrite_au(&video, au, len, prefix, &out_size));
        video.dv_rewrite = 0;
        assert(!video_dovi_rewrite_au(&video, au, len, prefix, &out_size));
    }
    assert(video_dovi_rewrite_supported() == VIDEO_DOVI_REWRITE_STRIP_HDR10PLUS);
    return 0;
}
''')

    def convert_profile7(self, name, el_type):
        libdovi = libdovi_dir(self)
        vps = bytes([0x40, 0x01, 0x0c, 0x01])
        slice_nal = bytes([0x26, 0x01, 0xaf, 0x10])
        rpu_nal = bytes([0x7c, 0x01]) + rpu_payload(name)
        el_nal = bytes([0x7e, 0x01, 0x55, 0x66])
        au = b''.join(len(n).to_bytes(4, 'big') + n for n in (vps, rpu_nal, el_nal, slice_nal))
        with tempfile.TemporaryDirectory(prefix='avos-dovi-p7-') as directory:
            src = Path(directory) / 'in.bin'
            dst = Path(directory) / 'out.bin'
            src.write_bytes(au)
            run_libdovi(libdovi, r"""
int main(int argc, char **argv) {
    assert(argc == 4);
    static unsigned char data[1 << 16];
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    int size = (int)fread(data, 1, sizeof(data), f);
    fclose(f);
    VIDEO_PROPERTIES video = {AV_CODEC_ID_HEVC, 7, 0, 0, 0, 4, VIDEO_DOVI_REWRITE_P7_TO_P81, 0, 0};
    video_dovi_rpu_probe(&video, data, size, 4);
    assert(video.dv_rpu_profile == 7 && video.dv_rpu_el_type == atoi(argv[3]));
    int out_size = 0;
    unsigned char *out = video_dovi_rewrite_au(&video, data, size, 4, &out_size);
    assert(out && out_size > 0 && video.dv_rewrite_logged && !video.dv_rewrite_errors);
    VIDEO_PROPERTIES check = {AV_CODEC_ID_HEVC, 8, 0, 0, 0, 4, 0, 0, 0};
    video_dovi_rpu_probe(&check, out, out_size, 4);
    assert(check.dv_rpu_profile == 8 && check.dv_rpu_el_type == VIDEO_DOVI_EL_NONE);
    f = fopen(argv[2], "wb");
    assert(f && fwrite(out, 1, out_size, f) == (size_t)out_size);
    fclose(f);
    free(out);

    /* A corrupt RPU keeps the packet unchanged and is counted. */
    VIDEO_PROPERTIES broken = {AV_CODEC_ID_HEVC, 7, 0, 0, 0, 4, VIDEO_DOVI_REWRITE_P7_TO_P81, 0, 0};
    unsigned char bad[] = {0, 0, 0, 5, 0x7c, 0x01, 0x19, 0xff, 0xff};
    assert(!video_dovi_rewrite_au(&broken, bad, sizeof(bad), 4, &out_size));
    assert(broken.dv_rewrite_errors == 1);
    return 0;
}
""", src, dst, el_type)
            result = nals(dst.read_bytes(), 4)
        self.assertEqual(len(result), 3, 'the enhancement-layer NAL must be dropped')
        self.assertEqual([result[0], result[1][:2], result[2]], [vps, b'\x7c\x01', slice_nal])
        return result[1]

    def test_profile7_mel_rpu_converted_to_profile81(self):
        rpu = self.convert_profile7('mel_orig.bin', 2)
        self.assertEqual(unescape(rpu[2:]), unescape(rpu_payload('mel_to_81.bin')))

    def test_profile7_fel_rpu_converted_to_profile81(self):
        self.convert_profile7('fel_orig.bin', 3)

    def test_probe_parses_profile5_sample(self):
        libdovi = libdovi_dir(self)
        if not shutil.which('ffmpeg') or not SAMPLE.is_file():
            self.skipTest('ffmpeg or the Profile 5 sample is unavailable')
        with tempfile.TemporaryDirectory(prefix='avos-dovi-p5-') as directory:
            access_units = Path(directory) / 'p5.hevc'
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(SAMPLE), '-map', '0:v:0',
                            '-c', 'copy', '-bsf:v', 'hevc_mp4toannexb', '-frames:v', '3',
                            '-f', 'hevc', str(access_units)], check=True)
            run_libdovi(libdovi, r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    static unsigned char data[1 << 22];
    int size = (int)fread(data, 1, sizeof(data), f);
    fclose(f);
    assert(size > 0 && size < (int)sizeof(data));
    VIDEO_PROPERTIES video = {AV_CODEC_ID_HEVC, 5, 0, 0, 0, 0, 0, 0, 0};
    video_dovi_rpu_probe(&video, data, size, 0);
    assert(video.dv_rpu_profile == 5);
    assert(video.dv_rpu_el_type == VIDEO_DOVI_EL_NONE);
    assert(video.dv_rpu_probe_count == VIDEO_DOVI_RPU_PROBE_PACKETS);
    /* Probing stops once an RPU has been parsed. */
    video.dv_rpu_profile = 0;
    video_dovi_rpu_probe(&video, data, size, 0);
    assert(video.dv_rpu_profile == 0);
    /* Non-DV and non-HEVC streams are ignored. */
    VIDEO_PROPERTIES hevc = {AV_CODEC_ID_HEVC, 0, 0, 0, 0, 0, 0, 0, 0};
    video_dovi_rpu_probe(&hevc, data, size, 0);
    assert(!hevc.dv_rpu_probe_count);
    VIDEO_PROPERTIES other = {27, 5, 0, 0, 0, 0, 0, 0, 0};
    video_dovi_rpu_probe(&other, data, size, 0);
    assert(!other.dv_rpu_probe_count);
    /* Profile 5 is never converted: no rewrite option touches its packets. */
    video.dv_rewrite = VIDEO_DOVI_REWRITE_P7_TO_P81;
    int out_size = 0;
    assert(!video_dovi_rewrite_au(&video, data, size, 0, &out_size));
    return 0;
}
''', access_units)


if __name__ == '__main__':
    unittest.main()
