/*
 * Copyright (C) 2026 The Nova Video Player Project
 *
 * Parse a Dolby Vision RPU into FFmpeg's AVDOVIMetadata via the libdovi C
 * parser. See dovi_rpu_meta.h; coefficient semantics replicate FFmpeg's
 * libavcodec/dovi_rpudec.c.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "global.h"
#include "debug.h"

#include <string.h>
#include <limits.h>

#include <libavutil/mem.h>
#include <libavutil/rational.h>

#include <libdovi/rpu_parser.h>

#include "dovi_rpu_meta.h"

#define TAG "DOVI_RPU"

static void fill_header(AVDOVIRpuDataHeader *hdr, const DoviRpuDataHeader *in)
{
	hdr->rpu_type            = in->rpu_type;
	hdr->rpu_format          = in->rpu_format;
	hdr->vdr_rpu_profile     = in->vdr_rpu_profile;
	hdr->vdr_rpu_level       = in->vdr_rpu_level;
	hdr->chroma_resampling_explicit_filter_flag = in->chroma_resampling_explicit_filter_flag;
	hdr->coef_data_type      = in->coefficient_data_type;
	hdr->coef_log2_denom     = (uint8_t) in->coefficient_log2_denom;
	hdr->vdr_rpu_normalized_idc = in->vdr_rpu_normalized_idc;
	hdr->bl_video_full_range_flag = in->bl_video_full_range_flag;
	hdr->bl_bit_depth        = (uint8_t)(in->bl_bit_depth_minus8 + 8);
	hdr->el_bit_depth        = (uint8_t)(in->el_bit_depth_minus8 + 8);
	hdr->vdr_bit_depth       = (uint8_t)(in->vdr_bit_depth_minus8 + 8);
	hdr->spatial_resampling_filter_flag = in->spatial_resampling_filter_flag;
	hdr->el_spatial_resampling_filter_flag = in->el_spatial_resampling_filter_flag;
	hdr->disable_residual_flag = in->disable_residual_flag;
	/* ext_mapping_idc_* are not exposed by the libdovi C API; they only
	 * matter for profile 8.4 extended BL inverse mapping. */
	hdr->ext_mapping_idc_0_4 = 0;
	hdr->ext_mapping_idc_5_7 = 0;
}

static void fill_curve(AVDOVIReshapingCurve *curve, const DoviReshapingCurve *in,
                       int coef_log2_denom)
{
	int num_pivots = (int)(in->num_pivots_minus2 + 2);
	int pieces = num_pivots - 1;
	int i, j, k;

	if (num_pivots < 2 || num_pivots > AV_DOVI_MAX_PIECES + 1)
		return;

	curve->num_pivots = (uint8_t) num_pivots;
	/* The RPU bitstream encodes pivots as *deltas* (cumulative sums):
	 * FFmpeg's decoder reads `pivot += get_bits(bl_bit_depth)` and its
	 * encoder writes `pivots[i] - prev`, and its encoder rejects
	 * non-monotonic results. libdovi (2.3.x) exposes the raw per-field
	 * values without accumulating, so a DEE-authored 8-piece curve
	 * whose bit fields are [0,128,128,128,128,128,128,128,127] comes
	 * out of libdovi as the degenerate absolute sequence
	 * [0,128,128,...,127] (six zero-width pieces + a backward step):
	 * every luma sample above pivot 128 then clamps out and the whole
	 * picture renders crushed to ~12% brightness. Accumulate here so
	 * the curve spans [0..1023] like FFmpeg/libplacebo/mpv see it.
	 * (2-pivot identity curves [0,1023] are unaffected: the first
	 * field is 0 and the single delta is the full range.) */
	{
		int pivot = 0;
		for (i = 0; i < num_pivots && i < (int) in->pivots.len; i++) {
			pivot += in->pivots.data[i];
			if (pivot > 0xFFFF)
				pivot = 0xFFFF;
			curve->pivots[i] = (uint16_t) pivot;
		}
	}

	/* libdovi exposes one mapping method per component; the DV spec allows
	 * per-piece methods, FFmpeg stores them per piece. Mirror the single
	 * method across all pieces (matches real-world RPUs). */
	for (i = 0; i < pieces; i++)
		curve->mapping_idc[i] = in->mapping_idc;

	if (in->mapping_idc == AV_DOVI_MAPPING_POLYNOMIAL && in->polynomial) {
		const DoviPolynomialCurve *poly = in->polynomial;
		for (i = 0; i < pieces; i++) {
			if (i < (int) poly->poly_order_minus1.len)
				curve->poly_order[i] = (uint8_t)(poly->poly_order_minus1.data[i] + 1);
			/* FFmpeg's get_se_coef (RPU_COEFF_FIXED) combines the two
			 * bit ranges libdovi exposes separately:
			 *   combined = ipart * (1 << coef_log2_denom) | fpart
			 * where poly_coef_int = ipart (signed golomb, type-0 only)
			 * and poly_coef = fpart (var-length fractional bits).
			 * Using either field alone breaks the scaling (identity
			 * curve becomes 1 instead of 1<<denom) and the reshape
			 * collapses to a flat green. */
			if (i < (int) poly->poly_coef_int.len && poly->poly_coef_int.list[i] &&
			    i < (int) poly->poly_coef.len && poly->poly_coef.list[i]) {
				const DoviI64Data *ipart = poly->poly_coef_int.list[i];
				const DoviU64Data *fpart = poly->poly_coef.list[i];
				for (k = 0; k < 3 && k < (int) ipart->len && k < (int) fpart->len; k++)
					curve->poly_coef[i][k] =
					(int64_t)ipart->data[k] * (1LL << coef_log2_denom) |
					(int64_t)fpart->data[k];
			}
		}
	} else if (in->mapping_idc == AV_DOVI_MAPPING_MMR && in->mmr) {
		const DoviMMRCurve *mmr = in->mmr;
		for (i = 0; i < pieces; i++) {
			int order = 1;
			if (i < (int) mmr->mmr_order_minus1.len)
				order = (int)(mmr->mmr_order_minus1.data[i] + 1);
			curve->mmr_order[i] = (uint8_t) order;
			/* FFmpeg's get_se_coef (RPU_COEFF_FIXED) combines the two bit
			 * ranges libdovi exposes separately:
			 *   combined = ipart * (1 << coef_log2_denom) | fpart
			 * with the sign taken from ipart (se-golomb). Measured on
			 * DEE-authored P7 CMv40 RPUs (the first real MMR reshape
			 * sample): mmr_constant_int = -1, frac = 7787113 -> the true
			 * constant is -0.0717, while reading the frac field alone
			 * yields +0.9283 (sign flipped, magnitude corrupted) - the
			 * chroma MMR transform then renders a purple/magenta cast.
			 * Same class of bug as the polynomial and NLQ fixes below. */
			if (i < (int) mmr->mmr_constant_int.len) {
				int64_t ipart = (int64_t) mmr->mmr_constant_int.data[i];
				uint64_t frac = i < (int) mmr->mmr_constant.len ?
					mmr->mmr_constant.data[i] : 0;
				/* exactly FFmpeg's get_se_coef: ipart * (1<<denom) | frac.
				 * Multiplication (not shift): for negative ipart the low
				 * denom bits of the product are zero, so the OR folds the
				 * fraction in with the correct sign. */
				curve->mmr_constant[i] =
					ipart * (1LL << coef_log2_denom) | (int64_t) frac;
			}
			if (i < (int) mmr->mmr_coef.len && mmr->mmr_coef.list[i]) {
				const DoviU64Data2D *per_piece = mmr->mmr_coef.list[i];
				for (j = 0; j < order && j < 3 && j < (int) per_piece->len; j++) {
					const DoviU64Data *row = per_piece->list[j];
					if (!row)
						continue;
					for (k = 0; k < 7 && k < (int) row->len; k++) {
						int64_t ipart = 0;
						uint64_t frac = row->data[k];
						if (i < (int) mmr->mmr_coef_int.len &&
						    j < (int) mmr->mmr_coef_int.list[i]->len)
							ipart = mmr->mmr_coef_int.list[i]->list[j]->data[k];
						curve->mmr_coef[i][j][k] =
							ipart * (1LL << coef_log2_denom) | (int64_t) frac;
					}
				}
			}
		}
	}
}

static void fill_mapping(AVDOVIDataMapping *map, const DoviRpuDataMapping *in,
                         int coef_log2_denom)
{
	int c;

	map->vdr_rpu_id = (uint8_t) in->vdr_rpu_id;
	map->mapping_color_space = (uint8_t) in->mapping_color_space;
	map->mapping_chroma_format_idc = (uint8_t) in->mapping_chroma_format_idc;
	map->num_x_partitions = (uint32_t)(in->num_x_partitions_minus1 + 1);
	map->num_y_partitions = (uint32_t)(in->num_y_partitions_minus1 + 1);

	for (c = 0; c < 3; c++)
		fill_curve(&map->curves[c], &in->curves[c], coef_log2_denom);

	map->nlq_method_idc = (enum AVDOVINLQMethod) in->nlq_method_idc;
	if (in->nlq) {
		for (c = 0; c < 3; c++) {
			/* FFmpeg's get_ue_coef (RPU_COEFF_FIXED) combines the two bit
		 * ranges libdovi exposes separately:
		 *   combined = ipart * (1 << coef_log2_denom) | fpart
		 * The NLQ fields are ue-coefficients like the polynomial coefs:
		 * *_int holds the ue-golomb integer part, the var-length field
		 * holds the fractional bits (measured on got-kf: slope_int=0,
		 * slope_frac=2048 -> the true slope is 2048/2^23). Reading
		 * only _int left slope/threshold/vdr_in_max at 0, which made
		 * sh_dovi_compose_nlq a no-op - FEL silently degraded to MEL. */
			uint64_t vim = in->nlq->vdr_in_max_int[c] << coef_log2_denom |
			               in->nlq->vdr_in_max[c];
			uint64_t slp = in->nlq->linear_deadzone_slope_int[c] << coef_log2_denom |
			               in->nlq->linear_deadzone_slope[c];
			uint64_t thr = in->nlq->linear_deadzone_threshold_int[c] << coef_log2_denom |
			               in->nlq->linear_deadzone_threshold[c];
			map->nlq[c].nlq_offset = in->nlq->nlq_offset[c];
			map->nlq[c].vdr_in_max = vim;
			map->nlq[c].linear_deadzone_slope = slp;
			map->nlq[c].linear_deadzone_threshold = thr;
		}
	}
	if (in->nlq_pred_pivot_value.len >= 2) {
		/* same delta-coded-pivots rule as the reshape curves above:
		 * accumulate the two NLQ prediction pivot fields */
		map->nlq_pivots[0] = in->nlq_pred_pivot_value.data[0];
		map->nlq_pivots[1] = (uint16_t)(map->nlq_pivots[0] +
			                         in->nlq_pred_pivot_value.data[1]);
	}
}

static void fill_color(AVDOVIColorMetadata *color, const DoviVdrDmData *in,
                       uint8_t profile)
{
	int i;
	int denom = (profile == 4) ? (1 << 30) : (1 << 28);
	const int16_t ycc[9] = {
		in->ycc_to_rgb_coef0, in->ycc_to_rgb_coef1, in->ycc_to_rgb_coef2,
		in->ycc_to_rgb_coef3, in->ycc_to_rgb_coef4, in->ycc_to_rgb_coef5,
		in->ycc_to_rgb_coef6, in->ycc_to_rgb_coef7, in->ycc_to_rgb_coef8,
	};
	const uint32_t off[3] = {
		in->ycc_to_rgb_offset0, in->ycc_to_rgb_offset1, in->ycc_to_rgb_offset2,
	};
	const int16_t lms[9] = {
		in->rgb_to_lms_coef0, in->rgb_to_lms_coef1, in->rgb_to_lms_coef2,
		in->rgb_to_lms_coef3, in->rgb_to_lms_coef4, in->rgb_to_lms_coef5,
		in->rgb_to_lms_coef6, in->rgb_to_lms_coef7, in->rgb_to_lms_coef8,
	};

	color->dm_metadata_id = (uint8_t) in->affected_dm_metadata_id;
	color->scene_refresh_flag = (uint8_t) in->scene_refresh_flag;

	for (i = 0; i < 9; i++)
		color->ycc_to_rgb_matrix[i] = av_make_q(ycc[i], 1 << 13);
	for (i = 0; i < 3; i++) {
		unsigned offset = off[i];
		int d = denom;
		if (offset > INT_MAX) {
			/* keep the value representable inside AVRational (FFmpeg does
			 * the same halving) */
			offset >>= 1;
			d >>= 1;
		}
		color->ycc_to_rgb_offset[i] = av_make_q((int) offset, d);
	}
	for (i = 0; i < 9; i++)
		color->rgb_to_lms_matrix[i] = av_make_q(lms[i], 1 << 14);

	color->signal_eotf        = in->signal_eotf;
	color->signal_eotf_param0 = in->signal_eotf_param0;
	color->signal_eotf_param1 = in->signal_eotf_param1;
	color->signal_eotf_param2 = in->signal_eotf_param2;
	color->signal_bit_depth   = in->signal_bit_depth;
	color->signal_color_space = in->signal_color_space;
	color->signal_chroma_format = in->signal_chroma_format;
	color->signal_full_range_flag = in->signal_full_range_flag;
	color->source_min_pq      = in->source_min_pq;
	color->source_max_pq      = in->source_max_pq;
	color->source_diagonal    = in->source_diagonal;
}

static void fill_primaries(AVColorPrimariesDesc *out,
                           uint16_t rx, uint16_t ry, uint16_t gx, uint16_t gy,
                           uint16_t bx, uint16_t by, uint16_t wx, uint16_t wy)
{
	const int denom = 32767;	/* FFmpeg get_cie_xy */
	out->prim.r.x = av_make_q((int)(int16_t) rx, denom);
	out->prim.r.y = av_make_q((int)(int16_t) ry, denom);
	out->prim.g.x = av_make_q((int)(int16_t) gx, denom);
	out->prim.g.y = av_make_q((int)(int16_t) gy, denom);
	out->prim.b.x = av_make_q((int)(int16_t) bx, denom);
	out->prim.b.y = av_make_q((int)(int16_t) by, denom);
	out->wp.x     = av_make_q((int)(int16_t) wx, denom);
	out->wp.y     = av_make_q((int)(int16_t) wy, denom);
}

/* append DM extension blocks; returns number written */
static int fill_ext_blocks(AVDOVIMetadata *meta, const DoviDmData *dm)
{
	int n = 0, i;
	AVDOVIDmData *ext;

#define NEXT() \
	do { \
		if (n >= AV_DOVI_MAX_EXT_BLOCKS) \
			return n; \
		ext = av_dovi_get_ext(meta, n++); \
		memset(ext, 0, sizeof(*ext)); \
	} while (0)

	if (dm->level1) {
		NEXT();
		ext->level = 1;
		ext->l1.min_pq = dm->level1->min_pq;
		ext->l1.max_pq = dm->level1->max_pq;
		ext->l1.avg_pq = dm->level1->avg_pq;
	}
	for (i = 0; i < (int) dm->level2.len; i++) {
		const DoviExtMetadataBlockLevel2 *b = dm->level2.list[i];
		if (!b)
			continue;
		NEXT();
		ext->level = 2;
		ext->l2.target_max_pq         = b->target_max_pq;
		ext->l2.trim_slope            = b->trim_slope;
		ext->l2.trim_offset           = b->trim_offset;
		ext->l2.trim_power            = b->trim_power;
		ext->l2.trim_chroma_weight    = b->trim_chroma_weight;
		ext->l2.trim_saturation_gain  = b->trim_saturation_gain;
		ext->l2.ms_weight             = b->ms_weight;
	}
	if (dm->level3) {
		NEXT();
		ext->level = 3;
		ext->l3.min_pq_offset = dm->level3->min_pq_offset;
		ext->l3.max_pq_offset = dm->level3->max_pq_offset;
		ext->l3.avg_pq_offset = dm->level3->avg_pq_offset;
	}
	if (dm->level4) {
		NEXT();
		ext->level = 4;
		ext->l4.anchor_pq   = dm->level4->anchor_pq;
		ext->l4.anchor_power = dm->level4->anchor_power;
	}
	if (dm->level5) {
		NEXT();
		ext->level = 5;
		ext->l5.left_offset   = dm->level5->active_area_left_offset;
		ext->l5.right_offset  = dm->level5->active_area_right_offset;
		ext->l5.top_offset    = dm->level5->active_area_top_offset;
		ext->l5.bottom_offset = dm->level5->active_area_bottom_offset;
	}
	if (dm->level6) {
		NEXT();
		ext->level = 6;
		ext->l6.max_luminance  = dm->level6->max_display_mastering_luminance;
		ext->l6.min_luminance  = dm->level6->min_display_mastering_luminance;
		ext->l6.max_cll        = dm->level6->max_content_light_level;
		ext->l6.max_fall       = dm->level6->max_frame_average_light_level;
	}
	for (i = 0; i < (int) dm->level8.len; i++) {
		const DoviExtMetadataBlockLevel8 *b = dm->level8.list[i];
		if (!b)
			continue;
		NEXT();
		ext->level = 8;
		ext->l8.target_display_index  = b->target_display_index;
		ext->l8.trim_slope            = b->trim_slope;
		ext->l8.trim_offset           = b->trim_offset;
		ext->l8.trim_power            = b->trim_power;
		ext->l8.trim_chroma_weight    = b->trim_chroma_weight;
		ext->l8.trim_saturation_gain  = b->trim_saturation_gain;
		ext->l8.ms_weight             = b->ms_weight;
		ext->l8.target_mid_contrast   = b->target_mid_contrast;
		ext->l8.clip_trim             = b->clip_trim;
		ext->l8.saturation_vector_field[0] = b->saturation_vector_field0;
		ext->l8.saturation_vector_field[1] = b->saturation_vector_field1;
		ext->l8.saturation_vector_field[2] = b->saturation_vector_field2;
		ext->l8.saturation_vector_field[3] = b->saturation_vector_field3;
		ext->l8.saturation_vector_field[4] = b->saturation_vector_field4;
		ext->l8.saturation_vector_field[5] = b->saturation_vector_field5;
		ext->l8.hue_vector_field[0] = b->hue_vector_field0;
		ext->l8.hue_vector_field[1] = b->hue_vector_field1;
		ext->l8.hue_vector_field[2] = b->hue_vector_field2;
		ext->l8.hue_vector_field[3] = b->hue_vector_field3;
		ext->l8.hue_vector_field[4] = b->hue_vector_field4;
		ext->l8.hue_vector_field[5] = b->hue_vector_field5;
	}
	if (dm->level9) {
		NEXT();
		ext->level = 9;
		ext->l9.source_primary_index = dm->level9->source_primary_index;
		fill_primaries(&ext->l9.source_display_primaries,
		               dm->level9->source_primary_red_x, dm->level9->source_primary_red_y,
		               dm->level9->source_primary_green_x, dm->level9->source_primary_green_y,
		               dm->level9->source_primary_blue_x, dm->level9->source_primary_blue_y,
		               dm->level9->source_primary_white_x, dm->level9->source_primary_white_y);
	}
	for (i = 0; i < (int) dm->level10.len; i++) {
		const DoviExtMetadataBlockLevel10 *b = dm->level10.list[i];
		if (!b)
			continue;
		NEXT();
		ext->level = 10;
		ext->l10.target_display_index = b->target_display_index;
		ext->l10.target_max_pq        = b->target_max_pq;
		ext->l10.target_min_pq        = b->target_min_pq;
		ext->l10.target_primary_index = b->target_primary_index;
		fill_primaries(&ext->l10.target_display_primaries,
		               b->target_primary_red_x, b->target_primary_red_y,
		               b->target_primary_green_x, b->target_primary_green_y,
		               b->target_primary_blue_x, b->target_primary_blue_y,
		               b->target_primary_white_x, b->target_primary_white_y);
	}
	if (dm->level11) {
		NEXT();
		ext->level = 11;
		ext->l11.content_type       = dm->level11->content_type;
		ext->l11.whitepoint         = dm->level11->whitepoint;
		ext->l11.reference_mode_flag = dm->level11->reference_mode_flag;
		/* remaining l11 fields are not exposed by libdovi; left zero */
	}
	if (dm->level254) {
		NEXT();
		ext->level = 254;
		ext->l254.dm_mode          = dm->level254->dm_mode;
		ext->l254.dm_version_index = dm->level254->dm_version_index;
	}
	if (dm->level255) {
		NEXT();
		ext->level = 255;
		ext->l255.dm_run_mode    = dm->level255->dm_run_mode;
		ext->l255.dm_run_version = dm->level255->dm_run_version;
		ext->l255.dm_debug[0]    = dm->level255->dm_debug0;
		ext->l255.dm_debug[1]    = dm->level255->dm_debug1;
		ext->l255.dm_debug[2]    = dm->level255->dm_debug2;
		ext->l255.dm_debug[3]    = dm->level255->dm_debug3;
	}
#undef NEXT
	return n;
}

AVDOVIMetadata *dovi_rpu_parse_to_avmetadata(const uint8_t *rpu_nal, size_t size)
{
	DoviRpuOpaque *rpu = NULL;
	const DoviRpuDataHeader *hdr = NULL;
	const DoviRpuDataMapping *mapping = NULL;
	const DoviVdrDmData *dm = NULL;
	AVDOVIMetadata *meta = NULL;
	const char *err;

	if (!rpu_nal || size < 3)
		return NULL;

	rpu = dovi_parse_unspec62_nalu(rpu_nal, size);
	if (!rpu)
		return NULL;
	err = dovi_rpu_get_error(rpu);
	if (err) {
		serprintf("%s: parse error: %s\n", TAG, err);
		dovi_rpu_free(rpu);
		return NULL;
	}

	hdr = dovi_rpu_get_header(rpu);
	mapping = dovi_rpu_get_data_mapping(rpu);
	dm = dovi_rpu_get_vdr_dm_data(rpu);
	if (!hdr || !mapping || !dm) {
		serprintf("%s: incomplete RPU (hdr %p mapping %p dm %p)\n",
		          TAG, (const void *) hdr, (const void *) mapping, (const void *) dm);
		goto fail;
	}

	meta = av_dovi_metadata_alloc(NULL);
	if (!meta)
		goto fail;

	fill_header(av_dovi_get_header(meta), hdr);
	fill_mapping(av_dovi_get_mapping(meta), mapping, hdr->coefficient_log2_denom);
	fill_color(av_dovi_get_color(meta), dm, hdr->vdr_rpu_profile);
	meta->num_ext_blocks = fill_ext_blocks(meta, &dm->dm_data);


	dovi_rpu_free_header(hdr);
	dovi_rpu_free_data_mapping(mapping);
	dovi_rpu_free_vdr_dm_data(dm);
	dovi_rpu_free(rpu);
	return meta;

fail:
	if (hdr)
		dovi_rpu_free_header(hdr);
	if (mapping)
		dovi_rpu_free_data_mapping(mapping);
	if (dm)
		dovi_rpu_free_vdr_dm_data(dm);
	if (rpu)
		dovi_rpu_free(rpu);
	if (meta)
		av_free(meta);
	return NULL;
}
