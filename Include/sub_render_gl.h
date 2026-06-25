#pragma once

#include "sub_types.h"
#include <android/native_window.h>

typedef struct SUB_RENDERER SUB_RENDERER;

SUB_RENDERER *sub_render_gl_create(void);
void          sub_render_gl_destroy(SUB_RENDERER *r);

void sub_render_gl_attach_surface(SUB_RENDERER *r, ANativeWindow *window);
void sub_render_gl_detach_surface(SUB_RENDERER *r);
void sub_render_gl_resize(SUB_RENDERER *r, int width, int height);

void sub_render_gl_clear(SUB_RENDERER *r);
void sub_render_gl_invalidate_cache(SUB_RENDERER *r);
void sub_render_gl_set_ui_mode(SUB_RENDERER *r, int mode);

// --- HYBRID 3D BRIDGE ---
// Extracts the current frame into an Android CPU Bitmap for the Java 3D Shader
int sub_render_gl_fill_bitmap(SUB_RENDERER *r, void* pixels, int dst_w, int dst_h, int dst_stride);
