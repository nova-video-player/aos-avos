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

ifeq ($(BUILD_FROM),ARCBUILD)
audiocompress: 
	$(MAKE) --directory ../audiocompress ARCH=$(ARCH) REL=$(REL) $(ARCH)/audiocompress.a

audiocompress-clean:
	$(MAKE) --directory ../audiocompress clean
endif #($(BUILD_FROM),ARCBUILD)

ifeq ($(AUDIO),ON)
 	DEFINES += -DCONFIG_AUDIO
	
	DEFINES += -DCONFIG_MP3			# MP3 
	DEFINES += -DCONFIG_PCM			# PCM/ADPCM
	DEFINES += -DCONFIG_WMA			# WMA
	DEFINES += -DCONFIG_WMA_PRO		# WMA PRO	
	DEFINES += -DCONFIG_WMA_SPEECH		# WMA SPEECH	
	DEFINES += -DCONFIG_WMA_LOSSLESS	# WMA LOSSLESS	
	DEFINES += -DCONFIG_AAC			# AAC
	DEFINES += -DCONFIG_FLAC		# FLAC
	DEFINES += -DCONFIG_OGG			# OGG VORBIS
	DEFINES += -DCONFIG_WAVPACK		# WAVPACK
	DEFINES += -DCONFIG_TTA			# TTA
	DEFINES += -DCONFIG_COOK		# RA8LBR aka COOK
	
	SHARED_LIBS += -lz

       ifeq ($(AUDIO_FF_AMR),ON)
		DEFINES += -DCONFIG_AMR
        endif
        ifeq ($(AUDIO_FF_AMR_WB),ON)
 		DEFINES += -DCONFIG_AMR
       endif
	
	ifeq ($(ARCH),i586)
		DEFINES += -DCONFIG_DTS			# DTS
		DEFINES += -DCONFIG_FF_MP3
		DEFINES += -DCONFIG_FF_AAC
		DEFINES += -DCONFIG_FF_WMA
		DEFINES += -DCONFIG_FF_VORBIS
		DEFINES += -DCONFIG_FF_WMA_PRO
	else
		# OGG VORBIS
		DEFINES += -DCONFIG_FF_VORBIS

		DEFINES += -DCONFIG_DTS			# DTS
		DEFINES += -DCONFIG_FF_MP3
		DEFINES += -DCONFIG_FF_AAC
		DEFINES += -DCONFIG_FF_WMA
		DEFINES += -DCONFIG_FF_WMA_PRO
	endif	

	ifeq ($(AUDIO_COMPRESS),ON)
		DEFINES += -DCONFIG_AUDIO_COMPRESS
		CSRC_AUDIO += stream_filter_audio_compress.c
		ifeq ($(TGT_BASE),sim)
			INCLUDES += -I$(LOCAL_PATH)/../audiocompress
			#static version
			STATIC_LIBS += ../audiocompress/$(ARCH)/audiocompress.a
			AVOS_DEPS += audiocompress
			PHONY_TARGETS += audiocompress
		else
			INCLUDES += -I$(AUDIOCOMPRESS_DIR)
			AVOS_SHARED_LIBS += -laudiocompress
		endif
	endif
	ifeq ($(AUDIO_AGC),ON)
		DEFINES += -DCONFIG_AUDIO_AGC
		CSRC_AUDIO += stream_filter_audio_agc.c
	endif
endif

ifeq ($(VIDEO),ON)
        DEFINES += -DCONFIG_VIDEO
        DEFINES += -DCONFIG_STREAM		# for video playing backend
	
	DEFINES += -DCONFIG_ASF			# parser:  ASF video container format
	DEFINES += -DCONFIG_MPEG_PS		# parser:  MPEG2 program stream
	DEFINES += -DCONFIG_MPEG_TS		# parser:  MPEG2 transport stream
	DEFINES += -DCONFIG_MPEG_TS_FF		# parser:  MPEG2 transport stream via FFmpeg/libav
	DEFINES += -DCONFIG_PSI_PARSE		# parser:  MPEG2 PSI/SI (in transport stream)
	DEFINES += -DCONFIG_MP4			# parser:  MP4
	DEFINES += -DCONFIG_FLV			# parser:  FLV
	DEFINES += -DCONFIG_MKV			# parser:  Matroska
	DEFINES += -DCONFIG_WTV			# parser:  WTV via lavf
	
	DEFINES += -DCONFIG_MPG4		# decoder: MPG4	
	DEFINES += -DCONFIG_MPEG2		# decoder: MPEG2  
	DEFINES += -DCONFIG_DIVX311		# decoder: DIVX311
	DEFINES += -DCONFIG_H264		# decoder: H.264  
	DEFINES += -DCONFIG_HEVC		# decoder: HEVC
	DEFINES += -DCONFIG_WMV			# decoder: WMV9   
	DEFINES += -DCONFIG_VC1			# decoder: VC1   
	DEFINES += -DCONFIG_MJPG		# decoder: MJPG   
	DEFINES += -DCONFIG_VP6			# decoder: ON2 VP6
	DEFINES += -DCONFIG_H263		# decoder: H263 for 3GPP files
	DEFINES += -DCONFIG_3GP
	
	DEFINES += -DCONFIG_SUBTITLES
        DEFINES += -DCONFIG_VOBSUB

	ifeq ($(VIDEO_FFMPEG),ON)
		# FFmpeg
		DEFINES += -DCONFIG_FFMPEG_VIDEO
		DEFINES += -DCONFIG_FFMPEG_AUDIO
		DEFINES += -DCONFIG_FFMPEG_PARSER
		CSRC_STREAM_CODEC += codec_ffmpeg_video.c 
		CSRC_STREAM_CODEC += codec_lavc_async.c 
		CSRC_STREAM_CODEC += codec_ffmpeg_HD.c
		CSRC_STREAM_CODEC += codec_ffmpeg_audio.c 
		
		ifneq (,$(LIBAV_CONFIG_DIR))
			INCLUDES += -I$(LIBAV_CONFIG_DIR)/include
		endif
		AVOS_SHARED_LIBS += -lavcodec -lavutil -lavformat -lavfilter
	endif

	ifeq ($(VIDEO_REALVIDEO),ON)
		DEFINES += -DCONFIG_REALVIDEO		# decoder: RV8+9/10
	endif

	ifeq ($(VIDEO_FF_VP6),ON)
		DEFINES += -DCONFIG_FF_VP6		# VP6 decoder using ffmpeg/libav
	endif
	ifeq ($(VIDEO_FF_VP8),ON)
		DEFINES += -DCONFIG_FF_VP8		# VP8 decoder using ffmpeg/libav
	endif
	ifeq ($(VIDEO_FF_VP9),ON)
		DEFINES += -DCONFIG_FF_VP9              # VP8 decoder using ffmpeg/libav
	endif
	ifeq ($(VIDEO_FF_MPEG2),ON)
		DEFINES += -DCONFIG_FF_MPEG2		# MPEG2 decoder using ffmpeg/libav
	endif
	ifeq ($(VIDEO_FF_MPEG4),ON)
		DEFINES += -DCONFIG_FF_MPEG4		# MPEG4 decoder using ffmpeg/libav
	endif
	ifeq ($(VIDEO_FF_H264),ON)
		DEFINES += -DCONFIG_FF_H264		# H264 decoder using ffmpeg/libav
	endif
	ifeq ($(VIDEO_FF_HEVC),ON)
		DEFINES += -DCONFIG_FF_HEVC		# HEVC/H.265 decoder using ffmpeg/libav
	endif
	ifeq ($(VIDEO_FF_WMV),ON)
		DEFINES += -DCONFIG_FF_WMV		# WMV/VC1 decoder using ffmpeg/libav
	endif
	ifeq ($(VIDEO_FF_RV1020),ON)
		DEFINES += -DCONFIG_FF_RV1020		# RV10/RV20 decoder using ffmpeg/libav
	endif
	ifeq ($(VIDEO_FF_RV3040),ON)
		DEFINES += -DCONFIG_FF_RV3040		# RV30/RV40 decoder using ffmpeg/libav
	endif
	ifeq ($(VIDEO_OMX_MPEG2),ON)
		DEFINES += -DCONFIG_OMX_MPEG2
	endif
	ifeq ($(VIDEO_OMX_MPEG4),ON)
		DEFINES += -DCONFIG_OMX_MPEG4
	endif
	ifeq ($(VIDEO_OMX_H264),ON)
		DEFINES += -DCONFIG_OMX_H264
	endif
	ifeq ($(VIDEO_OMX_HEVC),ON)
		DEFINES += -DCONFIG_OMX_HEVC
	endif
	ifeq ($(VIDEO_OMX_WMV),ON)
		DEFINES += -DCONFIG_OMX_WMV
	endif
	ifeq ($(VIDEO_OMX_RV3040),ON)
		DEFINES += -DCONFIG_OMX_RV3040
	endif
	ifeq ($(VIDEO_OMX_VP6),ON)
		DEFINES += -DCONFIG_OMX_VP6
	endif
	ifeq ($(VIDEO_OMX_VP7),ON)
		DEFINES += -DCONFIG_OMX_VP7
	endif
	ifeq ($(VIDEO_OMX_VP8),ON)
		DEFINES += -DCONFIG_OMX_VP8
	endif
	ifeq ($(VIDEO_OMX_VP9),ON)
		DEFINES += -DCONFIG_OMX_VP9
	endif

	ifeq ($(ARCH),i586)
		DEFINES += -DCONFIG_FF_MPEG2
		DEFINES += -DCONFIG_FF_MPEG4
		DEFINES += -DCONFIG_FF_H264
		DEFINES += -DCONFIG_FF_WMV
		DEFINES += -DCONFIG_FF_RV1020
		DEFINES += -DCONFIG_FF_RV3040
	else	
		LIBDCE =
		LIBION =
		ifeq ($(SINK_VIDEO),ANDROID)
			CSRC_STREAM_SINK += stream_sink_video_android.c
			CSRC_STREAM_SINK += stream_sink_video_android2.c
			CSRC_STREAM_SINK += stream_sink_video_android3.c
			DEFINES += -DCONFIG_SINK_VIDEO_ANDROID
		endif
		OMX-y :=
		ifeq ($(OMX),IOMX)
			OMX-y := ON
		endif
		ifeq ($(OMX),CORE)
			OMX-y := ON
		endif
		ifeq ($(OMX-y),ON)
			DEFINES += -DCONFIG_OMX
			DEFINES += -DCONFIG_OMX_$(OMX)
			CSRC_STREAM_CODEC += codec_OMX.c OMX_utils.c
			INCLUDES += -I$(OMX_INCLUDES)
		endif
		ifeq ($(SFDEC),ON)
			DEFINES += -DCONFIG_SFDEC
			AVOS_SHARED_LIBS += -lsfdec
			CSRC_STREAM_CODEC += codec_sfdec.c codec_sfdec2.c codec_mediacodec_audio.c 
			INCLUDES += -I$(ANDROID_DIR)/libsfdec
		endif
	endif

	ifeq ($(DEINTERLACE),ON)
		AVOS_SHARED_LIBS += -ldeinterlace
	endif
endif
