/*
 * Copyright 2017 Archos SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PAINTER_H
#define PAINTER_H

#include "awchar.h"
#include "color.h"
#include "composer.h"
#include "layout.h"
#include "font_size.h"
#include "rect.h"


struct Gfx_str;
typedef struct Gfx_str Gfx;


typedef enum {
	REPLACE_ALPHA,
	BLEND_ALPHA
} BlendMode;


typedef enum {
	FX_NONE      = 0,
	FX_INVERTED  = 0x10000000,
	FX_GHOSTED   = 0x20000000,
	FX_THEMED    = 0x40000000,
	FX_LIGHTENED = 0x80000000,

	FX_LEVEL_1   = 0x01000000,
	FX_LEVEL_2   = 0x02000000,
	FX_LEVEL_3   = 0x03000000,
	FX_LEVEL_4   = 0x04000000,
	FX_LEVEL_5   = 0x05000000,
	FX_LEVEL_6   = 0x06000000,
	FX_LEVEL_7   = 0x07000000,
	FX_LEVEL_8   = 0x08000000
} BitmapEffects;

#define FX_NUM_LEVEL	8
#define FX_LEVEL_MASK	0x0F000000
#define FX_LEVEL_OFFSET	24


// can be combined: f.e. ALIGN_LEFT | ALIGN_BOTTOM
typedef enum {
	ALIGN_CENTER = 0,
	ALIGN_LEFT   = 1,
	ALIGN_RIGHT  = 2,
	ALIGN_TOP    = 4,
	ALIGN_BOTTOM = 8
} Alignment;


typedef struct {
	PaintSurface 	*surface;
	const Gfx 	*gfx;

	BlendMode	mode;
	Rect		clip_rect;
	int		xofs, yofs;
	FontSize	font_size;
	BOOL		text_fading_enabled;
	Rect		text_area;
} Painter;

extern Painter app_p;

void Painter_initFor(Painter *obj, PaintSurface *surface);
Painter* Painter_createFor(PaintSurface *surface);

void Painter_setBlendMode(      Painter *obj, BlendMode mode);
void Painter_setDefaultMode(	BlendMode mode);
void Painter_setFontSize(       Painter *obj, FontSize font_size);
void Painter_setOrigin(         Painter *obj, int xofs, int yofs);
void Painter_moveOriginBy(      Painter *obj, int xofs, int yofs);
void Painter_setClipRect(       Painter *obj, const Rect *area);
void Painter_restrictClipRectTo(Painter *obj, const Rect *area);
void Painter_enableTextFading(  Painter *obj, BOOL active, Rect *text_area);

BlendMode Painter_getBlendMode(const Painter *obj);
FontSize Painter_getFontSize(  const Painter *obj);
void Painter_getOrigin(        const Painter *obj, int *xofs, int *yofs);
Rect Painter_getClipRect(      const Painter *obj);

void Painter_drawHLine(const Painter *obj, int xpos, int ypos, int length, rgba color);
void Painter_drawVLine(const Painter *obj, int xpos, int ypos, int length, rgba color);
void Painter_drawRect( const Painter *obj, const Rect *area, rgba color);
void Painter_fillRect( const Painter *obj, const Rect *area, rgba color);

void Painter_drawChar(     const Painter *obj, int xpos, int ypos, wchar ch, rgba color, int left_alpha, int right_alpha);
void Painter_drawText(     const Painter *obj, int xpos, int ypos, const char *txt, rgba color);
void Painter_drawBitmap(   const Painter *obj, int xpos, int ypos, int bitmap_id);
void Painter_drawBitmapFx( const Painter *obj, int xpos, int ypos, int bitmap_id, BitmapEffects fx);
void Painter_drawHStripe(  const Painter *obj, int xpos, int ypos, int length, int bitmap_id);
void Painter_drawHStripeFx(const Painter *obj, int xpos, int ypos, int length, int bitmap_id, BitmapEffects fx);
void Painter_drawVStripe(  const Painter *obj, int xpos, int ypos, int length, int bitmap_id);
void Painter_drawVStripeFx(const Painter *obj, int xpos, int ypos, int length, int bitmap_id, BitmapEffects fx);
void Painter_drawImage(    const Painter *obj, int xpos, int ypos, const IMAGE *image);

//convenience methods
void Painter_drawOutlinedChar(const Painter *obj, int xpos, int ypos, wchar ch, rgba color);
void Painter_drawOutlinedText(const Painter *obj, int xpos, int ypos, const char *txt, rgba color);
void Painter_drawAlignedBitmap(const Painter *obj, const Rect *area, Alignment alignment, int bitmap_id, BitmapEffects fx);
void Painter_drawAlignedText(  const Painter *obj, const Rect *area, Alignment alignment, const char *txt, rgba color, BOOL outlined);

// for testing only - do not use
void Painter_drawSurface(const Painter *obj, int xpos, int ypos, const PaintSurface *surface);
void Painter_moveArea(   const Painter *obj, const Rect *area, int dx, int dy);

//---------------------------------------------------------------------

struct Gfx_str {
	void (*onBlitHLine)(   PaintSurface *dst, int xpos, int ypos, int length, rgba color);
	void (*onBlendHLine)(  PaintSurface *dst, int xpos, int ypos, int length, rgba color);
	void (*onBlitVLine)(   PaintSurface *dst, int xpos, int ypos, int length, rgba color);
	void (*onBlendVLine)(  PaintSurface *dst, int xpos, int ypos, int length, rgba color);
	void (*onBlitMask)(    PaintSurface *dst, int xpos, int ypos, const UCHAR *mask, const Rect *src_rect, int bpr, rgba color, int left_alpha, int right_alpha);
	void (*onBlendMask)(   PaintSurface *dst, int xpos, int ypos, const UCHAR *mask, const Rect *src_rect, int bpr, rgba color, int left_alpha, int right_alpha);
	void (*onBlitSurface)( PaintSurface *dst, int xpos, int ypos, const PaintSurface *src, const Rect *src_rect);
	void (*onBlendSurface)(PaintSurface *dst, int xpos, int ypos, const PaintSurface *src, const Rect *src_rect);
	void (*onMoveArea)(    PaintSurface *dst, const Rect *area, int dx, int dy);
};


#endif // PAINTER_H
