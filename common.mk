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

CFLAGS =
LDFLAGS =
ifeq ($(ASAN),1)
LDFLAGS += -g -fsanitize=address
endif
DEFINES =
INCLUDES =
AVOS_SHARED_LIBS =
AVOS_STATIC_LIBS =
SHARED_LIBS =
CSRC =

INCLUDES += -I$(LOCAL_PATH)/Include
INCLUDES += -I$(LOCAL_PATH)/common/Include

LIBINCLUDES = -I$(INCPATH)

ifeq (BETA,$(BUILD))
	DEFINES += -DCONFIG_RELEASE
	DEFINES += -DNDEBUG
endif
ifeq (RELEASE,$(BUILD))
	DEFINES += -DCONFIG_RELEASE
	DEFINES += -DNDEBUG
endif

ifdef $(PRODUCT)
DEFINES += -D$(PRODUCT)
endif
ifeq ($(CLI),ON)
DEFINES += -DCONFIG_CLI
endif

ifeq ($(ARCH),arm)
DEFINES += -DARM
ifeq ($(BUILD_FROM),ARCBUILD)
DEFINES += -DUCLIBC
endif
endif

ifeq ($(TGT_BASE),sim)
DEFINES += -DSIM
else
ifeq ($(BUILD_FROM),ANDROID)
DEFINES += -DBIONIC
DEFINES += -DCONFIG_MEDIACENTER
endif
endif

# needed in order to handle files > 2GB correctly
SYSFLAGS = -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE
DEFINES += $(SYSFLAGS)

ASMFLAGS = $(DEFINES)

CFLAGS = -pipe -Werror-implicit-function-declaration -Wall -Wno-missing-braces \
	 -Wpointer-arith -Wno-cast-align -fno-strict-aliasing -Wno-pointer-sign \
	 -Wno-format-security -Wno-deprecated-declarations
CFLAGS += -Wno-unused -Wno-sign-compare -Wno-missing-field-initializers #TODO
CFLAGS += -Werror -no-integrated-as

ifeq ($(ASAN),1)
CFLAGS += -g -fsanitize=address -fno-omit-frame-pointer
endif

ifeq (gcc,$(CCFLAVOR))
	CFLAGS += -Wno-misleading-indentation
endif

ifeq (DEBUG,$(BUILD))
	ifeq (ON,$(DBG_ALLOC))
		DEFINES += -DDMALLOC -DDMALLOC_FUNC_CHECK
		STATIC_LIBS += $(TOOLCHAIN_PATH)/usr/lib/libdmallocth.a
	endif
endif

# shared libs that we link against
ifeq ($(ARCH),i586)
SHARED_LIBS += -lrt
endif

SHARED_LIBS += -ldl

# AVOS LIB
# 
CSRC_AVOS_CORE = \
	libavos.c \
	atime.c athread.c cbe.c \
	debug.c util.c serial.c \
	i18n.c bits.c \
	iso639.c iso3166.c \
	awchar.c  \
	fb_dummy.c file.c file_type.c \
	av.c av_dump.c bmp.c frame_q.c\
	image.c image_resize.c\
	rect.c  \
	file_info.c\
	linked_list.c  \
	browse.c ac_av.c object.c\
	sysfs_ll.c \
	thumb_storage.c thumb_stream.c \
	pixel_utils.c pixel_utils_neon.c \
	device_config.c

CSRC_AVOS_CORE += mainloop.c dataevent.c \
	threadcom.c timers.c avos_lifetime.c

ifneq ($(FLAVOR),ANDROID)
CSRC_AVOS_CORE += avsh.c
endif

CSRC_STREAM_CORE = \
	stream.c stream_buffer.c stream_buffer_raw.c stream_buffer_cooked.c \
	stream_oem.c stream_queue.c xdm_utils.c \
	stream_audio.c  \
	stream_video.c \
	stream_config.c \
	stream_alloc.c \
	stream_dumper.c stream_heap.c \
	stream_global.c stream_sync.c  

CSRC_STREAM_MISC = \
	mpeg2.c h264.c mpg4.c realvideo.c wmv.c downmix.c pts_reorder.c hevc.c

CSRC_STREAM_IO = \
	stream_io_file.c \
	stream_io_fd.c

CSRC_STREAM_PARSER = \
	stream_parser.c \
	stream_parser_ffmpeg.c \
	dts.c
	
CSRC_STREAM_CODEC = \
	codec_yuv.c \
	codec_ssa.c codec_textsub.c vobsub.c codec_vobsub.c codec_utils.c 

CSRC_STREAM_SINK = \
	stream_sink_audio.c stream_sink_audio_fake.c stream_sink_video.c stream_sink_video_sim2.c\
	stream_sink_video_fake.c \
	stream_resizer_dss.c

CSRC_STREAM_SUB = \
	stream_subtitle.c stream_sub_ext.c \
	subtitle_formats.c subtitle_ssa.c subtitle_srt.c subtitle_smi.c subtitle_sub.c subtitle_idx.c \
	subtitle_mpl2.c

CSRC_AUDIO = \
	id3tag.c mp3.c \
	pcm_autogain.c
	
CSRC += $(CSRC_AVOS_CORE) $(CSRC_STREAM_CORE) $(CSRC_STREAM_MISC) $(CSRC_STREAM_IO) \
        $(CSRC_STREAM_PARSER) $(CSRC_STREAM_CODEC) $(CSRC_STREAM_SINK) $(CSRC_STREAM_SOURCE) \
	$(CSRC_STREAM_SUB) $(CSRC_AUDIO)

#
# AVOS APP
#
CSRC_APP = \
	main.c app_start.c app_stop.c
		
CSRC_CLI = cli.c cli_video.c fb.c platform.c

ifeq ($(CLI),ON)
CSRC_APP += $(CSRC_CLI)
endif

# assembler sources
XASRC = 

ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
XASRC += neon_yuv.S neon_rgb.S neon_memcpy.S neon_memset.S
endif

SUBMAKEFILES = $(addprefix $(LOCAL_PATH)/, codecs.mk main.mk sound.mk fb.mk)
EXTRAMAKEFILES = $(addprefix $(LOCAL_PATH)/, extra.mk )

include $(SUBMAKEFILES)
-include $(EXTRAMAKEFILES)
