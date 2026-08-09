/*
 * sub_style.h — User-configurable subtitle appearance settings.
 *
 * This is the C-side mirror of Nova's existing subtitle settings UI.
 * It is now a single struct, owned natively, with explicit setters
 * exposed to JNI (via SubtitleEngine.java).
 *
 * Applies to SUB_EVENT_TEXT events (SRT, plain SSA fallback) only.
 */

#pragma once

#include "sub_types.h"
#include <stdint.h>
#include <pthread.h>

/* ------------------------------------------------------------------
 * Global, persisted user style — one instance per player session.
 * Thread-safety: all setters take an internal lock; the renderer reads
 * a snapshot copy at the start of each frame, never a live pointer.
 * ------------------------------------------------------------------ */

typedef struct SUB_USER_STYLE {
    float    font_size;
    float    font_scale;
    char    *font_family;
    int      is_bold;
    uint32_t text_color;

    int      bg_mode;       // 0 = Disabled, 1 = Per-Line Box, 2 = Unified Block
    uint32_t bg_color;

    int      outline_width;
    uint32_t outline_color;

    int      shadow_width;  // Acts as Box Padding in Mode 2
    uint32_t shadow_color;

    int      margin_bottom; // Vertical Offset (evades Android UI)
    int      override_mode; // 0 = Embedded, 1 = Force Custom, 2 = Scale Only

    int      serial;        // Incremented on any style change to trigger cache invalidation

    pthread_mutex_t lock;    // Guards every field above + serial. See sub_style.c for why:
                             // setters run on the Android UI thread (via JNI), snapshot()
                             // runs on the native EGL render thread — without this lock,
                             // a setter's non-atomic "write field, then serial++" could
                             // interleave with snapshot()'s memcpy, producing a torn read
                             // (some fields updated, some not) or a fresh-fields/stale-serial
                             // mismatch that silently defers a style change to whenever the
                             // next unrelated change happens to land.
} SUB_USER_STYLE;

// --- LIFECYCLE ---
SUB_USER_STYLE* sub_style_create(void);
void sub_style_destroy(SUB_USER_STYLE *style);
int  sub_style_get_serial(const SUB_USER_STYLE *style);
void sub_style_snapshot(const SUB_USER_STYLE *style, SUB_USER_STYLE *out);

// --- MASTER & GEOMETRY SETTERS ---
void sub_style_set_override_mode(SUB_USER_STYLE *style, int mode);
void sub_style_set_margin_bottom(SUB_USER_STYLE *style, int margin);

// --- TYPOGRAPHY SETTERS ---
void sub_style_set_font_size(SUB_USER_STYLE *style, float pt);
void sub_style_set_font_scale(SUB_USER_STYLE *style, float scale);
void sub_style_set_font_family(SUB_USER_STYLE *style, const char *family);
void sub_style_set_bold(SUB_USER_STYLE *style, int bold);
void sub_style_set_text_color(SUB_USER_STYLE *style, uint32_t argb);

// --- INTERDEPENDENT BACKGROUNDS, OUTLINES & SHADOWS ---
void sub_style_set_bg_mode(SUB_USER_STYLE *style, int mode);
void sub_style_set_bg_color(SUB_USER_STYLE *style, uint32_t argb);
void sub_style_set_bg_opacity(SUB_USER_STYLE *style, uint8_t android_alpha);
void sub_style_set_outline_width(SUB_USER_STYLE *style, float px);
void sub_style_set_outline_color(SUB_USER_STYLE *style, uint32_t argb);
void sub_style_set_shadow_width(SUB_USER_STYLE *style, float px);
void sub_style_set_shadow_color(SUB_USER_STYLE *style, uint32_t argb);
