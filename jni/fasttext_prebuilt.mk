# Copyright 2026 Courville Software
# SPDX-License-Identifier: Apache-2.0
# Static fastText from native/fastText-android-builder. Optional until the
# builder/prebuilt repositories are distributed with the manifest.

FASTTEXT_PREBUILT_ROOT := $(LOCAL_PATH)/../../prebuilt/fastText

ifneq ($(wildcard $(FASTTEXT_PREBUILT_ROOT)/lib/$(TARGET_ARCH_ABI)/libfasttext.a),)
include $(CLEAR_VARS)
LOCAL_MODULE := fasttext_prebuilt
LOCAL_SRC_FILES := ../../prebuilt/fastText/lib/$(TARGET_ARCH_ABI)/libfasttext.a
LOCAL_EXPORT_C_INCLUDES := $(FASTTEXT_PREBUILT_ROOT)/include
LOCAL_CPP_FEATURES := exceptions rtti
include $(PREBUILT_STATIC_LIBRARY)
endif
