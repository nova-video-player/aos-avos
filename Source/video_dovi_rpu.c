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

#include "global.h"
#include "types.h"
#include "debug.h"
#include "util.h"
#include "video_dovi_rpu.h"

#include <stdlib.h>
#include <string.h>
#include <libavcodec/codec_id.h>

#ifdef CONFIG_LIBDOVI
#include <libdovi/rpu_parser.h>
#endif

int video_dovi_next_hevc_nal(const unsigned char *data, int size, int nal_unit_size,
	int *pos, const unsigned char **nal, int *nal_size)
{
	if (!data || size <= 0 || !pos || !nal || !nal_size || *pos < 0)
		return 0;
	if (nal_unit_size) {
		if (nal_unit_size < 1 || nal_unit_size > 4)
			return -1;
		while (size - *pos >= nal_unit_size) {
			unsigned int len = 0;
			for (int i = 0; i < nal_unit_size; i++)
				len = (len << 8) | data[*pos + i];
			*pos += nal_unit_size;
			if (len > (unsigned int)(size - *pos))
				return -1;
			const unsigned char *start = data + *pos;
			*pos += (int)len;
			if (len >= 2) {
				*nal = start;
				*nal_size = (int)len;
				return 1;
			}
		}
		return 0;
	}
	// Annex B: emulation prevention keeps 00 00 01 out of NAL payloads.
	while (*pos + 3 <= size) {
		int p = *pos;
		if (data[p] || data[p + 1] || data[p + 2] != 1) {
			(*pos)++;
			continue;
		}
		int start = p + 3;
		int end = start;
		while (end + 3 <= size && (data[end] || data[end + 1] || data[end + 2] != 1))
			end++;
		if (end + 3 > size)
			end = size;
		int last = end;
		while (last > start && !data[last - 1])
			last--;
		*pos = end;
		if (last - start >= 2) {
			*nal = data + start;
			*nal_size = last - start;
			return 1;
		}
	}
	return 0;
}

int video_dovi_find_hevc_nal(const unsigned char *data, int size, int nal_unit_size,
	int type, const unsigned char **nal, int *nal_size)
{
	int pos = 0;
	const unsigned char *n;
	int n_size;
	if (!nal || !nal_size)
		return 0;
	while (video_dovi_next_hevc_nal(data, size, nal_unit_size, &pos, &n, &n_size) == 1) {
		if (((n[0] >> 1) & 0x3f) == type) {
			*nal = n;
			*nal_size = n_size;
			return 1;
		}
	}
	return 0;
}

int video_dovi_hevc_nal_unit_size(const unsigned char *extradata, int extradata_size)
{
	// HEVCDecoderConfigurationRecord: version 1, lengthSizeMinusOne at byte 21.
	if (!extradata || extradata_size < 23 || extradata[0] != 1)
		return 0;
	return (extradata[21] & 3) + 1;
}

int video_dovi_rewrite_supported(void)
{
#ifdef CONFIG_LIBDOVI
	return VIDEO_DOVI_REWRITE_P7_TO_P81 | VIDEO_DOVI_REWRITE_STRIP_HDR10PLUS;
#else
	return VIDEO_DOVI_REWRITE_STRIP_HDR10PLUS;
#endif
}

#ifdef CONFIG_LIBDOVI
static int video_dovi_el_type(const char *el_type)
{
	if (!el_type)
		return VIDEO_DOVI_EL_NONE;
	if (!strcmp(el_type, "FEL"))
		return VIDEO_DOVI_EL_FEL;
	if (!strcmp(el_type, "MEL"))
		return VIDEO_DOVI_EL_MEL;
	return VIDEO_DOVI_EL_UNKNOWN;
}

static const char *video_dovi_el_name(int el_type)
{
	switch (el_type) {
	case VIDEO_DOVI_EL_NONE: return "none";
	case VIDEO_DOVI_EL_MEL: return "MEL";
	case VIDEO_DOVI_EL_FEL: return "FEL";
	default: return "unknown";
	}
}
#endif

void video_dovi_rpu_probe(VIDEO_PROPERTIES *video, const unsigned char *data, int size,
	int nal_unit_size)
{
#ifdef CONFIG_LIBDOVI
	if (!video || video->codec_id != AV_CODEC_ID_HEVC || !video->dv_bitstream_profile ||
	    video->dv_rpu_probe_count >= VIDEO_DOVI_RPU_PROBE_PACKETS)
		return;
	video->dv_rpu_probe_count++;
	const unsigned char *nal = NULL;
	int nal_size = 0;
	if (!video_dovi_find_hevc_nal(data, size, nal_unit_size, VIDEO_DOVI_NAL_RPU, &nal, &nal_size)) {
		if (video->dv_rpu_probe_count == VIDEO_DOVI_RPU_PROBE_PACKETS)
			serprintf("Dolby Vision RPU: none found in the first %d packets\r\n",
				VIDEO_DOVI_RPU_PROBE_PACKETS);
		return;
	}
	const unsigned char *el = NULL;
	int el_size = 0;
	int el_nal = video_dovi_find_hevc_nal(data, size, nal_unit_size, VIDEO_DOVI_NAL_EL, &el, &el_size);

	DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu(nal, (size_t)nal_size);
	if (!rpu)
		return;
	const DoviRpuDataHeader *header = dovi_rpu_get_header(rpu);
	if (header) {
		video->dv_rpu_profile = header->guessed_profile;
		video->dv_rpu_el_type = video_dovi_el_type(header->el_type);
		video->dv_rpu_probe_count = VIDEO_DOVI_RPU_PROBE_PACKETS;
		serprintf("Dolby Vision RPU: config_profile=%d guessed_profile=%d el=%s el_nal=%d\r\n",
			video->dv_bitstream_profile, video->dv_rpu_profile,
			video_dovi_el_name(video->dv_rpu_el_type), el_nal);
		dovi_rpu_free_header(header);
	} else {
		const char *error = dovi_rpu_get_error(rpu);
		serprintf("Dolby Vision RPU: parse failed: %s\r\n", error ? error : "unknown error");
	}
	dovi_rpu_free(rpu);
#else
	(void)video;
	(void)data;
	(void)size;
	(void)nal_unit_size;
#endif
}

typedef struct {
	unsigned char *data;
	int size;
	int cap;
} DOVI_AU;

static int au_reserve(DOVI_AU *au, int add)
{
	if (add < 0 || au->size > 0x7fffffff - add)
		return -1;
	if (au->size + add <= au->cap)
		return 0;
	int cap = au->cap ? au->cap : 256;
	while (cap < au->size + add) {
		if (cap > 0x3fffffff)
			return -1;
		cap *= 2;
	}
	unsigned char *data = realloc(au->data, (size_t)cap);
	if (!data)
		return -1;
	au->data = data;
	au->cap = cap;
	return 0;
}

// Append one NAL in the input framing: length prefix or 4-byte start code.
static int au_put_nal(DOVI_AU *au, const unsigned char *nal, int len, int nal_unit_size)
{
	int prefix = nal_unit_size ? nal_unit_size : 4;
	if (len <= 0 || (nal_unit_size && nal_unit_size < 4 &&
	                 (unsigned int)len >= (1u << (8 * nal_unit_size))))
		return -1;
	if (au_reserve(au, prefix + len))
		return -1;
	unsigned char *p = au->data + au->size;
	if (nal_unit_size) {
		for (int i = 0; i < nal_unit_size; i++)
			p[i] = (unsigned char)((unsigned int)len >> (8 * (nal_unit_size - 1 - i)));
	} else {
		p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1;
	}
	memcpy(p + prefix, nal, (size_t)len);
	au->size += prefix + len;
	return 0;
}

// Registered ITU-T T.35 SEI carrying SMPTE ST 2094-40 (HDR10+): country 0xB5,
// provider 0x003C, provider-oriented code 0x0001, application identifier 4.
static int sei_is_hdr10plus(int type, const unsigned char *payload, int size)
{
	return type == 4 && size >= 6 && payload[0] == 0xB5 && payload[1] == 0x00 &&
		payload[2] == 0x3C && payload[3] == 0x00 && payload[4] == 0x01 && payload[5] == 0x04;
}

/* Remove HDR10+ messages from a prefix SEI NAL, preserving the other messages.
 * Returns 1 when out_nal holds a replacement NAL (out_len 0: drop the NAL),
 * 0 when the NAL is unchanged, -1 when it cannot be parsed (keep it). */
static int sei_strip_hdr10plus(const unsigned char *nal, int len, unsigned char **out_nal, int *out_len)
{
	*out_nal = NULL;
	*out_len = 0;
	if (len < 3)
		return -1;
	unsigned char *rbsp = malloc((size_t)len);
	unsigned char *kept = malloc((size_t)len);
	if (!rbsp || !kept) {
		free(rbsp);
		free(kept);
		return -1;
	}
	int rbsp_len = 0, zeros = 0;
	for (int i = 2; i < len; i++) {
		if (zeros >= 2 && nal[i] == 3) {
			zeros = 0;
			continue;
		}
		rbsp[rbsp_len++] = nal[i];
		zeros = nal[i] ? 0 : zeros + 1;
	}
	int i = 0, kept_len = 0, removed = 0, ret = -1;
	// more_rbsp_data(): stop at the trailing 0x80 byte.
	while (i < rbsp_len && !(i == rbsp_len - 1 && rbsp[i] == 0x80)) {
		int start = i, type = 0, size = 0;
		while (i < rbsp_len && rbsp[i] == 0xFF) { type += 255; i++; }
		if (i >= rbsp_len) goto Exit;
		type += rbsp[i++];
		while (i < rbsp_len && rbsp[i] == 0xFF) { size += 255; i++; }
		if (i >= rbsp_len) goto Exit;
		size += rbsp[i++];
		if (size > rbsp_len - i) goto Exit;
		if (sei_is_hdr10plus(type, rbsp + i, size)) {
			removed++;
		} else {
			memcpy(kept + kept_len, rbsp + start, (size_t)(i + size - start));
			kept_len += i + size - start;
		}
		i += size;
	}
	if (!removed) {
		ret = 0;
		goto Exit;
	}
	ret = 1;
	if (!kept_len)
		goto Exit;
	// Header + escaped messages + trailing bits; escaping adds at most len/2.
	unsigned char *out = malloc((size_t)(2 + kept_len + kept_len / 2 + 2));
	if (!out) {
		ret = -1;
		goto Exit;
	}
	int o = 0;
	out[o++] = nal[0];
	out[o++] = nal[1];
	kept[kept_len++] = 0x80;
	zeros = 0;
	for (int k = 0; k < kept_len; k++) {
		if (zeros >= 2 && kept[k] <= 3) {
			out[o++] = 3;
			zeros = 0;
		}
		out[o++] = kept[k];
		zeros = kept[k] ? 0 : zeros + 1;
	}
	*out_nal = out;
	*out_len = o;
Exit:
	free(rbsp);
	free(kept);
	return ret;
}

#ifdef CONFIG_LIBDOVI
/* Convert a Profile 7 RPU to Profile 8.1. Returns 1 with a libdovi buffer to
 * release with dovi_data_free(), 0 when the RPU is not Profile 7, -1 on error. */
static int rpu_convert_p81(const unsigned char *nal, int len, const DoviData **out, const char **error)
{
	*out = NULL;
	*error = NULL;
	DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu(nal, (size_t)len);
	if (!rpu)
		return -1;
	int ret = -1;
	const DoviRpuDataHeader *header = dovi_rpu_get_header(rpu);
	if (!header) {
		*error = "parse";
		goto Exit;
	}
	int profile = header->guessed_profile;
	dovi_rpu_free_header(header);
	if (profile != 7) {
		ret = 0;
		goto Exit;
	}
	if (dovi_convert_rpu_with_mode(rpu, 2) != 0) {
		*error = "convert";
		goto Exit;
	}
	const DoviData *data = dovi_write_unspec62_nalu(rpu);
	if (!data || !data->data || !data->len || data->len > 0x7fffffff) {
		if (data)
			dovi_data_free(data);
		*error = "write";
		goto Exit;
	}
	*out = data;
	ret = 1;
Exit:
	dovi_rpu_free(rpu);
	return ret;
}
#endif

unsigned char *video_dovi_rewrite_au(VIDEO_PROPERTIES *video, const unsigned char *data,
	int size, int nal_unit_size, int *out_size)
{
	if (out_size)
		*out_size = 0;
	if (!video || !data || size <= 0 || !out_size)
		return NULL;
	int rewrite = video->dv_rewrite & video_dovi_rewrite_supported();
	if (!rewrite)
		return NULL;
	DOVI_AU au = { 0 };
	int pos = 0, changed = 0, nal_size = 0, converted = 0, dropped_el = 0, stripped = 0, r;
	const unsigned char *nal = NULL;
	while ((r = video_dovi_next_hevc_nal(data, size, nal_unit_size, &pos, &nal, &nal_size)) == 1) {
		int type = (nal[0] >> 1) & 0x3f;
		if ((rewrite & VIDEO_DOVI_REWRITE_P7_TO_P81) && type == VIDEO_DOVI_NAL_EL) {
			dropped_el++;
			changed = 1;
			continue;
		}
#ifdef CONFIG_LIBDOVI
		if ((rewrite & VIDEO_DOVI_REWRITE_P7_TO_P81) && type == VIDEO_DOVI_NAL_RPU) {
			const DoviData *rpu = NULL;
			const char *error = NULL;
			int c = rpu_convert_p81(nal, nal_size, &rpu, &error);
			if (c == 1) {
				int put = au_put_nal(&au, rpu->data, (int)rpu->len, nal_unit_size);
				dovi_data_free(rpu);
				if (put)
					goto Fail;
				converted++;
				changed = 1;
				continue;
			}
			if (c < 0) {
				video->dv_rewrite_errors++;
				if (video->dv_rewrite_errors == 1)
					serprintf("Dolby Vision rewrite: RPU %s failed, keeping packets unchanged\r\n",
						error ? error : "parse");
				goto Fail;
			}
		}
#endif
		if ((rewrite & VIDEO_DOVI_REWRITE_STRIP_HDR10PLUS) && type == VIDEO_DOVI_NAL_SEI_PREFIX) {
			unsigned char *sei = NULL;
			int sei_len = 0;
			if (sei_strip_hdr10plus(nal, nal_size, &sei, &sei_len) == 1) {
				int put = sei_len ? au_put_nal(&au, sei, sei_len, nal_unit_size) : 0;
				free(sei);
				if (put)
					goto Fail;
				stripped++;
				changed = 1;
				continue;
			}
		}
		if (au_put_nal(&au, nal, nal_size, nal_unit_size))
			goto Fail;
	}
	if (r < 0 || !changed)
		goto Fail;
	if (!video->dv_rewrite_logged) {
		video->dv_rewrite_logged = 1;
		serprintf("Dolby Vision rewrite: rpu_p7_to_p81=%d el_nal_dropped=%d hdr10plus_sei_removed=%d\r\n",
			converted, dropped_el, stripped);
	}
	*out_size = au.size;
	return au.data;
Fail:
	free(au.data);
	return NULL;
}
