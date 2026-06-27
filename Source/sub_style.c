#include "sub_style.h"
#include <stdlib.h>
#include <string.h>

// Helper: Android ARGB (0xAARRGGBB) -> Libass RGBA (0xRRGGBBAA where AA is transparency)
static uint32_t argb_to_libass(uint32_t argb) {
    uint32_t a = (argb >> 24) & 0xFF;
    uint32_t r = (argb >> 16) & 0xFF;
    uint32_t g = (argb >> 8)  & 0xFF;
    uint32_t b =  argb        & 0xFF;

    // Libass treats Alpha as "Transparency" (0x00 = Solid, 0xFF = Invisible)
    uint32_t libass_transparency = 255 - a;

    return (r << 24) | (g << 16) | (b << 8) | libass_transparency;
}

SUB_USER_STYLE* sub_style_create(void) {
    SUB_USER_STYLE *style = calloc(1, sizeof(SUB_USER_STYLE));

    style->font_size     = 55.0f;
    style->font_scale    = 1.0f;
    style->font_family   = strdup("roboto medium"); // Locked internal fontconfig target
    style->is_bold       = 0;
    style->text_color    = argb_to_libass(0xFFFFFFFF); // Solid White

    style->bg_mode       = 0;                          // Default: Floating Text
    style->bg_color      = argb_to_libass(0x88000000); // 50% Transparent Black

    style->outline_width = 2;
    style->outline_color = argb_to_libass(0xFF000000); // Solid Black

    style->shadow_width  = 2;
    style->shadow_color  = argb_to_libass(0xAA000000); // Drop Shadow

    style->margin_bottom = 0;
    style->override_mode = 1;                          // Default: Force Custom
    style->serial        = 1;

    return style;
}

void sub_style_destroy(SUB_USER_STYLE *style) {
    if (!style) return;
    if (style->font_family) free(style->font_family);
    free(style);
}

int sub_style_get_serial(const SUB_USER_STYLE *style) {
    return style ? style->serial : 0;
}

// --- SNAPSHOT (For Engine Sync) ---
void sub_style_snapshot(const SUB_USER_STYLE *style, SUB_USER_STYLE *out) {
    if (!style || !out) return;

    // Perform a fast memory copy of the entire struct.
    // Shallow copy of the font_family pointer is perfectly safe here
    // because sync_styles() only reads it transiently.
    memcpy(out, style, sizeof(SUB_USER_STYLE));
}

// --- MASTER & GEOMETRY SETTERS ---

void sub_style_set_override_mode(SUB_USER_STYLE *style, int mode) {
    if (style && style->override_mode != mode) {
        style->override_mode = mode;
        style->serial++;
    }
}

void sub_style_set_margin_bottom(SUB_USER_STYLE *style, int margin) {
    if (style && style->margin_bottom != margin) {
        style->margin_bottom = margin;
        style->serial++;
    }
}

// --- TYPOGRAPHY SETTERS ---

void sub_style_set_font_size(SUB_USER_STYLE *style, float pt) {
    if (style && style->font_size != pt) {
        style->font_size = pt;
        style->serial++;
    }
}

void sub_style_set_font_scale(SUB_USER_STYLE *style, float scale) {
    if (style && style->font_scale != scale) {
        style->font_scale = scale;
        style->serial++;
    }
}

void sub_style_set_font_family(SUB_USER_STYLE *style, const char *family) {
    if (!style || !family) return;
    if (style->font_family && strcmp(style->font_family, family) == 0) return;
    if (style->font_family) free(style->font_family);
    style->font_family = strdup(family);
    style->serial++;
}

void sub_style_set_bold(SUB_USER_STYLE *style, int bold) {
    if (style && style->is_bold != bold) {
        style->is_bold = bold;
        style->serial++;
    }
}

void sub_style_set_text_color(SUB_USER_STYLE *style, uint32_t argb) {
    if (!style) return;
    uint32_t converted = argb_to_libass(argb);
    if (style->text_color != converted) {
        style->text_color = converted;
        style->serial++;
    }
}

// --- INTERDEPENDENT BACKGROUNDS, OUTLINES & SHADOWS ---

void sub_style_set_bg_mode(SUB_USER_STYLE *style, int mode) {
    if (style && style->bg_mode != mode) {
        style->bg_mode = mode;
        style->serial++;
    }
}

void sub_style_set_bg_color(SUB_USER_STYLE *style, uint32_t argb) {
    if (!style) return;
    uint32_t converted = argb_to_libass(argb);
    if (style->bg_color != converted) {
        style->bg_color = converted;
        style->serial++;
    }
}

void sub_style_set_outline_width(SUB_USER_STYLE *style, float px) {
    int w = (int)px;
    if (style && style->outline_width != w) {
        style->outline_width = w;
        style->serial++;
    }
}

void sub_style_set_outline_color(SUB_USER_STYLE *style, uint32_t argb) {
    if (!style) return;
    uint32_t converted = argb_to_libass(argb);
    if (style->outline_color != converted) {
        style->outline_color = converted;
        style->serial++;
    }
}

void sub_style_set_shadow_width(SUB_USER_STYLE *style, float px) {
    int w = (int)px;
    if (style && style->shadow_width != w) {
        style->shadow_width = w;
        style->serial++;
    }
}

void sub_style_set_shadow_color(SUB_USER_STYLE *style, uint32_t argb) {
    if (!style) return;
    uint32_t converted = argb_to_libass(argb);
    if (style->shadow_color != converted) {
        style->shadow_color = converted;
        style->serial++;
    }
}
