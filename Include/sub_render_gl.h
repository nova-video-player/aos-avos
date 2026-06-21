#pragma once

#include "sub_types.h"
#include <android/native_window.h>

typedef struct SUB_RENDERER SUB_RENDERER;

SUB_RENDERER *sub_render_gl_create(void);
void          sub_render_gl_destroy(SUB_RENDERER *r);

void sub_render_gl_attach_surface(SUB_RENDERER *r, ANativeWindow *window);
void sub_render_gl_detach_surface(SUB_RENDERER *r);
void sub_render_gl_resize(SUB_RENDERER *r, int width, int height);

// NEW: Tells the GL thread to dump its current memory and clear the screen
void sub_render_gl_clear(SUB_RENDERER *r);

void sub_render_gl_invalidate_cache(SUB_RENDERER *r);
