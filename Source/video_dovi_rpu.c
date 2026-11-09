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

#include <string.h>
#include <libavcodec/codec_id.h>

#ifdef CONFIG_LIBDOVI
#include <libdovi/rpu_parser.h>
#endif

int video_dovi_find_hevc_nal(const unsigned char *data, int size, int nal_unit_size,
	int type, const unsigned char **nal, int *nal_size)
{
	if (!data || size <= 0 || !nal || !nal_size)
		return 0;
	if (nal_unit_size) {
		if (nal_unit_size < 1 || nal_unit_size > 4)
			return 0;
		int pos = 0;
		while (size - pos >= nal_unit_size) {
			unsigned int len = 0;
			for (int i = 0; i < nal_unit_size; i++)
				len = (len << 8) | data[pos + i];
			pos += nal_unit_size;
			if (len > (unsigned int)(size - pos))
				return 0;
			if (len >= 2 && ((data[pos] >> 1) & 0x3f) == type) {
				*nal = data + pos;
				*nal_size = (int)len;
				return 1;
			}
			pos += (int)len;
		}
		return 0;
	}
	// Annex B: emulation prevention keeps 00 00 01 out of NAL payloads.
	int pos = 0;
	while (pos + 3 <= size) {
		if (data[pos] || data[pos + 1] || data[pos + 2] != 1) {
			pos++;
			continue;
		}
		int start = pos + 3;
		int end = start;
		while (end + 3 <= size && (data[end] || data[end + 1] || data[end + 2] != 1))
			end++;
		if (end + 3 > size)
			end = size;
		int last = end;
		while (last > start && !data[last - 1])
			last--;
		if (last - start >= 2 && ((data[start] >> 1) & 0x3f) == type) {
			*nal = data + start;
			*nal_size = last - start;
			return 1;
		}
		pos = end;
	}
	return 0;
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
