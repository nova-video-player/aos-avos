# Copyright 2026 Courville Software
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

# libdovi_prebuilt.mk: static libdovi from native/libdovi-android-builder.
# avos_config_android.mk enables LIBDOVI only when the prebuilt exists.

LIBDOVI_PREBUILT_ROOT     := ../../prebuilt
LIBDOVI_PREBUILT_ROOT_INC := $(LOCAL_PATH)/../../prebuilt

ifneq ($(wildcard $(LIBDOVI_PREBUILT_ROOT_INC)/libdovi/lib/$(TARGET_ARCH_ABI)/libdovi.a),)
include $(CLEAR_VARS)
LOCAL_MODULE            := dovi_prebuilt
LOCAL_SRC_FILES         := $(LIBDOVI_PREBUILT_ROOT)/libdovi/lib/$(TARGET_ARCH_ABI)/libdovi.a
LOCAL_EXPORT_C_INCLUDES := $(LIBDOVI_PREBUILT_ROOT_INC)/libdovi/include
include $(PREBUILT_STATIC_LIBRARY)
endif
