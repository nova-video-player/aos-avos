"""Dolby Vision reshaping parameters checked against FFmpeg's RPU parser.

video_dovi_reshape_params() feeds the Profile 5 GPU shader. FFmpeg decodes the
same RPU independently (ffprobe Dolby Vision Metadata side data), so the
pivots, polynomial coefficients and color matrices of a real Profile 5 frame
are compared field by field. Needs LIBDOVI_HOST_DIR (a host libdovi build),
ffmpeg/ffprobe and the Profile 5 sample at the workspace root.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from native_replay import ROOT
from test_dovi_rpu_probe import FIXTURES, SAMPLE, libdovi_dir, rpu_payload

# Keyframe at 8.058 s: an 8-piece luma polynomial (frame 0 is a black fade).
SAMPLE_SEEK = '8.06'

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video_dovi_reshape.h"

static unsigned char data[1 << 23];

/* First RPU NAL (type 62) of an Annex B file, or the raw NAL itself. */
static const unsigned char *find_rpu(int size, int *nal_size)
{
    if (data[0] == 0x7c && data[1] == 0x01) {
        *nal_size = size;
        return data;
    }
    for (int i = 0; i + 3 < size; i++) {
        if (data[i] || data[i + 1] || data[i + 2] != 1 || ((data[i + 3] >> 1) & 0x3f) != 62)
            continue;
        int start = i + 3, end = start + 2;
        while (end + 2 < size && !(data[end] == 0 && data[end + 1] == 0 &&
                                   (data[end + 2] == 1 || data[end + 2] == 0)))
            end++;
        if (end + 2 >= size)
            end = size;
        *nal_size = end - start;
        return data + start;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    int size = (int)fread(data, 1, sizeof(data), f);
    fclose(f);
    int nal_size = 0;
    const unsigned char *nal = find_rpu(size, &nal_size);
    assert(nal);
    VIDEO_DOVI_RESHAPE p;
    if (video_dovi_reshape_params(nal, nal_size, &p)) {
        printf("error\n");
        return 0;
    }
    printf("bl_bit_depth %d\n", p.bl_bit_depth);
    for (int c = 0; c < 3; c++) {
        printf("range %d %.9g %.9g\n", c, p.pivot_lo[c], p.pivot_hi[c]);
        printf("pivots %d", c);
        for (int i = 0; i < 7; i++)
            printf(" %.9g", p.pivots[c][i]);
        printf("\n");
        for (int i = 0; i < 8; i++)
            printf("coeffs %d %d %.9g %.9g %.9g %.9g\n", c, i, p.coeffs[c][i][0],
                   p.coeffs[c][i][1], p.coeffs[c][i][2], p.coeffs[c][i][3]);
    }
    printf("ycc");
    for (int i = 0; i < 9; i++)
        printf(" %.9g", p.ycc_to_rgb[i]);
    printf("\noffset %.9g %.9g %.9g\nlms", p.ycc_offset[0], p.ycc_offset[1], p.ycc_offset[2]);
    for (int i = 0; i < 9; i++)
        printf(" %.9g", p.lms_to_rgb[i]);
    printf("\npq %.9g %.9g\n", p.source_min_pq, p.source_max_pq);
    return 0;
}
'''

DOVI_LMS2RGB = [[3.06441879, -2.16597676, 0.10155818],
                [-0.65612108, 1.78554118, -0.12943749],
                [0.01736321, -0.04725154, 1.03004253]]


def run_harness(test, rpu_file):
    libdovi = libdovi_dir(test)
    with tempfile.TemporaryDirectory(prefix='avos-dovi-reshape-') as directory:
        binary = Path(directory) / 'reshape'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-DCONFIG_LIBDOVI', '-I' + str(ROOT / 'Include'),
                        '-I' + str(libdovi / 'include'), '-x', 'c', '-', '-x', 'none',
                        str(ROOT / 'Source/video_dovi_reshape.c'), str(libdovi / 'lib/libdovi.a'),
                        '-o', str(binary)], input=HARNESS.encode(), check=True)
        out = subprocess.run([str(binary), str(rpu_file)], check=True, capture_output=True,
                             text=True).stdout
    lines = {}
    for line in out.splitlines():
        key, *values = line.split()
        lines.setdefault(key, []).append(values)
    return lines


def ffprobe_metadata(path):
    """Dolby Vision Metadata of the first frame, as ffprobe prints it."""
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v', '-read_intervals',
                          '%+#1', '-show_frames', str(path)], check=True, capture_output=True,
                         text=True).stdout
    meta = {'components': []}
    for line in out.splitlines():
        if line == '[COMPONENT]':
            meta['components'].append({'pieces': []})
        elif '=' in line:
            key, value = line.split('=', 1)
            if key == 'pivots':
                meta['components'][-1]['pivots'] = [int(v) for v in value.split()]
            elif key == 'poly_coef':
                meta['components'][-1]['pieces'].append([int(v) for v in value.split()])
            elif key in ('mapping_idc', 'coef_log2_denom', 'bl_bit_depth', 'source_min_pq',
                         'source_max_pq') and key not in meta:
                meta[key] = int(value)
            elif key in ('ycc_to_rgb_matrix', 'ycc_to_rgb_offset', 'rgb_to_lms_matrix'):
                meta[key] = [int(n) / int(d) for n, d in (v.split('/') for v in value.split())]
    return meta


class DoviReshapeParamsTest(unittest.TestCase):
    def assertClose(self, actual, expected, tol=1e-6):
        self.assertLessEqual(abs(float(actual) - expected), tol * max(1.0, abs(expected)),
                             f'{actual} != {expected}')

    def test_profile5_frame_matches_ffmpeg(self):
        libdovi_dir(self)
        if not shutil.which('ffmpeg') or not shutil.which('ffprobe') or not SAMPLE.is_file():
            self.skipTest('ffmpeg/ffprobe or the Profile 5 sample is unavailable')
        with tempfile.TemporaryDirectory(prefix='avos-dovi-p5-') as directory:
            frame = Path(directory) / 'frame.hevc'
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', SAMPLE_SEEK, '-i', str(SAMPLE),
                            '-map', '0:v:0', '-c', 'copy', '-bsf:v', 'hevc_mp4toannexb',
                            '-frames:v', '1', '-f', 'hevc', str(frame)], check=True)
            ref = ffprobe_metadata(frame)
            got = run_harness(self, frame)
        self.assertNotIn('error', got)
        self.assertEqual(ref['mapping_idc'], 0)  # polynomial only in this sample
        depth, denom = ref['bl_bit_depth'], 2 ** ref['coef_log2_denom']
        self.assertEqual(int(got['bl_bit_depth'][0][0]), depth)
        self.assertGreater(len(ref['components'][0]['pivots']), 2)
        for c, comp in enumerate(ref['components']):
            pivots = [p / 2 ** depth for p in comp['pivots']]
            _, lo, hi = got['range'][c]
            self.assertClose(lo, pivots[0])
            self.assertClose(hi, pivots[-1])
            inner = got['pivots'][c][1:]
            for i in range(7):
                self.assertClose(inner[i], pivots[i + 1] if i < len(pivots) - 2 else 1e9)
            for i, coef in enumerate(comp['pieces']):
                row = [float(v) for v in got['coeffs'][c * 8 + i][2:]]
                expected = [k / denom for k in coef] + [0.0] * (3 - len(coef))
                for k in range(3):
                    self.assertClose(row[k], expected[k], 1e-6)
                self.assertEqual(row[3], 0.0)
        for got_v, ref_v in zip(got['ycc'][0], ref['ycc_to_rgb_matrix']):
            self.assertClose(got_v, ref_v)
        for got_v, ref_v in zip(got['offset'][0], ref['ycc_to_rgb_offset']):
            self.assertClose(got_v, ref_v)
        lms = ref['rgb_to_lms_matrix']
        for i in range(3):
            for j in range(3):
                expected = sum(DOVI_LMS2RGB[i][k] * lms[k * 3 + j] for k in range(3))
                self.assertClose(got['lms'][0][i * 3 + j], expected, 1e-5)
        self.assertClose(got['pq'][0][0], ref['source_min_pq'] / 4095)
        self.assertClose(got['pq'][0][1], ref['source_max_pq'] / 4095)

    def test_profile7_fixtures_parse(self):
        """Fixtures with MMR chroma (dovi_tool samples) map without error."""
        for name in ('mel_orig.bin', 'fel_orig.bin'):
            with self.subTest(name), tempfile.TemporaryDirectory() as directory:
                nal = Path(directory) / 'rpu.bin'
                nal.write_bytes(b'\x7c\x01' + rpu_payload(name))
                got = run_harness(self, nal)
                self.assertNotIn('error', got)
                for c in range(3):
                    self.assertLessEqual(float(got['range'][c][1]), float(got['range'][c][2]))


if __name__ == '__main__':
    unittest.main()
