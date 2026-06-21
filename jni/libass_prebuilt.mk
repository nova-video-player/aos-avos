# libass-prebuilt.mk

LIBASS_PREBUILT_ROOT     := ../../prebuilt
LIBASS_PREBUILT_ROOT_INC := $(LOCAL_PATH)/../../prebuilt

# ── libxml2 (Required for fontconfig XML parsing) ───────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := xml2_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_PREBUILT_ROOT)/libxml2/lib/$(TARGET_ARCH_ABI)/libxml2.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_PREBUILT_ROOT_INC)/libxml2/include/libxml2
include $(PREBUILT_STATIC_LIBRARY)

# ── zlib (Required for FreeType and libpng) ─────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := z_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_PREBUILT_ROOT)/zlib/lib/$(TARGET_ARCH_ABI)/libz.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_PREBUILT_ROOT_INC)/zlib/include
include $(PREBUILT_STATIC_LIBRARY)

# ── libpng (Required for Color Emojis) ──────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := png_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_PREBUILT_ROOT)/libpng/lib/$(TARGET_ARCH_ABI)/libpng16.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_PREBUILT_ROOT_INC)/libpng/include/libpng16
include $(PREBUILT_STATIC_LIBRARY)

# ── fontconfig (Required for Android system font auto-detect) ───────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := fontconfig_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_PREBUILT_ROOT)/fontconfig/lib/$(TARGET_ARCH_ABI)/libfontconfig.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_PREBUILT_ROOT_INC)/fontconfig/include
include $(PREBUILT_STATIC_LIBRARY)

# ── libunibreak (Required for proper Unicode line-breaking) ─────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := unibreak_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_PREBUILT_ROOT)/libunibreak/lib/$(TARGET_ARCH_ABI)/libunibreak.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_PREBUILT_ROOT_INC)/libunibreak/include
include $(PREBUILT_STATIC_LIBRARY)

# ── freetype ────────────────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := freetype_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_PREBUILT_ROOT)/freetype/lib/$(TARGET_ARCH_ABI)/libfreetype.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_PREBUILT_ROOT_INC)/freetype/include/freetype2
include $(PREBUILT_STATIC_LIBRARY)

# ── fribidi ─────────────────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := fribidi_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_PREBUILT_ROOT)/fribidi/lib/$(TARGET_ARCH_ABI)/libfribidi.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_PREBUILT_ROOT_INC)/fribidi/include
include $(PREBUILT_STATIC_LIBRARY)

# ── harfbuzz ────────────────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := harfbuzz_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_PREBUILT_ROOT)/harfbuzz/lib/$(TARGET_ARCH_ABI)/libharfbuzz.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_PREBUILT_ROOT_INC)/harfbuzz/include/harfbuzz
include $(PREBUILT_STATIC_LIBRARY)

# ── libass ──────────────────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE            := ass_prebuilt
LOCAL_SRC_FILES         := $(LIBASS_PREBUILT_ROOT)/libass/lib/$(TARGET_ARCH_ABI)/libass.a
LOCAL_EXPORT_C_INCLUDES := $(LIBASS_PREBUILT_ROOT_INC)/libass/include
include $(PREBUILT_STATIC_LIBRARY)
