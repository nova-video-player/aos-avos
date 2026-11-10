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

#include <stdint.h>

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
	/* Static HDR metadata for the display: mastering luminance (nits) from
	 * the L6 block, else the source range; MaxCLL / MaxFALL (nits) from L6,
	 * 0 when unknown. */
	float mastering_min_nits, mastering_max_nits;
	int max_cll, max_fall;
	int bl_bit_depth;
} VIDEO_DOVI_RESHAPE;

/* Fill `out` from one RPU NAL unit (type 62, starting at its two-byte NAL
 * header, still escaped). Returns 0 on success, -1 when the RPU cannot be
 * parsed or uses an unsupported coding (float coefficients); always -1 without
 * CONFIG_LIBDOVI. */
int video_dovi_reshape_params(const unsigned char *nal, int size, VIDEO_DOVI_RESHAPE *out);

/* RPUs may reuse an earlier mapping (use_prev_vdr_rpu_flag, by vdr_rpu_id) and
 * omit display management data, which then stays as last sent. Feed every RPU
 * of a stream in decode order; reset on flush/seek. */
#define VIDEO_DOVI_RESHAPE_IDS 16
typedef struct {
	VIDEO_DOVI_RESHAPE params; /* complete parameters of the last RPU */
	VIDEO_DOVI_RESHAPE mappings[VIDEO_DOVI_RESHAPE_IDS]; /* curves by vdr_rpu_id */
	unsigned mapping_valid;    /* bit per vdr_rpu_id */
	int dm_valid;
} VIDEO_DOVI_RESHAPE_STATE;

void video_dovi_reshape_reset(VIDEO_DOVI_RESHAPE_STATE *state);
/* Update `state` from the next RPU NAL unit (as video_dovi_reshape_params) and
 * resolve state->params. Returns 0 when they are complete for this RPU, -1
 * otherwise (unparsable, or referencing a mapping or DM data not received
 * since the last reset). */
int video_dovi_reshape_update(VIDEO_DOVI_RESHAPE_STATE *state, const unsigned char *nal, int size);

/* CPU version of the shader's SDR output (thumbnails): reshape one 4:2:0 frame
 * of 16-bit samples holding bl_bit_depth-bit codes into BGRA, alpha 255.
 * Strides are in samples for the planes, in bytes for dst. */
void video_dovi_reshape_bgra(const VIDEO_DOVI_RESHAPE *p, const uint16_t *const planes[3],
	const int strides[3], int width, int height, uint8_t *dst, int dst_stride);

#endif
