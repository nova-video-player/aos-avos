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

AVOS_JNI_DIR := $(LOCAL_PATH)
AVOS_DIR := $(LOCAL_PATH)/..
EXTERNAL_DIR := $(AVOS_DIR)/external
ANDROID_DIR := $(EXTERNAL_DIR)/android
include $(ANDROID_DIR)/include/ndkbuild.mk

LIBAV_DIR := $(AVOS_DIR)/../ffmpeg-android-builder
ifeq ($(LIBAV_CONFIG),)
	LIBAV_CONFIG := base
endif
LIBAV_CONFIG_DIR := $(LIBAV_DIR)/dist-$(LIBAV_CONFIG)-$(TARGET_ARCH_ABI)

AUDIOCOMPRESS_DIR := $(AVOS_DIR)/../audiocompress

ifeq ($(TARGET_ARCH_ABI),armeabi)
AVOS_LIBS_SUFFIX := _no_neon
else
AVOS_LIBS_SUFFIX :=
endif

NDK_ANDROID_L := true
HAVE_ANDROID_SYSTEM_PROP := true

### libavos_android ###

LOCAL_PATH := $(ANDROID_DIR)/libavos_android
include  $(LOCAL_PATH)/ndkbuild.mk

### avos ###

LOCAL_PATH := $(AVOS_DIR)
include  $(LOCAL_PATH)/ndkbuild.mk

### audiocompress ###

include $(AUDIOCOMPRESS_DIR)/ndkbuild.mk

### libavosjni ###

LOCAL_PATH := $(AVOS_JNI_DIR)/libavosjni
include $(LOCAL_PATH)/ndkbuild.mk

### libsfdec ###

LOCAL_PATH := $(ANDROID_DIR)/libsfdec
include $(LOCAL_PATH)/ndkbuild.mk

### deinterlace ###

LOCAL_PATH := $(EXTERNAL_DIR)/libdeinterlace
include $(LOCAL_PATH)/ndkbuild.mk

### colorconversion ###

LOCAL_PATH := $(ANDROID_DIR)/colorconversion/
include $(LOCAL_PATH)/ndkbuild.mk

### libcputest ###

LOCAL_PATH := $(AVOS_JNI_DIR)/libcputest
include $(LOCAL_PATH)/ndkbuild.mk

$(call import-module,android/cpufeatures)
