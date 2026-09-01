/*
 * Copyright (C) 2026 The Nova Video Player Project
 *
 * Parse a Dolby Vision RPU (NAL type 62) into FFmpeg's AVDOVIMetadata using
 * the libdovi C parser, so libplacebo's full DV reshaping
 * (pl_map_avdovi_metadata / pl_map_dovi_metadata) can be driven from frames
 * that never passed through FFmpeg's decoder (e.g. MediaCodec hardware
 * decode). FFmpeg's own RPU parser is private to libavcodec, so this
 * converter replicates the coefficient semantics of libavcodec/dovi_rpudec.c:
 *   - ycc_to_rgb_matrix:  q(raw s16, 2^13)
 *   - ycc_to_rgb_offset:  q(raw u32, 2^30 for profile 4 else 2^28), halved
 *                         when the raw value exceeds INT_MAX
 *   - rgb_to_lms_matrix:  q(raw s16, 2^14)
 *   - reshaping/NLQ coefficients: integer form scaled by 2^coef_log2_denom
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef DOVI_RPU_META_H
#define DOVI_RPU_META_H

#include <stddef.h>
#include <stdint.h>

#include <libavutil/dovi_meta.h>

/*
 * Parse one Dolby Vision RPU NAL unit (2-byte HEVC NAL header included,
 * i.e. the raw payload of a NAL type 62) into a freshly allocated
 * AVDOVIMetadata (free with av_free). Returns NULL on parse failure.
 */
AVDOVIMetadata *dovi_rpu_parse_to_avmetadata(const uint8_t *rpu_nal, size_t size);

#endif /* DOVI_RPU_META_H */
