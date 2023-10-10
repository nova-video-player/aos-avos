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

LOCAL_PATH := $(call my-dir)

# sfdec.core.21 (using ndk API)

include $(CLEAR_VARS)
ANDROID_L_PLATFORMS := $(NDK_ROOT)/platforms/android-21/arch-$(TARGET_ARCH)
LOCAL_MODULE := libsfdec.core.21$(AVOS_LIBS_SUFFIX)
LOCAL_SRC_FILES := sfdec_ndkmediacodec.cpp codec_audio_mediacodec.cpp sfdec_common.cpp
LOCAL_C_INCLUDES := \
	$(AVOS_DIR)/common/Include \
	$(ANDROID_L_PLATFORMS)/usr/include
LOCAL_CFLAGS := -DSFDEC_ANDROID_API=21
LOCAL_ALLOW_UNDEFINED_SYMBOLS := true
LOCAL_LDLIBS:= -L$(ANDROID_L_PLATFORMS)/usr/lib -lmediandk -landroid
include $(BUILD_SHARED_LIBRARY)

# libsfdec
include $(CLEAR_VARS)
LOCAL_MODULE := libsfdec$(AVOS_LIBS_SUFFIX)
LOCAL_SRC_FILES := sfdec.c dec_audio.c
LOCAL_C_INCLUDES :=
LOCAL_CFLAGS :=

#libsfdec audio

include $(BUILD_ANDROID_LIBS)
include $(BUILD_SHARED_LIBRARY)
