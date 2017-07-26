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

#include "global.h"
#include "types.h"
#include "fb.h"
#include "platform.h"
#include "debug.h"

#include <stdlib.h>
#include <string.h>

#include "fb_modes.h"

#define DBG if(Debug[DBG_FB])

FRAME_BUFFER fb;

int FB_connect( FRAME_BUFFER *fb )
{
	FB_CONNECT fb_connect = platform_get_fb_connect();
	if( fb_connect ) {
serprintf("connecting to FB:\r\n" ); 
		return fb_connect( fb );
	}
	return 1;
}

void FB_update( int flags )
{
	fb.fb_ops.update( flags );
}

void FB_set_osd_active( int active )
{
DBG serprintf("FB_set_osd_active( %d )\r\n", active );
	fb.fb_ops.set_osd_active( active );
}

void FB_set_osd_mix( int mix )
{
DBG serprintf("FB_set_osd_mix( %d )\r\n", mix );
	fb.fb_ops.set_osd_mix( mix );
}

void FB_set_osd_blend( int blend )
{
DBG serprintf("FB_set_osd_blend( %d )\r\n", blend );
	fb.fb_ops.set_osd_blend( blend );
}

void FB_set_video_active( int active )
{
DBG serprintf("FB_set_video_active( %d )\r\n", active );
	fb.fb_ops.set_video_active( active );
}

void FB_set_alpha_active( int active )
{
DBG serprintf("FB_set_alpha_active( %d )\r\n", active );
	fb.fb_ops.set_alpha_active( active );
}

int FB_set_display_mode( int tvmode, int output )
{
DBG serprintf("FB_set_display_mode( %d, %d )\r\n", tvmode, output);
	return fb.fb_ops.set_display_mode(tvmode, output);
}

int FB_set_osd_properties(const OsdWindowProperties* props)
{
	return fb.fb_ops.set_osd_properties(props);
}

int FB_set_video_properties(const OsdWindowProperties* props)
{
	return fb.fb_ops.set_video_properties(props);
}

void FB_set_display_enable( int active )
{
DBG serprintf("FB_set_display_enable(%i)\r\n", active);
	fb.fb_ops.set_display_enable(active);
}

void FB_set_crop(int x, int y, int width, int height)
{
DBG serprintf("FB_set_crop(%dx%d  %dx%d)\r\n", x, y, width, height);
	if( fb.fb_ops.set_crop ) {
		fb.fb_ops.set_crop( x, y, width, height );
	}
}

void FB_dump_window_info(OsdWindowProperties* win_info, char *label)
{
	serprintf("%s dimensions %4d x %4d/%4d x %2d\n", label, win_info->x_l, win_info->y_l, win_info->line_length, win_info->depth);
	serprintf("%s address    %08X  size %d\n",       label, win_info->addr, win_info->map_size );
	serprintf("\n");

}

#ifdef DEBUG_MSG
// ********************************************************************************
// ********************************************************************************
//
//	D E B U G stuff
//
// ********************************************************************************
// ********************************************************************************

static void _osd( int argc, char *argv[] )
{
	if (argc < 2)
		return;
	FB_set_osd_active( atoi( argv[1] ) );
}

static void _osd_alpha( int argc, char *argv[] )
{
	if (argc < 2)
		return;
	FB_set_alpha_active( atoi( argv[1] ) );
}

static void _osd_mix( int argc, char *argv[] )
{
	if (argc < 2)
		return;
	FB_set_osd_mix( atoi( argv[1] ) );
}

static void _osd_blend( int argc, char *argv[] )
{
	if (argc < 2)
		return;
	FB_set_osd_blend( atoi( argv[1] ) );
}

static void _osd_v( int argc, char *argv[] )
{
	if (argc < 2)
		return;
	FB_set_video_active( atoi( argv[1] ) );
}

static void _osd_crop( int argc, char *argv[] )
{
	if (argc < 5)
		return;
	FB_set_crop( atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]) );
}

static void _osd_format( int argc, char *argv[] )
{
	if (argc < 2)
		return;
	
	if (!strcmp(argv[1], "pal_sv")) {
		FB_set_display_mode( MODE_TV_PAL, OSD_OUTPUT_SVIDEO );
	} else if (!strcmp(argv[1], "pal_cvbs")) {
		FB_set_display_mode( MODE_TV_PAL, OSD_OUTPUT_COMPOSITE );
	} else if (!strcmp(argv[1], "pal_cm")) {
		FB_set_display_mode( MODE_TV_PAL, OSD_OUTPUT_COMPONENT );
	} else if (!strcmp(argv[1], "pal_rgb")) {
		FB_set_display_mode( MODE_TV_PAL, OSD_OUTPUT_RGB );
	} else if (!strcmp(argv[1], "ntsc_sv")) {
		FB_set_display_mode( MODE_TV_NTSC, OSD_OUTPUT_SVIDEO );
	} else if (!strcmp(argv[1], "ntsc_cvbs")) {
		FB_set_display_mode( MODE_TV_NTSC, OSD_OUTPUT_COMPOSITE );
	} else if (!strcmp(argv[1], "ntsc_cm")) {
		FB_set_display_mode( MODE_TV_NTSC, OSD_OUTPUT_COMPONENT );
	} else if (!strcmp(argv[1], "ntsc_rgb")) {
		FB_set_display_mode( MODE_TV_NTSC, OSD_OUTPUT_RGB );
	} else if (!strcmp(argv[1], "lcd50")) {
		FB_set_display_mode( MODE_LCD_60HZ, OSD_OUTPUT_RGB );
	} else if (!strcmp(argv[1], "lcd60")) {
		FB_set_display_mode( MODE_LCD_50HZ, OSD_OUTPUT_RGB );
	} else if (!strcmp(argv[1], "lcd48_7")) {
		FB_set_display_mode( MODE_LCD_48_7HZ, OSD_OUTPUT_RGB );
        }else if (!strcmp(argv[1], "hdmi480p")) {
           FB_set_display_mode( MODE_HDMI_480P, OSD_OUTPUT_RGB );
        }else if (!strcmp(argv[1], "hdmi576p")) {
           FB_set_display_mode( MODE_HDMI_576P, OSD_OUTPUT_RGB );
        }else if (!strcmp(argv[1], "hdmivga")) {
           FB_set_display_mode( MODE_HDMI_VGA, OSD_OUTPUT_RGB );
        }else if (!strcmp(argv[1], "hdmi720p50")) {
           FB_set_display_mode( MODE_HDMI_720P_50, OSD_OUTPUT_RGB );
	}else if (!strcmp(argv[1], "hdmi720p60")) {
		FB_set_display_mode( MODE_HDMI_720P_60, OSD_OUTPUT_RGB );
	}
}

DECLARE_DEBUG_COMMAND("osd", 	_osd );
DECLARE_DEBUG_COMMAND("osda", 	_osd_alpha );
DECLARE_DEBUG_COMMAND("osdm", 	_osd_mix );
DECLARE_DEBUG_COMMAND("osdb", 	_osd_blend );
DECLARE_DEBUG_COMMAND("osdv", 	_osd_v );
DECLARE_DEBUG_COMMAND("osdf", 	_osd_format );
DECLARE_DEBUG_COMMAND("osdc", 	_osd_crop );
DECLARE_DEBUG_SWITCH( "dbgfb",	DBG_FB);

#endif
