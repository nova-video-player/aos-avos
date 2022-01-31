# Copyright 2017 Archos SA
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

GUI			= OFF

AUDIO			= ON
AUDIO_FF_AMR		= ON
AUDIO_FF_AMR_WB		= ON
AUDIO_COMPRESS		= ON

AUDIO_LIBAV		= ON
OMX			= IOMX
SFDEC			= ON
SINK_VIDEO		= ANDROID

VIDEO			= ON
VIDEO_REALVIDEO		= ON
VIDEO_FFMPEG		= ON
VIDEO_FF_VP6		= ON
VIDEO_FF_VP8		= ON
VIDEO_FF_VP9		= ON
VIDEO_FF_RV1020		= ON

# FFMPEG:
VIDEO_FF_MPEG2		= ON
VIDEO_FF_MPEG4		= ON
VIDEO_FF_H264		= ON
VIDEO_FF_HEVC		= ON
VIDEO_FF_WMV		= ON
VIDEO_FF_RV3040		= ON

LIBYUV			= ON

# IOMX:
VIDEO_OMX_MPEG2		= ON
VIDEO_OMX_MPEG4		= ON
VIDEO_OMX_H264		= ON
VIDEO_OMX_HEVC		= ON
VIDEO_OMX_WMV		= ON
VIDEO_OMX_RV3040	= ON
VIDEO_OMX_VP6		= OFF
VIDEO_OMX_VP7		= OFF
VIDEO_OMX_VP8		= ON
VIDEO_OMX_VP9		= ON

DEINTERLACE		= ON

AUDIO_INTERFACE		= ANDROID

#STATIC_FFMPEG		= ON

MASTER_OS		= ANDROID

FRAMEBUFFER		= OFF
SOUND			= ALSA

TTY			= ANDROID
HARDWARE		= ANDROID

