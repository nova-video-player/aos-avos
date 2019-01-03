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

LOCAL_PATH:= $(call my-dir)


### libnativehelper ###
LIBNATIVEHELPER_DIR := libnativehelper
include $(CLEAR_VARS)
LOCAL_MODULE    := libnativehelper
LOCAL_SRC_FILES := ../../../$(LIBNATIVEHELPER_DIR)/obj/local/$(TARGET_ARCH_ABI)/libnativehelper.so
LOCAL_EXPORT_C_INCLUDES :=  ../../../$(LIBNATIVEHELPER_DIR)/include
include $(PREBUILT_SHARED_LIBRARY)


include $(CLEAR_VARS)

LOCAL_CFLAGS := -O3 -DNDEBUG

LOCAL_SRC_FILES := \
	libavos.c \
	avos_media_player.c \
	avos_media_metadata_retriever.c

LOCAL_LDLIBS:= -L$(TARGET_OUT) -llog -landroid
ifneq ($(TARGET_ARCH_ABI),x86)
LOCAL_LDLIBS += -fuse-ld=bfd
endif

LOCAL_C_INCLUDES := \
	$(AVOS_DIR)/public

LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES := libnativehelper libavos$(AVOS_LIBS_SUFFIX)
LOCAL_MODULE := libavosjni$(AVOS_LIBS_SUFFIX)

include $(BUILD_SHARED_LIBRARY)

