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

#ifndef VIDEO_DOVI_RPU_H
#define VIDEO_DOVI_RPU_H

#include "av.h"

#define VIDEO_DOVI_NAL_RPU 62
#define VIDEO_DOVI_NAL_EL  63
#define VIDEO_DOVI_NAL_SEI_PREFIX 39

/* Packets inspected per stream before giving up on finding a parsable RPU. */
#define VIDEO_DOVI_RPU_PROBE_PACKETS 32

enum {
	VIDEO_DOVI_EL_UNKNOWN = 0,
	VIDEO_DOVI_EL_NONE,
	VIDEO_DOVI_EL_MEL,
	VIDEO_DOVI_EL_FEL,
};

/* VIDEO_PROPERTIES.dv_rewrite: rewrites selected for the DV decoder path. */
#define VIDEO_DOVI_REWRITE_P7_TO_P81      1 /* libdovi mode 2, drop NAL 63 */
#define VIDEO_DOVI_REWRITE_STRIP_HDR10PLUS 2 /* remove HDR10+ T.35 SEI messages */

/* Return the next HEVC NAL unit at *pos. nal_unit_size is the hvcC
 * length-prefix size (1..4), or 0 for Annex B. *nal points at the two-byte NAL
 * header; *nal_size excludes start codes and trailing zero bytes. Returns 1 for
 * a NAL, 0 at the end, -1 for a malformed length prefix. */
int video_dovi_next_hevc_nal(const unsigned char *data, int size, int nal_unit_size,
	int *pos, const unsigned char **nal, int *nal_size);

/* Find the first HEVC NAL unit of `type` in one access unit. Returns 1 when
 * found, 0 otherwise (including malformed packets). */
int video_dovi_find_hevc_nal(const unsigned char *data, int size, int nal_unit_size,
	int type, const unsigned char **nal, int *nal_size);

/* hvcC length-prefix size from codec extradata, or 0 for Annex B input. */
int video_dovi_hevc_nal_unit_size(const unsigned char *extradata, int extradata_size);

/* Rewrite options actually available in this build (P7 needs libdovi). */
int video_dovi_rewrite_supported(void);

/* Read-only diagnostics: parse the RPU of early HEVC Dolby Vision packets with
 * libdovi and record the guessed profile and enhancement-layer type. Never
 * modifies the packet. A no-op without CONFIG_LIBDOVI. */
void video_dovi_rpu_probe(VIDEO_PROPERTIES *video, const unsigned char *data, int size,
	int nal_unit_size);

/* Apply video->dv_rewrite to one access unit, keeping its framing. Returns a
 * malloc'd access unit and its size, or NULL when the packet is unchanged or
 * cannot be rewritten safely; the caller then keeps the original packet. */
unsigned char *video_dovi_rewrite_au(VIDEO_PROPERTIES *video, const unsigned char *data,
	int size, int nal_unit_size, int *out_size);

#endif
