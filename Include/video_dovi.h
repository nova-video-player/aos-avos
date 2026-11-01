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

#ifndef VIDEO_DOVI_H
#define VIDEO_DOVI_H

#include "av.h"
#include <libavcodec/codec_id.h>

/* A decodable base codec is not necessarily a display-compatible base layer.
 * CCIDs 0/3/15 are proprietary, 5/7..14 reserved. Profile 5 always needs DV.
 * Keep the original bitstream profile: Android maps both profiles 7 and 8 to
 * DolbyVisionProfileDvheSt for device compatibility (phh/Kodi mapping).
 */
static inline int video_dovi_base_format(const VIDEO_PROPERTIES *video)
{
	if (!video->dv_bl_present_flag)
		return VIDEO_FORMAT_UNKNOWN;
	switch (video->dv_bl_signal_compatibility_id) {
	case 1: // HDR10
	case 2: // SDR
	case 4: // HLG
	case 6: // Blu-ray HDR
		break;
	default:
		return VIDEO_FORMAT_UNKNOWN;
	}
	if (video->codec_id == AV_CODEC_ID_H264 && video->dv_bitstream_profile == 9 &&
	    video->dv_bl_signal_compatibility_id == 2)
		return VIDEO_FORMAT_H264;
	if (video->codec_id == AV_CODEC_ID_AV1 && video->dv_bitstream_profile == 10)
		return VIDEO_FORMAT_AV1;
	if (video->codec_id == AV_CODEC_ID_HEVC &&
	    (video->dv_bitstream_profile == 4 || video->dv_bitstream_profile == 7 ||
	     video->dv_bitstream_profile == 8))
		return VIDEO_FORMAT_HEVC;
	return VIDEO_FORMAT_UNKNOWN;
}

/* Change the stream, not just a decoder-private copy, so subsequent hardware
 * and software candidates see the base codec. The one-way change also bounds
 * retries. Keep codec_id, DV metadata and packet framing for seeks/recovery.
 */
static inline int video_dovi_use_base(VIDEO_PROPERTIES *video)
{
	if (video->format != VIDEO_FORMAT_DOLBY_VISION)
		return 0;
	int format = video_dovi_base_format(video);
	if (format == VIDEO_FORMAT_UNKNOWN)
		return 0;
	video->format = format;
	video->fourcc = format == VIDEO_FORMAT_AV1 ? VIDEO_FOURCC_AV01 :
	    format == VIDEO_FORMAT_H264 ? VIDEO_FOURCC_H264 : VIDEO_FOURCC_HEVC;
	return 1;
}

#endif
