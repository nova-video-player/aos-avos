/*
 * Copyright (C) 2026 The Nova Video Player Project
 *
 * Shared HEVC NAL walking helpers for Dolby Vision processing.
 * See dovi_nal.h for the interface; NAL classification mirrors FFmpeg's
 * libavcodec/bsf/dovi_split.c.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include <string.h>

#include <libavutil/mem.h>
#include <libavcodec/defs.h>	/* AV_INPUT_BUFFER_PADDING_SIZE (FFmpeg >= 9) */
#include <libavcodec/packet.h>

#include "dovi_nal.h"

int dovi_hvcc_nal_length_size(const uint8_t *data, int size)
{
	if (size >= 23 && data[0] == 1 &&
	    !(data[1] == 0 && data[2] == 0 && data[3] == 1) &&
	    !(data[1] == 0 && data[2] == 0 && data[3] == 0 && size >= 5 && data[4] == 1))
		return (data[21] & 3) + 1;
	return 0;
}

const uint8_t *dovi_next_nal(const uint8_t *data, int size, int lsize,
                             int *pos, int *nal_size)
{
	if (lsize > 0) {
		int len = 0, i;
		if (*pos + lsize > size)
			return NULL;
		for (i = 0; i < lsize; i++)
			len = (len << 8) | data[*pos + i];
		*pos += lsize;
		if (len <= 0 || *pos + len > size)
			return NULL;		/* malformed, stop */
		*nal_size = len;
		data += *pos;
		*pos += len;
		return data;
	}
	/* Annex-B: find next start code */
	{
		int i = *pos, start = -1, sc_len = 0;
		for (; i + 2 < size; i++) {
			if (!data[i] && !data[i + 1] && data[i + 2] == 1) {
				if (i > 0 && !data[i - 1]) { start = i - 1; sc_len = 4; }
				else                       { start = i;     sc_len = 3; }
				break;
			}
		}
		if (start < 0)
			return NULL;
		/* NAL payload runs until the next start code (or end) */
		{
			int end = size, j;
			for (j = start + sc_len; j + 2 < size; j++) {
				if (!data[j] && !data[j + 1] && data[j + 2] == 1) {
					end = (j > 0 && !data[j - 1]) ? j - 1 : j;
					break;
				}
			}
			*nal_size = end - (start + sc_len);
			*pos = end;
			if (*nal_size <= 0)
				return dovi_next_nal(data, size, lsize, pos, nal_size);
			return data + start + sc_len;
		}
	}
}

int dovi_extract_rpu(const uint8_t *data, int size, int lsize,
                     uint8_t **rpu_out, int *rpu_size_out)
{
	int pos = 0, nal_size;
	const uint8_t *nal;

	*rpu_out = NULL;
	*rpu_size_out = 0;

	while ((nal = dovi_next_nal(data, size, lsize, &pos, &nal_size)) != NULL) {
		int type = (nal[0] >> 1) & 0x3F;
		if (type == DOVI_NAL_TYPE_RPU) {
			uint8_t *copy = (uint8_t *) av_malloc(nal_size + AV_INPUT_BUFFER_PADDING_SIZE);
			if (!copy)
				return 1;
			memcpy(copy, nal, nal_size);
			memset(copy + nal_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
			*rpu_out = copy;
			*rpu_size_out = nal_size;
			return 0;
		}
	}
	return 1;	/* no RPU in this access unit */
}

int dovi_strip_dv_nals(const uint8_t *data, int size, int lsize,
                       uint8_t **out, int *out_size)
{
	int prefix = lsize ? lsize : 4;
	size_t total = 0;
	int kept = 0, pass, pos, nal_size, i;
	const uint8_t *nal;
	uint8_t *buf = NULL, *dst;

	*out = NULL;
	*out_size = 0;

	for (pass = 0; pass < 2; pass++) {
		pos = 0;
		dst = buf;
		while ((nal = dovi_next_nal(data, size, lsize, &pos, &nal_size)) != NULL) {
			int type = (nal[0] >> 1) & 0x3F;
			if (type == DOVI_NAL_TYPE_RPU || type == DOVI_NAL_TYPE_EL)
				continue;
			if (pass == 0) {
				total += prefix + nal_size;
				kept++;
			} else {
				if (lsize) {
					for (i = lsize - 1; i >= 0; i--)
						*dst++ = (nal_size >> (8 * i)) & 0xFF;
				} else {
					*dst++ = 0; *dst++ = 0; *dst++ = 0; *dst++ = 1;
				}
				memcpy(dst, nal, nal_size);
				dst += nal_size;
			}
		}
		if (pass == 0) {
			if (!kept)
				return 0;	/* nothing left (shouldn't happen for video AUs) */
			buf = (uint8_t *) av_malloc(total + AV_INPUT_BUFFER_PADDING_SIZE);
			if (!buf)
				return 1;
		}
	}
	memset(dst, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	*out = buf;
	*out_size = (int) total;
	return 0;
}
