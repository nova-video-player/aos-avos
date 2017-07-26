LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                     \
        ColorConverter.cpp

LOCAL_C_INCLUDES += \
	$(ANDROID_INCLUDES_14)

LOCAL_MODULE:= libstagefright_color_conversion

include $(BUILD_STATIC_LIBRARY)
