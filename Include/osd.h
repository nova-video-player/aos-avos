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

#ifndef _OSD_H
#define _OSD_H

#include "rect.h"
#include "types.h"

//----------------------------------------------------
//	OSD constants
//----------------------------------------------------
// OSD profiles
// Warning : when adding or removing some profiles please check the tables in AVOS which depend of OSD_NUM_PROFILES
enum {
	OSD_LCD_LANDSCAPE = 0,
	OSD_LCD_PORTRAIT,
	OSD_PAL_4_3,
	OSD_PAL_16_9,
	OSD_NTSC_4_3,
	OSD_NTSC_16_9,
	OSD_PAL_PLUS,
        OSD_HDMI_VGA,
        OSD_HDMI_480P_16_9,
        OSD_HDMI_576P_16_9,
        OSD_HDMI_720P50,
	OSD_HDMI_720P60,
	OSD_HDMI_768,
	OSD_HDMI_1024,
	OSD_HDMI_1080P,
	OSD_NUM_PROFILES	// must always be the last
};

/* possible output configuration */
#define OSD_OUTPUT_COMPOSITE		0	
#define OSD_OUTPUT_SVIDEO		1
#define OSD_OUTPUT_COMPONENT		2
#define OSD_OUTPUT_RGB			3

typedef struct {
	unsigned char* addr;	// Adress of the FB start
	int depth;		// data format (depth in bit)
	int line_length;	// length in byte of one line in memory
	int map_size;		// size of mapped memory in byte
	int x_l;		// Width (pixels)
	int y_l;		// Height (pixels)
	int x_p;		// Horizontal position
	int y_p;		// Vertical position
	int x_offset;		// horizontal offset ("virtual desktop")
	int y_offset;		// vertical offset ("virtual desktop")
	int zoom_x;		// Horizontal zoom (0 = x1, 1 = x2, 2 = x4)
	int zoom_y;		// Vertical zoom
} OsdWindowProperties;

typedef struct {
	int x_p;		// Video Window Horizontal position
	int y_p;		// Video Window Vertical position
	int x_l;		// Video Window Width (pixels)
	int y_l;		// Video Window Height (pixels)
	int x_osd_offset;	// OSD Horizontal offset (from top left of video window)
	int y_osd_offset;	// OSD vertical offset (from top left of video window)
	int x_l_osd;		// OSD Window Width (pixels)
	int y_l_osd;		// OSD Window Height (pixels)
	int pixel_ratio_n;	// pixel ratio of the display
	int pixel_ratio_d;	// pixel ratio of the display
} OsdWindowProfile;

typedef struct {
	int x_p;		// Real Video Window Horizontal position
	int y_p;		// Real Video Window Vertical position
	int x_l;		// Real Video Window Width (pixels)
	int y_l;		// Real Video Window Height (pixels)
} OsdVideoProfile;

#define 	PAL_PIXEL_RATIO_N		59
#define 	PAL_PIXEL_RATIO_D		54

#define 	NTSC_PIXEL_RATIO_N		10
#define 	NTSC_PIXEL_RATIO_D		11

#define 	PAL_PLUS_PIXEL_RATIO_N		118
#define 	PAL_PLUS_PIXEL_RATIO_D		81

#define 	LCD_PIXEL_RATIO_N		1
#define 	LCD_PIXEL_RATIO_D		1

#define         HDMI_VGA_PIXEL_RATIO_N     1
#define         HDMI_VGA_PIXEL_RATIO_D     1

#define         HDMI_480P_4_3_PIXEL_RATIO_N      8
#define         HDMI_480P_4_3_PIXEL_RATIO_D      9

#define         HDMI_480P_16_9_PIXEL_RATIO_N     32
#define         HDMI_480P_16_9_PIXEL_RATIO_D     27
#define 	HDMI_480P_16_9_OPERA_PAR	 0.94f

#define         HDMI_576P_4_3_PIXEL_RATIO_N      16
#define         HDMI_576P_4_3_PIXEL_RATIO_D      15

#define         HDMI_576P_16_9_PIXEL_RATIO_N     64
#define         HDMI_576P_16_9_PIXEL_RATIO_D     45
#define 	HDMI_576P_16_9_OPERA_PAR	 0.77f

#define         HDMI_720P_PIXEL_RATIO_N     1
#define         HDMI_720P_PIXEL_RATIO_D     1

// OSD & video planes settings
#define LCD_OSD_OFFSX			0
#define LCD_OSD_OFFSY			0

// offsets tv for Gen6 
#define PAL_4_3_OSD_OFFSX		32 + 30
#define PAL_4_3_OSD_OFFSY		24 + 24

#define PAL_16_9_OSD_OFFSX              16 + 26
#define PAL_16_9_OSD_OFFSY              42 + 48

#define NTSC_4_3_OSD_OFFSX              48 + 18
#define NTSC_4_3_OSD_OFFSY              24 + 24

#define NTSC_16_9_OSD_OFFSX		32 + 14
#define NTSC_16_9_OSD_OFFSY		29 + 43

#define PAL_PLUS_OSD_OFFSX		30 + 12 /* FIXME 42 in SF */
#define PAL_PLUS_OSD_OFFSY		6 + 12 /* FIXME 18 in SF */


#define TV_PAL_MAX_WIDTH		720
#define TV_PAL_OUT_WIDTH		672
#define TV_PAL_MAX_HEIGHT		576 // 574

#define TV_NTSC_MAX_WIDTH		720
#define TV_NTSC_OUT_WIDTH		698	//666
#define TV_NTSC_MAX_HEIGHT		480

#define HDMI_VGA_WIDTH                 640
#define HDMI_VGA_HEIGHT                480
#define HDMI_VGA_OSD_OFFSX              18
#define HDMI_VGA_OSD_OFFSY              16

#define HDMI_480P_WIDTH                 720
#define HDMI_480P_HEIGHT                480
#define HDMI_480P_OSD_OFFSX              34
#define HDMI_480P_OSD_OFFSY               8

#define HDMI_576P_WIDTH                 720
#define HDMI_576P_HEIGHT                576
#define HDMI_576P_OSD_OFFSX              34
#define HDMI_576P_OSD_OFFSY              18

#define HDMI_720P_WIDTH                 1280
#define HDMI_720P_HEIGHT                720
#define HDMI_720P_OSD_OFFSX              48
#define HDMI_720P_OSD_OFFSY              32

#define HDMI_768_WIDTH                 1024
#define HDMI_768_HEIGHT                768
#define HDMI_768_OSD_OFFSX              0
#define HDMI_768_OSD_OFFSY              0

#define HDMI_1024_WIDTH                 1280
#define HDMI_1024_HEIGHT                1024
#define HDMI_1024_OSD_OFFSX              0
#define HDMI_1024_OSD_OFFSY              0

#define HDMI_1080P_WIDTH                 1920
#define HDMI_1080P_HEIGHT                1080
#define HDMI_1080P_OSD_OFFSX              0
#define HDMI_1080P_OSD_OFFSY              0

#define 	BLEND_VIDEO_0		0
#define		BLEND_VIDEO_1		1

// Hardware transparency levels for a 32bits OSD
#define 	BLEND_OPAQUE		255
#define 	BLEND_HIGH		191
#define 	BLEND_MEDIUM_HIGH	159
#define 	BLEND_MEDIUM		127
#define		BLEND_MEDIUM_LOW	95
#define 	BLEND_LOW		63
#define 	BLEND_VERY_LOW		31
#define 	BLEND_TRANSPARENT	0

#define		BLEND_MSGBOX_BG		BLEND_VERY_LOW

#define		ALPHA_OPAQUE		255
#define		ALPHA_TRANSPARENT	0

#define		OSD_FADING_OFFSET	32


#ifdef CONFIG_HDMI
# define OSD0_SHADOW_YOFFSET	720	  // DISP_YRES_MAX from davincifb.c
#else
# define OSD0_SHADOW_YOFFSET	640	  // DISP_YRES_MAX from davincifb.c
#endif

//----------------------------------------------------
//	OSD variables
//----------------------------------------------------
extern int		OSD_profile;
extern OsdWindowProfile	OSD_display_info;

//----------------------------------------------------
//	OSD functions
//----------------------------------------------------
void    OSD_set_lcd_size( int width, int height );

void	OSD_InitDisplay( BOOL bChangeGeometry ) ;
void	OSD_BlendVideo( int mode );
void	OSD_SetProfile( int profile );
void	OSD_InitSize( int profile );
void	OSD_StartDisplay(void);
void	OSD_StopDisplay(void);
void	OSD_StartGFX(void);
void	OSD_StopGFX(void);
void	OSD_CropGFX( Rect *area );
void	OSD_Stop( void );
void	OSD_EnableAttribWindow( void );
#ifdef CONFIG_GUI
int	OSD_IsLCD( void );
#else
static inline int OSD_IsLCD( void ) { return 1; }
#endif
int	OSD_IsNTSC( void );
int	OSD_IsPAL( void );
int	OSD_IsAnalogTV( void );
int	OSD_Is720p( void );
int	OSD_IsWide( void );
int	OSD_IsPortrait( void );
void	OSD_GetMaxVideoResolution( int *max_width, int *max_height );
void	OSD_SetExternalVideoFormat( void );
void	OSD_SetVideoDefaultProperties( int profile );
void	OSD_get_frame_rate( int *rate, int *scale );
Rect	OSD_GetVisibleArea( void );
float	OSD_GetPixelAspectRatio( void );

void	OSD_MakeOpaqueAndResetTimeout( void );
void	OSD_MakeOpaque( void );
void	OSD_MakeTransparent( void );
int	OSD_IsOpaque( void );
int	OSD_IsTransparent( void );

typedef enum {
	ORIENTATION_LANDSCAPE = 0,
	ORIENTATION_PORTRAIT
} OrientationMode;

OrientationMode OSD_GetDeviceOrientation( void );
void            OSD_SetDeviceOrientation( OrientationMode mode );

#endif
