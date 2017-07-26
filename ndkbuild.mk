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

ANDROID_SRC := $(ANDROID_DIR)
### avos common config ###

ARCH := $(TARGET_ARCH)
PRODUCT := $(TARGET_PRODUCT)
FLAVOR := ANDROID
LIBAVOS := YES
BUILD_FROM := ANDROID

LOCAL_PATH := $(AVOS_DIR)
CONFIGMAKEFILE := $(AVOS_DIR)/avos_config_android.mk
include $(CONFIGMAKEFILE)
include $(AVOS_DIR)/common.mk

INCLUDES +=  $(ANDROID_DIR)/obj/include/


### libyuv ###
include $(CLEAR_VARS)
LOCAL_MODULE    := libyuv
LOCAL_SRC_FILES := $(LIBYUV_DIR)/obj/local/$(TARGET_ARCH_ABI)/libyuv_static.a
LOCAL_EXPORT_C_INCLUDES := $(LIBYUV_DIR)/include
include $(PREBUILT_STATIC_LIBRARY)

### libavos ###

include $(CLEAR_VARS)
AVOS_DEPS := $(AVOS_DIR)/Android.mk $(AVOS_DIR)/common.mk $(CONFIGMAKEFILE) $(SUBMAKEFILES)

LOCAL_SRC_FILES := $(addprefix Source/, $(CSRC)) \
	$(addprefix Source/, $(XASRC))

LOCAL_C_INCLUDES := $(patsubst -I%,%,$(INCLUDES))
LOCAL_C_INCLUDES += $(OMX_INCLUDES)
LOCAL_C_INCLUDES += $(ANDROID_DIR)/libavos_android/include

LOCAL_CFLAGS := $(CFLAGS) $(DEFINES)

#uncomment for clang, it does not like gnu extensions
#LOCAL_CFLAGS += -Wno-error=gnu-variable-sized-type-not-at-end

LOCAL_LDFLAGS := $(LDFLAGS)

LOCAL_SHARED_LIBRARIES := $(patsubst -l%,lib%, $(addsuffix $(AVOS_LIBS_SUFFIX), $(AVOS_SHARED_LIBS)))

LOCAL_LDLIBS := -L$(TARGET_OUT) -lz \
	$(SHARED_LIBS) \
	$(addsuffix $(AVOS_LIBS_SUFFIX), $(AVOS_SHARED_LIBS)) \
	-llog

ifneq ($(TARGET_ARCH_ABI),x86)
LOCAL_LDLIBS += -fuse-ld=bfd
endif

LOCAL_STATIC_LIBRARIES := $(sort $(addsuffix $(AVOS_LIBS_SUFFIX), $(AVOS_STATIC_LIBS))) \
	cpufeatures libyuv

LOCAL_MODULE := libavos$(AVOS_LIBS_SUFFIX)

LOCAL_MODULE_TAGS := optional

ifeq ($(TARGET_ARCH),arm)
LOCAL_ARM_MODE := arm
ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
LOCAL_CFLAGS += -DARM_HAS_NEON
LOCAL_ARM_NEON := true
endif
endif
LOCAL_LDLIBS += -L$(LIBAV_CONFIG_DIR)/lib

ifeq ($(HAVE_ANDROID_SYSTEM_PROP),true)
LOCAL_CFLAGS += -DHAVE_ANDROID_SYSTEM_PROP
endif

include $(BUILD_ANDROID_LIBS)
include $(BUILD_SHARED_LIBRARY)
