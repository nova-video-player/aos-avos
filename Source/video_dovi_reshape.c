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

#include <math.h>
#include <stdlib.h>
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

static float pq_nits(double e)
{
	const double p = pow(e, 1.0 / 78.84375);
	return (float)(10000.0 * pow(fmax(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125));
}

static void fill_dm(VIDEO_DOVI_RESHAPE *out, const DoviVdrDmData *dm)
{
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
	const DoviExtMetadataBlockLevel6 *l6 = dm->dm_data.level6;
	out->mastering_max_nits = l6 && l6->max_display_mastering_luminance ?
		(float)l6->max_display_mastering_luminance : pq_nits(out->source_max_pq);
	out->mastering_min_nits = l6 && l6->max_display_mastering_luminance ?
		l6->min_display_mastering_luminance / 10000.0f : pq_nits(out->source_min_pq);
	out->max_cll = l6 ? l6->max_content_light_level : 0;
	out->max_fall = l6 ? l6->max_frame_average_light_level : 0;
}

static void copy_mapping(VIDEO_DOVI_RESHAPE *dst, const VIDEO_DOVI_RESHAPE *src)
{
	memcpy(dst->pivots, src->pivots, sizeof(dst->pivots));
	memcpy(dst->coeffs, src->coeffs, sizeof(dst->coeffs));
	memcpy(dst->mmr, src->mmr, sizeof(dst->mmr));
	memcpy(dst->pivot_lo, src->pivot_lo, sizeof(dst->pivot_lo));
	memcpy(dst->pivot_hi, src->pivot_hi, sizeof(dst->pivot_hi));
}

void video_dovi_reshape_reset(VIDEO_DOVI_RESHAPE_STATE *state)
{
	if (state) {
		state->mapping_valid = 0;
		state->dm_valid = 0;
	}
}

int video_dovi_reshape_update(VIDEO_DOVI_RESHAPE_STATE *state, const unsigned char *nal, int size)
{
	if (!state || !nal || size <= 2)
		return -1;
	DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu(nal, (size_t)size);
	if (!rpu)
		return -1;
	const DoviRpuDataHeader *header = dovi_rpu_get_header(rpu);
	const DoviRpuDataMapping *mapping = NULL;
	const DoviVdrDmData *dm = NULL;
	int ret = -1;
	/* Float coefficients and the 64-bit shift guard are not supported. */
	if (!header || dovi_rpu_get_error(rpu) || header->coefficient_data_type != 0 ||
	    header->coefficient_log2_denom > 32 || header->bl_bit_depth_minus8 > 8)
		goto done;

	/* Curves: new ones are stored under their id; a reused id that was never
	 * received falls back to id 0, as FFmpeg does. */
	int id;
	if (header->use_prev_vdr_rpu_flag) {
		id = header->prev_vdr_rpu_id < VIDEO_DOVI_RESHAPE_IDS ? (int)header->prev_vdr_rpu_id : 0;
		if (!(state->mapping_valid & (1u << id)))
			id = 0;
		if (!(state->mapping_valid & 1u))
			goto done;
	} else {
		mapping = dovi_rpu_get_data_mapping(rpu);
		if (!mapping || mapping->vdr_rpu_id >= VIDEO_DOVI_RESHAPE_IDS)
			goto done;
		id = (int)mapping->vdr_rpu_id;
		VIDEO_DOVI_RESHAPE *m = &state->mappings[id];
		memset(m, 0, sizeof(*m)); /* unused pieces as in a one-shot parse */
		state->mapping_valid &= ~(1u << id);
		for (int c = 0; c < 3; c++)
			if (map_component(m, c, &mapping->curves[c], header))
				goto done;
		state->mapping_valid |= 1u << id;
	}

	/* Display management data: kept until the next RPU carrying it. */
	if (header->vdr_dm_metadata_present_flag) {
		dm = dovi_rpu_get_vdr_dm_data(rpu);
		if (!dm)
			goto done;
		fill_dm(&state->params, dm);
		state->dm_valid = 1;
	}
	if (!state->dm_valid)
		goto done;
	copy_mapping(&state->params, &state->mappings[id]);
	state->params.bl_bit_depth = (int)header->bl_bit_depth_minus8 + 8;
	ret = 0;
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

void video_dovi_reshape_reset(VIDEO_DOVI_RESHAPE_STATE *state)
{
	(void)state;
}

int video_dovi_reshape_update(VIDEO_DOVI_RESHAPE_STATE *state, const unsigned char *nal, int size)
{
	(void)state;
	(void)nal;
	(void)size;
	return -1;
}

#endif

int video_dovi_reshape_params(const unsigned char *nal, int size, VIDEO_DOVI_RESHAPE *out)
{
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));
	VIDEO_DOVI_RESHAPE_STATE *state = calloc(1, sizeof(*state));
	if (!state)
		return -1;
	int ret = video_dovi_reshape_update(state, nal, size);
	if (ret == 0)
		*out = state->params;
	free(state);
	return ret;
}

/* Same math as VIDEO_DOVI_RESHAPE_FS_BODY with SDR output. */
static float reshape_component(const VIDEO_DOVI_RESHAPE *p, int c, const float sig[3])
{
	const float s = sig[c];
	int piece = 0;
	for (int i = 0; i < VIDEO_DOVI_RESHAPE_PIECES - 1; i++)
		if (s >= p->pivots[c][i])
			piece = i + 1;
	const float *k = p->coeffs[c][piece];
	if (k[3] == 0.0f)
		return (k[2] * s + k[1]) * s + k[0];
	const float (*w)[4] = &p->mmr[c][(int)k[1]];
	float x[4] = { sig[0] * sig[1], sig[0] * sig[2], sig[1] * sig[2], 0.0f };
	x[3] = x[0] * sig[2];
	float r = k[0] + w[0][0] * sig[0] + w[0][1] * sig[1] + w[0][2] * sig[2] +
		w[1][0] * x[0] + w[1][1] * x[1] + w[1][2] * x[2] + w[1][3] * x[3];
	float s1[3] = { sig[0], sig[1], sig[2] }, x1[4] = { x[0], x[1], x[2], x[3] };
	for (int order = 2; order <= (int)k[3] && order <= 3; order++) {
		for (int i = 0; i < 3; i++)
			s1[i] *= sig[i];
		for (int i = 0; i < 4; i++)
			x1[i] *= x[i];
		const float *a = w[(order - 1) * 2], *b = w[(order - 1) * 2 + 1];
		r += a[0] * s1[0] + a[1] * s1[1] + a[2] * s1[2] +
			b[0] * x1[0] + b[1] * x1[1] + b[2] * x1[2] + b[3] * x1[3];
	}
	return r;
}

#define DOVI_LUT_SIZE 4096

static float pq_eotf(float e)
{
	e = powf(e, 1.0f / 78.84375f);
	return powf(fmaxf(e - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * e), 1.0f / 0.1593017578125f);
}

static float lut_lookup(const float *lut, float v)
{
	v = v <= 0.0f ? 0.0f : v >= 1.0f ? 1.0f : v;
	float f = v * (DOVI_LUT_SIZE - 1);
	int i = (int)f;
	if (i >= DOVI_LUT_SIZE - 1)
		return lut[DOVI_LUT_SIZE - 1];
	return lut[i] + (lut[i + 1] - lut[i]) * (f - (float)i);
}

void video_dovi_reshape_bgra(const VIDEO_DOVI_RESHAPE *p, const uint16_t *const planes[3],
	const int strides[3], int width, int height, uint8_t *dst, int dst_stride)
{
	/* 203-nit reference white, tone mapped up to the source peak, BT.709,
	 * gamma 2.2; both power curves go through tables. */
	static const float BT2020_TO_BT709[9] = {
		1.6605f, -0.5876f, -0.0728f,
		-0.1246f, 1.1329f, -0.0083f,
		-0.0182f, -0.1006f, 1.1187f,
	};
	float eotf[DOVI_LUT_SIZE], gamma[DOVI_LUT_SIZE];
	for (int i = 0; i < DOVI_LUT_SIZE; i++) {
		float v = (float)i / (DOVI_LUT_SIZE - 1);
		eotf[i] = pq_eotf(v);
		gamma[i] = powf(v, 1.0f / 2.2f);
	}
	const float scale = 10000.0f / 203.0f;
	const float peak_nits = 10000.0f * pq_eotf(p->source_max_pq > 0.0f ? p->source_max_pq : 0.7518f);
	const float peak = fmaxf(peak_nits / 203.0f, 1.0f);
	const float code_scale = 1.0f / (float)(1 << p->bl_bit_depth);
	for (int y = 0; y < height; y++) {
		const uint16_t *py = planes[0] + (size_t)y * strides[0];
		const uint16_t *pu = planes[1] + (size_t)(y / 2) * strides[1];
		const uint16_t *pv = planes[2] + (size_t)(y / 2) * strides[2];
		uint8_t *out = dst + (size_t)y * dst_stride;
		for (int x = 0; x < width; x++) {
			float sig[3] = { py[x] * code_scale, pu[x / 2] * code_scale, pv[x / 2] * code_scale };
			for (int c = 0; c < 3; c++)
				sig[c] = fminf(fmaxf(sig[c], p->pivot_lo[c]), p->pivot_hi[c]);
			float r[3];
			for (int c = 0; c < 3; c++)
				r[c] = fminf(fmaxf(reshape_component(p, c, sig), 0.0f), 1.0f) - p->ycc_offset[c];
			float lms[3], rgb[3];
			for (int i = 0; i < 3; i++)
				lms[i] = lut_lookup(eotf, p->ycc_to_rgb[i * 3] * r[0] + p->ycc_to_rgb[i * 3 + 1] * r[1] +
					p->ycc_to_rgb[i * 3 + 2] * r[2]);
			for (int i = 0; i < 3; i++)
				rgb[i] = fmaxf(p->lms_to_rgb[i * 3] * lms[0] + p->lms_to_rgb[i * 3 + 1] * lms[1] +
					p->lms_to_rgb[i * 3 + 2] * lms[2], 0.0f) * scale;
			float l = 0.2627f * rgb[0] + 0.6780f * rgb[1] + 0.0593f * rgb[2];
			float m = l > 0.0f ? (1.0f + l / (peak * peak)) / (1.0f + l) : 0.0f;
			uint8_t bgr[3];
			for (int i = 0; i < 3; i++) {
				float v = (BT2020_TO_BT709[i * 3] * rgb[0] + BT2020_TO_BT709[i * 3 + 1] * rgb[1] +
					BT2020_TO_BT709[i * 3 + 2] * rgb[2]) * m;
				bgr[2 - i] = (uint8_t)(lut_lookup(gamma, v) * 255.0f + 0.5f);
			}
			out[x * 4] = bgr[0];
			out[x * 4 + 1] = bgr[1];
			out[x * 4 + 2] = bgr[2];
			out[x * 4 + 3] = 255;
		}
	}
}
