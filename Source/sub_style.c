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

    pthread_mutex_init(&style->lock, NULL);

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
    pthread_mutex_destroy(&style->lock);
    free(style);
}

int sub_style_get_serial(const SUB_USER_STYLE *style) {
    if (!style) return 0;
    // Cast away const to lock: the mutex is a synchronization mechanism, not logical
    // state, so locking a "const" style here doesn't violate the const-correctness
    // contract from the caller's point of view.
    pthread_mutex_lock((pthread_mutex_t *)&style->lock);
    int serial = style->serial;
    pthread_mutex_unlock((pthread_mutex_t *)&style->lock);
    return serial;
}

// --- SNAPSHOT (For Engine Sync) ---
// IMPORTANT: out->font_family is a freshly-strdup'd deep copy, NOT the live pointer.
// The caller (sync_styles() in sub_format_ssa.c) owns this copy and MUST free() it once
// done reading — see the matching comment there. A shallow pointer copy would leave a
// window between this function returning and the caller finishing its use of
// out->font_family during which a concurrent sub_style_set_font_family() call could
// free() the string out from under the caller (use-after-free), since the lock below
// only protects the copy itself, not how long the caller holds onto the result.
void sub_style_snapshot(const SUB_USER_STYLE *style, SUB_USER_STYLE *out) {
    if (!style || !out) return;

    pthread_mutex_lock((pthread_mutex_t *)&style->lock);
    memcpy(out, style, sizeof(SUB_USER_STYLE));
    out->font_family = style->font_family ? strdup(style->font_family) : NULL;
    pthread_mutex_unlock((pthread_mutex_t *)&style->lock);

    // out->lock is a byte-copy of style's mutex and must never be locked/unlocked itself —
    // copying a live pthread_mutex_t and using the copy as a real mutex is undefined
    // behavior. Zero it defensively so any future code touching out->lock fails in the
    // most benign way available (a zero-initialized mutex behaves as a fresh unlocked
    // fast mutex on every mainstream pthreads implementation, unlike an aliased copy of a
    // potentially-contended mutex).
    memset(&out->lock, 0, sizeof(out->lock));
}

// --- MASTER & GEOMETRY SETTERS ---

void sub_style_set_override_mode(SUB_USER_STYLE *style, int mode) {
    if (!style) return;
    pthread_mutex_lock(&style->lock);
    if (style->override_mode != mode) {
        style->override_mode = mode;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

void sub_style_set_margin_bottom(SUB_USER_STYLE *style, int margin) {
    if (!style) return;
    pthread_mutex_lock(&style->lock);
    if (style->margin_bottom != margin) {
        style->margin_bottom = margin;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

// --- TYPOGRAPHY SETTERS ---

void sub_style_set_font_size(SUB_USER_STYLE *style, float pt) {
    if (!style) return;
    pthread_mutex_lock(&style->lock);
    if (style->font_size != pt) {
        style->font_size = pt;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

void sub_style_set_font_scale(SUB_USER_STYLE *style, float scale) {
    if (!style) return;
    pthread_mutex_lock(&style->lock);
    if (style->font_scale != scale) {
        style->font_scale = scale;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

void sub_style_set_font_family(SUB_USER_STYLE *style, const char *family) {
    if (!style || !family) return;
    pthread_mutex_lock(&style->lock);
    // font_family is a heap pointer that sub_style_snapshot()'s memcpy also copies —
    // this is the one setter where an unguarded free()+strdup() could race a concurrent
    // snapshot into copying a pointer that's mid-free (dangling) or mid-realloc, which is
    // strictly worse than the plain scalar races the other setters have (use-after-free
    // vs. a merely-stale value), so this lock matters more here than anywhere else.
    if (!style->font_family || strcmp(style->font_family, family) != 0) {
        if (style->font_family) free(style->font_family);
        style->font_family = strdup(family);
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

void sub_style_set_bold(SUB_USER_STYLE *style, int bold) {
    if (!style) return;
    pthread_mutex_lock(&style->lock);
    if (style->is_bold != bold) {
        style->is_bold = bold;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

void sub_style_set_text_color(SUB_USER_STYLE *style, uint32_t argb) {
    if (!style) return;
    uint32_t converted = argb_to_libass(argb);
    pthread_mutex_lock(&style->lock);
    if (style->text_color != converted) {
        style->text_color = converted;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

// --- INTERDEPENDENT BACKGROUNDS, OUTLINES & SHADOWS ---

void sub_style_set_bg_mode(SUB_USER_STYLE *style, int mode) {
    if (!style) return;
    pthread_mutex_lock(&style->lock);
    if (style->bg_mode != mode) {
        style->bg_mode = mode;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

void sub_style_set_bg_color(SUB_USER_STYLE *style, uint32_t argb) {
    if (!style) return;
    uint32_t converted = argb_to_libass(argb);
    pthread_mutex_lock(&style->lock);
    if (style->bg_color != converted) {
        style->bg_color = converted;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

void sub_style_set_bg_opacity(SUB_USER_STYLE *style, uint8_t android_alpha) {
    if (!style) return;
    uint8_t ass_transparency = 255 - android_alpha;
    pthread_mutex_lock(&style->lock);
    uint32_t new_color = (style->bg_color & 0xFFFFFF00) | ass_transparency;
    if (style->bg_color != new_color) {
        style->bg_color = new_color;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}
void sub_style_set_outline_width(SUB_USER_STYLE *style, float px) {
    if (!style) return;
    int w = (int)px;
    pthread_mutex_lock(&style->lock);
    if (style->outline_width != w) {
        style->outline_width = w;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

void sub_style_set_outline_color(SUB_USER_STYLE *style, uint32_t argb) {
    if (!style) return;
    uint32_t converted = argb_to_libass(argb);
    pthread_mutex_lock(&style->lock);
    if (style->outline_color != converted) {
        style->outline_color = converted;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

void sub_style_set_shadow_width(SUB_USER_STYLE *style, float px) {
    if (!style) return;
    int w = (int)px;
    pthread_mutex_lock(&style->lock);
    if (style->shadow_width != w) {
        style->shadow_width = w;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}

void sub_style_set_shadow_color(SUB_USER_STYLE *style, uint32_t argb) {
    if (!style) return;
    uint32_t converted = argb_to_libass(argb);
    pthread_mutex_lock(&style->lock);
    if (style->shadow_color != converted) {
        style->shadow_color = converted;
        style->serial++;
    }
    pthread_mutex_unlock(&style->lock);
}
