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
include $(CLEAR_VARS)

LOCAL_MODULE := libavos_android$(AVOS_LIBS_SUFFIX)

LOCAL_SRC_FILES := android_buffer.c android_window.c

LOCAL_C_INCLUDES := $(LOCAL_PATH)/include $(ANDROID_INCLUDES_21)

LOCAL_LDLIBS:= -L$(TARGET_OUT) -landroid #-lstagefright -lmedia -lcutils -lutils -lbinder -lui -lhardware

include $(BUILD_ANDROID_LIBS)
include $(BUILD_SHARED_LIBRARY)
