# not needed for android L and after
ifneq ($(NDK_ANDROID_L),true)

LOCAL_PATH := $(call my-dir)

define libiomx_build_shared
include $(CLEAR_VARS)
LOCAL_MODULE := libiomx.$(1)$(AVOS_LIBS_SUFFIX)
LOCAL_SRC_FILES := iomx.cpp
LOCAL_C_INCLUDES := \
	$(ANDROID_INCLUDES_$(1))
LOCAL_ALLOW_UNDEFINED_SYMBOLS := true
LOCAL_LDLIBS:= -L$(TARGET_OUT) -lstagefright -lmedia -lcutils -lutils -lbinder -lui
ifneq ($(TARGET_ARCH_ABI),x86)
LOCAL_LDLIBS += -fuse-ld=bfd
endif
LOCAL_CFLAGS := -DLIBIOMX_ANDROID_API=$(1)
include $(BUILD_SHARED_LIBRARY)
endef

$(eval $(call libiomx_build_shared,14))
$(eval $(call libiomx_build_shared,16))
$(eval $(call libiomx_build_shared,18))
$(eval $(call libiomx_build_shared,19))

include $(BUILD_ANDROID_LIBS)

endif # ifneq ($(NDK_ANDROID_L),true)
