LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := libdeinterlace$(AVOS_LIBS_SUFFIX)

LOCAL_SRC_FILES := deinterlace.c

LOCAL_C_INCLUDES :=

LOCAL_SHARED_ANDROID_LIBRARIES :=

ifeq ($(TARGET_ARCH),arm)
LOCAL_ARM_MODE := arm
ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
LOCAL_CFLAGS += -DARM_HAS_NEON -no-integrated-as
LOCAL_ARM_NEON := true
LOCAL_SRC_FILES += neon_deinterlace.S
endif
endif

include $(BUILD_SHARED_LIBRARY)
