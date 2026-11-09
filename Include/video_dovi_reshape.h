/*
 * Copyright 2026 Courville Software
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VIDEO_DOVI_RESHAPE_H
#define VIDEO_DOVI_RESHAPE_H

/* Dolby Vision single-layer reshaping (Profile 5 without a DV decoder):
 * per-frame RPU parameters in the layout of the shader in
 * video_dovi_reshape_glsl.h. The math follows libplacebo's
 * pl_shader_dovi_reshape() and pl_shader_decode_color(). */

#define VIDEO_DOVI_RESHAPE_PIECES 8
#define VIDEO_DOVI_RESHAPE_MMR_ROWS (VIDEO_DOVI_RESHAPE_PIECES * 6)

typedef struct {
	/* Inner pivots per component, normalized to code / 2^bl_bit_depth;
	 * unused entries hold a 1e9 sentinel. */
	float pivots[3][VIDEO_DOVI_RESHAPE_PIECES - 1];
	/* Per piece: polynomial (c0, c1, c2, 0) or MMR (constant, first MMR row,
	 * 0, order 1..3). */
	float coeffs[3][VIDEO_DOVI_RESHAPE_PIECES][4];
	/* MMR weights, two rows per order: (w0, w1, w2, 0), (w3, w4, w5, w6). */
	float mmr[3][VIDEO_DOVI_RESHAPE_MMR_ROWS][4];
	float pivot_lo[3], pivot_hi[3];
	float ycc_to_rgb[9];  /* row-major */
	float ycc_offset[3];
	float lms_to_rgb[9];  /* row-major, linear LMS to linear BT.2020 RGB */
	float source_min_pq, source_max_pq; /* 0..1 */
	int bl_bit_depth;
} VIDEO_DOVI_RESHAPE;

/* Fill `out` from one RPU NAL unit (type 62, starting at its two-byte NAL
 * header, still escaped). Returns 0 on success, -1 when the RPU cannot be
 * parsed or uses an unsupported coding (float coefficients); always -1 without
 * CONFIG_LIBDOVI. */
int video_dovi_reshape_params(const unsigned char *nal, int size, VIDEO_DOVI_RESHAPE *out);

#endif
