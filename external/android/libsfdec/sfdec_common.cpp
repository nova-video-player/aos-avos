// Copyright 2017 Archos SA
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sfdec_common.h"

const char *get_mimetype(sfdec_codec_t codec)
{
    switch (codec) {
        case SFDEC_VIDEO_VP8:
            return "video/x-vnd.on2.vp8";
        case SFDEC_VIDEO_VP9:
            return "video/x-vnd.on2.vp9";
        case SFDEC_VIDEO_AVC:
            return "video/avc";
        case SFDEC_VIDEO_HEVC:
            return "video/hevc";
        case SFDEC_VIDEO_MPEG4:
            return "video/mp4v-es";
        case SFDEC_VIDEO_H263:
            return "video/3gpp";
        case SFDEC_VIDEO_MPEG2:
            return "video/mpeg2";
        case SFDEC_VIDEO_WMV:
            return "video/x-ms-wmv";
	case SFDEC_AUDIO_AC3:
            return "audio/ac3";
	case SFDEC_AUDIO_EAC3:
	    return "audio/eac3";
	case SFDEC_AUDIO_DTS:
	    return "audio/vnd.dts";
	case SFDEC_AUDIO_DTS_HD:
	    return "audio/vnd.dts.hd";
	case SFDEC_AUDIO_OPUS:
	    return "audio/opus";
        case SFDEC_VIDEO_RAW:
        case SFDEC_VIDEO_UNKNOWN:
            return NULL;
    }
}

