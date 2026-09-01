/*
 * Copyright (C) 2026 The Nova Video Player Project
 *
 * Shared HEVC NAL walking helpers for Dolby Vision processing.
 *
 * The NAL classification mirrors FFmpeg's libavcodec/bsf/dovi_split.c:
 *   - NAL type 62 (HEVC_NAL_UNSPEC62): Dolby Vision RPU metadata
 *   - NAL type 63 (HEVC_NAL_UNSPEC63): Dolby Vision enhancement layer
 *   - everything else: base layer
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef DOVI_NAL_H
#define DOVI_NAL_H

#include <stdint.h>

#define DOVI_NAL_TYPE_RPU 62	/* HEVC_NAL_UNSPEC62 */
#define DOVI_NAL_TYPE_EL  63	/* HEVC_NAL_UNSPEC63 */

/*
 * hvcC extradata -> NAL length field size; 0 means Annex-B.
 * (FFmpeg hvcc_nal_length_size semantics)
 */
int dovi_hvcc_nal_length_size(const uint8_t *extradata, int size);

/*
 * Iterate the NALs of one access unit.
 *   data/size: the access unit
 *   lsize:     NAL length field size (0 = Annex-B start codes)
 *   pos:       iterator state, initialize to 0
 *   nal_size:  out: size of the returned NAL (header included)
 * Returns a pointer to the NAL payload start, or NULL at end/malformed.
 */
const uint8_t *dovi_next_nal(const uint8_t *data, int size, int lsize,
                             int *pos, int *nal_size);

/*
 * Extract the Dolby Vision RPU NAL (type 62) from an access unit.
 * On success returns 0 and *rpu_out points to a freshly av_malloc'd copy of
 * the RPU NAL (2-byte NAL header included), size in *rpu_size_out.
 * Returns 1 when no RPU is present or on allocation failure.
 */
int dovi_extract_rpu(const uint8_t *data, int size, int lsize,
                     uint8_t **rpu_out, int *rpu_size_out);

/*
 * Build a copy of the access unit without RPU (62) and EL (63) NALs, keeping
 * the same NAL framing (length-prefixed or Annex-B). Used to feed a plain
 * HEVC decoder that must not see the DV-specific NALs.
 * Returns 0 on success (*out and *out_size freshly av_malloc'd; *out may be
 * NULL when nothing remained), 1 on malformed input or allocation failure.
 */
int dovi_strip_dv_nals(const uint8_t *data, int size, int lsize,
                       uint8_t **out, int *out_size);

#endif /* DOVI_NAL_H */
