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

ifneq ($(NDK_ANDROID_L),true)

# not needed for android L and after

define sfdec_build_shared
include $(CLEAR_VARS)
LOCAL_MODULE := libsfdec.core.$(1)$(AVOS_LIBS_SUFFIX)
LOCAL_SRC_FILES := sfdec_omxcodec.cpp sfdec_common.cpp
ifneq ($(1),14)
LOCAL_SRC_FILES += sfdec_mediacodec.cpp
endif
LOCAL_C_INCLUDES := \
	$(AVOS_DIR)/common/Include \
	$(ANDROID_INCLUDES_$(1))
LOCAL_CFLAGS := -DSFDEC_ANDROID_API=$(1)
LOCAL_ALLOW_UNDEFINED_SYMBOLS := true
LOCAL_LDLIBS:= -L$(TARGET_OUT) -lstagefright -lmedia -lcutils -lutils -lbinder -lui
ifneq ($(TARGET_ARCH_ABI),x86)
LOCAL_LDLIBS += -fuse-ld=bfd
endif
include $(BUILD_SHARED_LIBRARY)
endef

# sfdec.core.*

$(eval $(call sfdec_build_shared,14))
$(eval $(call sfdec_build_shared,16))
$(eval $(call sfdec_build_shared,18))
$(eval $(call sfdec_build_shared,19))

endif # ifneq ($(NDK_ANDROID_L),true)

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
