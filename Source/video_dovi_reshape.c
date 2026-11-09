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

#include "video_dovi_reshape.h"

#include <string.h>

#ifdef CONFIG_LIBDOVI
#include <libdovi/rpu_parser.h>

/* Inverse of the BT.2020-referred HPE LMS matrix that Dolby Vision outputs to
 * (libplacebo dovi_lms2rgb). */
static const double dovi_lms2rgb[9] = {
	 3.06441879, -2.16597676,  0.10155818,
	-0.65612108,  1.78554118, -0.12943749,
	 0.01736321, -0.04725154,  1.03004253,
};

static float fixed_coef(int64_t ipart, uint64_t fpart, double scale)
{
	/* Coefficients are ipart + fpart / 2^coef_log2_denom. */
	return (float)((double)ipart + (double)fpart * scale);
}

static int map_component(VIDEO_DOVI_RESHAPE *out, int c, const DoviReshapingCurve *curve,
	const DoviRpuDataHeader *header)
{
	const double coef_scale = 1.0 / (double)(1ULL << header->coefficient_log2_denom);
	const double pivot_scale = 1.0 / (double)(1 << (header->bl_bit_depth_minus8 + 8));
	int num_pivots = (int)curve->num_pivots_minus2 + 2;
	if (num_pivots > VIDEO_DOVI_RESHAPE_PIECES + 1 || (int)curve->pivots.len < num_pivots)
		return -1;

	/* The bitstream codes pivots as increments. */
	float pivots[VIDEO_DOVI_RESHAPE_PIECES + 1];
	unsigned pivot = 0;
	for (int i = 0; i < num_pivots; i++) {
		pivot += curve->pivots.data[i];
		pivots[i] = (float)(pivot * pivot_scale);
	}
	out->pivot_lo[c] = pivots[0];
	out->pivot_hi[c] = pivots[num_pivots - 1];
	for (int i = 0; i < VIDEO_DOVI_RESHAPE_PIECES - 1; i++)
		out->pivots[c][i] = i < num_pivots - 2 ? pivots[i + 1] : 1e9f;

	int pieces = num_pivots - 1;
	if (curve->polynomial) {
		const DoviPolynomialCurve *poly = curve->polynomial;
		if ((int)poly->poly_order_minus1.len < pieces || (int)poly->poly_coef_int.len < pieces ||
		    (int)poly->poly_coef.len < pieces)
			return -1;
		for (int i = 0; i < pieces; i++) {
			int order = (int)poly->poly_order_minus1.data[i] + 1;
			const DoviI64Data *ipart = poly->poly_coef_int.list[i];
			const DoviU64Data *fpart = poly->poly_coef.list[i];
			if (order > 2 || (int)ipart->len < order + 1 || (int)fpart->len < order + 1)
				return -1;
			for (int k = 0; k <= order; k++)
				out->coeffs[c][i][k] = fixed_coef(ipart->data[k], fpart->data[k], coef_scale);
			out->coeffs[c][i][3] = 0.0f;
		}
	} else if (curve->mmr) {
		const DoviMMRCurve *mmr = curve->mmr;
		if ((int)mmr->mmr_order_minus1.len < pieces || (int)mmr->mmr_constant_int.len < pieces ||
		    (int)mmr->mmr_constant.len < pieces || (int)mmr->mmr_coef_int.len < pieces ||
		    (int)mmr->mmr_coef.len < pieces)
			return -1;
		int row = 0;
		for (int i = 0; i < pieces; i++) {
			int order = mmr->mmr_order_minus1.data[i] + 1;
			const DoviI64Data2D *ipart = mmr->mmr_coef_int.list[i];
			const DoviU64Data2D *fpart = mmr->mmr_coef.list[i];
			if (order > 3 || (int)ipart->len < order || (int)fpart->len < order)
				return -1;
			out->coeffs[c][i][0] = fixed_coef(mmr->mmr_constant_int.data[i], mmr->mmr_constant.data[i],
				coef_scale);
			out->coeffs[c][i][1] = (float)row;
			out->coeffs[c][i][2] = 0.0f;
			out->coeffs[c][i][3] = (float)order;
			for (int j = 0; j < order; j++) {
				const DoviI64Data *wi = ipart->list[j];
				const DoviU64Data *wf = fpart->list[j];
				if (wi->len < 7 || wf->len < 7)
					return -1;
				float w[7];
				for (int k = 0; k < 7; k++)
					w[k] = fixed_coef(wi->data[k], wf->data[k], coef_scale);
				float *r0 = out->mmr[c][row], *r1 = out->mmr[c][row + 1];
				r0[0] = w[0]; r0[1] = w[1]; r0[2] = w[2]; r0[3] = 0.0f;
				r1[0] = w[3]; r1[1] = w[4]; r1[2] = w[5]; r1[3] = w[6];
				row += 2;
			}
		}
	} else {
		return -1;
	}
	return 0;
}

int video_dovi_reshape_params(const unsigned char *nal, int size, VIDEO_DOVI_RESHAPE *out)
{
	if (!nal || size <= 2 || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu(nal, (size_t)size);
	if (!rpu)
		return -1;
	const DoviRpuDataHeader *header = dovi_rpu_get_header(rpu);
	const DoviRpuDataMapping *mapping = header ? dovi_rpu_get_data_mapping(rpu) : NULL;
	const DoviVdrDmData *dm = mapping ? dovi_rpu_get_vdr_dm_data(rpu) : NULL;
	int ret = -1;
	/* Float coefficients and the 64-bit shift guard are not supported. */
	if (!dm || dovi_rpu_get_error(rpu) || header->coefficient_data_type != 0 ||
	    header->coefficient_log2_denom > 32 || header->bl_bit_depth_minus8 > 8)
		goto done;
	out->bl_bit_depth = (int)header->bl_bit_depth_minus8 + 8;
	ret = 0;
	for (int c = 0; c < 3 && !ret; c++)
		ret = map_component(out, c, &mapping->curves[c], header);
	if (ret)
		goto done;

	const int16_t ycc[9] = { dm->ycc_to_rgb_coef0, dm->ycc_to_rgb_coef1, dm->ycc_to_rgb_coef2,
		dm->ycc_to_rgb_coef3, dm->ycc_to_rgb_coef4, dm->ycc_to_rgb_coef5,
		dm->ycc_to_rgb_coef6, dm->ycc_to_rgb_coef7, dm->ycc_to_rgb_coef8 };
	const int16_t lms[9] = { dm->rgb_to_lms_coef0, dm->rgb_to_lms_coef1, dm->rgb_to_lms_coef2,
		dm->rgb_to_lms_coef3, dm->rgb_to_lms_coef4, dm->rgb_to_lms_coef5,
		dm->rgb_to_lms_coef6, dm->rgb_to_lms_coef7, dm->rgb_to_lms_coef8 };
	const uint32_t offset[3] = { dm->ycc_to_rgb_offset0, dm->ycc_to_rgb_offset1,
		dm->ycc_to_rgb_offset2 };
	for (int i = 0; i < 9; i++)
		out->ycc_to_rgb[i] = ycc[i] / 8192.0f;
	/* Profile 4 uses 2^30; this path only serves single-layer profiles. */
	for (int i = 0; i < 3; i++)
		out->ycc_offset[i] = (float)(offset[i] / (double)(1 << 28));
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			double sum = 0.0;
			for (int k = 0; k < 3; k++)
				sum += dovi_lms2rgb[i * 3 + k] * (lms[k * 3 + j] / 16384.0);
			out->lms_to_rgb[i * 3 + j] = (float)sum;
		}
	out->source_min_pq = dm->source_min_pq / 4095.0f;
	out->source_max_pq = dm->source_max_pq / 4095.0f;
done:
	if (dm)
		dovi_rpu_free_vdr_dm_data(dm);
	if (mapping)
		dovi_rpu_free_data_mapping(mapping);
	if (header)
		dovi_rpu_free_header(header);
	dovi_rpu_free(rpu);
	return ret;
}

#else

int video_dovi_reshape_params(const unsigned char *nal, int size, VIDEO_DOVI_RESHAPE *out)
{
	(void)nal;
	(void)size;
	(void)out;
	return -1;
}

#endif
