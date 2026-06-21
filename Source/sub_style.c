#include "sub_style.h"
#include <stdlib.h>
#include <string.h>

// Helper: Converts Android ARGB to Libass RGBA
static uint32_t argb_to_rgba(uint32_t argb) {
    uint8_t a = (argb >> 24) & 0xFF;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;

    // CRITICAL FIX: Libass treats 'A' as Transparency!
    // Android 255 (Solid) -> Libass 0 (Solid)
    uint8_t ass_alpha = 255 - a;

    return (r << 24) | (g << 16) | (b << 8) | ass_alpha;
}

SUB_USER_STYLE* sub_style_create(void) {
    SUB_USER_STYLE *style = calloc(1, sizeof(SUB_USER_STYLE));
    style->font_size = 24.0f;
    style->font_scale = 1.0f;
    style->text_color = 0xFFFFFFFF; // White
    style->outline_color = 0x000000FF; // Black
    style->outline_width = 2;
    style->bg_enabled = 0;
    style->bg_color = 0x00000088; // Semi-transparent black
    style->margin_bottom = 0;
    style->override_mode = ASS_OVERRIDE_SCALE;
    style->serial = 1;
    return style;
}

void sub_style_destroy(SUB_USER_STYLE *style) {
    if (style) {
        if (style->font_family) free(style->font_family);
        free(style);
    }
}

void sub_style_set_font_size(SUB_USER_STYLE *style, float pt) {
    if (style){
        style->font_size = pt;
        style->serial++;
    }
}

void sub_style_set_font_scale(SUB_USER_STYLE *style, float scale) {
    if (style) {
        style->font_scale = scale;
        style->serial++;
    }
}

void sub_style_set_font_family(SUB_USER_STYLE *style, const char *family_name) {
    if (style && family_name) {
        if (style->font_family) free(style->font_family);
        style->font_family = strdup(family_name);
        style->serial++;
    }
}

void sub_style_set_bold(SUB_USER_STYLE *style, int bold) {
    if (style) {
        style->is_bold = bold;
        style->serial++;
    }
}

void sub_style_set_italic(SUB_USER_STYLE *style, int italic) {
    if (style) {
        style->is_italic = italic;
        style->serial++;
    }
}

void sub_style_set_text_color(SUB_USER_STYLE *style, uint32_t argb_color) {
    if (style) {
        style->text_color = argb_to_rgba(argb_color);
        style->serial++;
    }
}

void sub_style_set_outline_color(SUB_USER_STYLE *style, uint32_t argb_color) {
    if (style) {
        style->outline_color = argb_to_rgba(argb_color);
        style->outline_width = 2;
        style->serial++;
    }
}

void sub_style_set_bg_color(SUB_USER_STYLE *style, uint32_t argb_color) {
    if (style) {
        style->bg_color = argb_to_rgba(argb_color);
        style->serial++;
    }
}

void sub_style_set_bg_opacity(SUB_USER_STYLE *style, float opacity) {
    if (style) {
        uint8_t android_alpha = (uint8_t)(255.0f * opacity);
        uint8_t ass_alpha = 255 - android_alpha; // INVERT FOR LIBASS

        style->bg_color = (style->bg_color & 0xFFFFFF00) | ass_alpha;
        style->serial++;
    }
}

void sub_style_set_vertical_offset(SUB_USER_STYLE *style, float fraction) {
    if (style) {
        style->margin_bottom = (int)fraction;
        style->serial++;
    }
}

void sub_style_set_force_override(SUB_USER_STYLE *style, int force) {
    if (style) {
        style->override_mode = force ? ASS_OVERRIDE_FORCE : ASS_OVERRIDE_NO;
        style->serial++;
    }
}

void sub_style_snapshot(const SUB_USER_STYLE *src, SUB_USER_STYLE *dst) {
    if (!src || !dst) return;
    char *old_family = dst->font_family;
    memcpy(dst, src, sizeof(SUB_USER_STYLE));
    dst->font_family = src->font_family ? strdup(src->font_family) : NULL;
    if (old_family) free(old_family);
}
