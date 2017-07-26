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

#ifndef _FB_H_
#define _FB_H_

#include "types.h"
#include "osd.h"

struct fb_str;
typedef struct fb_str FRAME_BUFFER;

#define FB_UPDATE_GFX	1
#define FB_UPDATE_VID	2
#define FB_UPDATE_ALL	3

struct fb_ops_str
{
	void	(*update)( int );
	void	(*set_osd_active)( int );
	void	(*set_osd_blend)( int );
	void	(*set_osd_mix)( int );
	void	(*set_video_active)( int );
	void	(*set_alpha_active)( int );
	void	(*set_global_alpha)( int );
	void	(*select_osd_buffer)( int );
	int	(*set_display_mode)( int, int );
	int	(*set_osd_properties)(const OsdWindowProperties*);
	int	(*set_video_properties)(const OsdWindowProperties*);
	void	(*set_display_enable)( int );
	void	(*set_crop)( int, int, int, int );
	unsigned char*	(*get_osd_data)( void );
};

typedef struct fb_ops_str FRAME_BUFFER_OPS;

struct fb_str
{
	OsdWindowProperties	osd_props;
	OsdWindowProperties	alpha_props;
	OsdWindowProperties	video0_props;
	FRAME_BUFFER_OPS	fb_ops;
};

extern int FB_connect( FRAME_BUFFER *fb );

extern void FB_update( int flags );

extern void FB_set_osd_active( int active );
extern void FB_set_osd_blend( int blend );
extern void FB_set_osd_mix( int mix );
extern void FB_set_video_active( int active );
extern void FB_set_alpha_active( int active );

extern int FB_set_display_mode( int tvmode, int output);
extern int FB_set_osd_properties(const OsdWindowProperties* props);
extern int FB_set_video_properties(const OsdWindowProperties* props);

extern void FB_set_display_enable(int active );
extern void FB_set_crop(int x, int y, int width, int height);
void FB_dump_window_info(OsdWindowProperties* win_info, char *label);

extern FRAME_BUFFER fb;

static inline int FB_get_osd_depth( void )
{
	return fb.osd_props.depth;
}

static inline int FB_get_osd_width( void )
{
	return fb.osd_props.x_l;
}

static inline int FB_get_osd_height( void )
{
	return fb.osd_props.y_l;
}

static inline int FB_get_osd_linestep( void )
{
	return fb.osd_props.line_length;
}

static inline int FB_get_alpha_width( void )
{
	return fb.alpha_props.x_l;
}

static inline int FB_get_alpha_height( void )
{
	return fb.alpha_props.y_l;
}

static inline int FB_get_alpha_linestep( void )
{
	return fb.alpha_props.line_length;
}

static inline int FB_get_video_width( void )
{
	return fb.video0_props.x_l;
}

static inline int FB_get_video_height( void )
{
	return fb.video0_props.y_l;
}

static inline int FB_get_video_linestep( void )
{
	return fb.video0_props.line_length;
}

static inline const OsdWindowProperties *FB_get_osd_window_properties( void )
{
	return &fb.osd_props;
}

static inline const OsdWindowProperties *FB_get_alpha_window_properties( void )
{
	return &fb.alpha_props;
}

static inline const OsdWindowProperties *FB_get_video_window_properties( void )
{
	return &fb.video0_props;
}

static inline unsigned char *FB_get_osd_data( void )
{
	return (unsigned char*)fb.fb_ops.get_osd_data();
}

static inline unsigned char *FB_get_alpha_data( void )
{
	return (unsigned char*)fb.alpha_props.addr;
}

static inline unsigned char *FB_get_video_data( void )
{
	return (unsigned char*)fb.video0_props.addr;
}

#endif
