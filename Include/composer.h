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

#ifndef _COMPOSER_H_
#define _COMPOSER_H_

#include "image.h"
#include "rect.h"

typedef struct PaintSurface PaintSurface;
typedef struct Background   Background;
typedef struct SurfaceTool  SurfaceTool;

void Composer_init( void );
void Composer_deInit( void );
void Composer_setSizes( int width, int height, int background_width, int background_height );
void Composer_dump( void );

void Composer_blit( void );
void Composer_enableBlitting(    void );
void Composer_disableBlitting(   void );
void Composer_forceBlitting(     void );
int  Composer_isBlittingEnabled( void );

void Composer_setBlitterInhibit( int inhibit );

enum {
	MERGE,
	REPLACE
};

enum {
	BACKGROUND_DOUBLE_BUFFERED,
	BLIT_ALPHA_FIRST,
	SYNC_WITH_VD,
	BACKGROUND_BLURRED_HORIZONTALLY
};

Background   *Composer_getBackground( void );
PaintSurface *Composer_allocLayer(    int mem_type );
SurfaceTool  *Composer_getSurfaceTool( void );
void	      Composer_addLayer(      PaintSurface *ps, int mode );
void	      Composer_freeLayer(     PaintSurface *ps );
void	      Composer_setAlpha(      PaintSurface *ps, int alpha );
int	      Composer_getProperty(   int property );
void	      Composer_setProperty(   int property, int value );

void	      Composer_setBackgroundTransform(  rect *transform  );
void	      Composer_setBackgroundLuminosity( float luminosity );
void	      Composer_resetBackgroundLuminosity( void );
void          Composer_resetBackgroundTransform(  void );
void	      Composer_setBackgroundLayer( const IMAGE *image );
void	      Composer_deleteBackgroundLayer( const IMAGE *image );
void	      Composer_setBackgroundLayerAlpha( int alpha );
void 	      Composer_initBackbuffer( void );
void 	      Composer_initNoBackbuffer( void );

typedef enum {
	SURFACE_GRAYSCALE, 
	SURFACE_RGB16, 
	SURFACE_RGB16_A4, 
	SURFACE_RGB16_A8,
	SURFACE_BGRA32, 
	SURFACE_RGBA32,
	SURFACE_ARGB32,
	SURFACE_YUV422
} SurfaceType;

struct PaintSurface {
	SurfaceType type;
	USHORT *color_buf;
	int	color_ppr;
	int 	color_size;
	rect    bounds;
	rect	dirty_area;
	ImageMemoryType mem_type;
	int	direct;
};

PaintSurface* PaintSurface_create(SurfaceType format, ImageMemoryType mem_type, int width, int height);
void PaintSurface_initFor(PaintSurface *obj, IMAGE *img);
void PaintSurface_destroy(PaintSurface *obj);


struct Background {
	IMAGE  *img;
	PaintSurface surface;
	rect    dirty_area;
	int	type;
};

struct SurfaceTool {
 	void (*copy_aligned_rect_rgba32_rotated) ( PaintSurface *tgt, PaintSurface *src, rect *area, int alpha );
	void (*copy_aligned_rect_rgba32) ( PaintSurface *tgt, PaintSurface *src, rect *area, int alpha );
	void (*copy_yuv_image_rotated) ( int xpos, int ypos, const IMAGE *src, const IMAGE *dst, int fx_mask );
};

extern PaintSurface osd_surface;
extern SurfaceTool surface_tool;


#endif
