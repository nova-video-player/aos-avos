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

#if defined(CONFIG_FB_QVFB)
#include "fb.h"
#include "qvfb.h"
#include "debug.h"
#include "types.h"
#include "hardware.h"
#include "platform.h"
#include "platform.h"
#include "fb_modes.h"

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <stdio.h>

static struct QVFbHeader *hdr;

int qvfb_brightness	= 255; //2009-05-28: Hack to have nice colors in simulator //( 127 + BACKLIGHT_DEFAULT_PARAM * 64 );
static int osd_mix	= 1;
static int osd_blend	= 0;

static int osd_active    = 0;
static int alpha_active  = 0;
static int video0_active = 0;
static int osd_actbuf    = 0;

static int display_mode = MODE_LCD_50HZ;

static void init_fb_ops( FRAME_BUFFER *fb );

static int QVFB_connect( FRAME_BUFFER *fb )
{
	unsigned char *shmrgn;
	int w;
	int h;
	int d;
	int lstep;

serprintf("Connecting to VFB server %s\r\n", QT_VFB_MOUSE_PIPE);
	key_t key = ftok(QT_VFB_MOUSE_PIPE, 'b');

	if (key == -1) {
serprintf("no key\r\n");
		return 1;
	}
	int shmId = shmget(key, 0, 0);
	if (shmId != -1)
		shmrgn = (unsigned char *) shmat(shmId, 0, 0);
	else {
serprintf("no shmId\r\n");
		return 1;
	}
	if ((long)shmrgn == -1 || shmrgn == 0) {
serprintf("no shmrgn\r\n");
		return 1;
	}

	hdr = (struct QVFbHeader *) shmrgn;

	w = hdr->width;
	h = hdr->height;
	d = hdr->depth;
	lstep = hdr->linestep;

serprintf("Connected to VFB server: %d x %d x %d\r\n", w, h, d);

	fb->osd_props.x_l = w;
	fb->osd_props.y_l = h;
	fb->osd_props.depth = d;
	fb->osd_props.line_length = lstep;
	
	fb->video0_props = fb->osd_props;
	if(d == 32) {
		fb->video0_props.line_length /= 2;
	}

	fb->alpha_props  = fb->osd_props;
	fb->alpha_props.line_length = lstep / 2;

	fb->osd_props.addr    = shmrgn + hdr->osd_offset[osd_actbuf];
	fb->alpha_props.addr  = shmrgn + hdr->alpha_offset;
	fb->video0_props.addr = shmrgn + hdr->video0_offset;

	init_fb_ops( fb );

	return 0;
}

PLATFORM_REGISTER_FB( QVFB_connect );

static void QVFB_update( int flags )
{
	if( !flags ) {
		// nothing to update, return
		return;
	}
		
	hdr->update.x1 = 0;
	hdr->update.y1 = 0;
	
	hdr->update.x2 = hdr->width - 1;
	hdr->update.y2 = hdr->height - 1;
	
	hdr->brightness = qvfb_brightness;

	hdr->osd_actbuf    = osd_actbuf;
	hdr->osd_active    = osd_active;
	hdr->osd_mix       = osd_mix;
	hdr->osd_blend     = osd_blend;
	hdr->alpha_active  = alpha_active;
	hdr->video0_active = video0_active;
	hdr->video1_active = 0;
	
	hdr->dirty = 1;
}

static void QVFB_set_osd_active( int active )
{
	osd_active = (active != 0);
	FB_update( FB_UPDATE_ALL );
}

static void QVFB_set_osd_mix( int mix )
{
	osd_mix = mix;
	FB_update( FB_UPDATE_ALL );
}

static void QVFB_set_osd_blend( int blend )
{
	osd_blend = blend;
	FB_update( FB_UPDATE_ALL );
}

static void QVFB_set_video_active( int active )
{
	video0_active = (active != 0);
	FB_update( FB_UPDATE_ALL );
}

static void QVFB_set_alpha_active( int active )
{
	alpha_active = (active != 0);
	FB_update( FB_UPDATE_ALL );
}

static void QVFB_set_display_enable( int active )
{
}

static int QVFB_set_display_mode( int tvmode, int output )
{
	display_mode = tvmode;
	return 0;
}

static int QVFB_set_osd_props( const OsdWindowProperties *props)
{
	fb.osd_props.x_p = props->x_p;
	fb.osd_props.y_p = props->y_p;
	fb.osd_props.x_l = props->x_l;
	fb.osd_props.y_l = props->y_l;
	
	return 0;
}

static int QVFB_set_video_props( const OsdWindowProperties *props)
{
	fb.video0_props.x_p = props->x_p;
	fb.video0_props.y_p = props->y_p;
	fb.video0_props.x_l = props->x_l;
	fb.video0_props.y_l = props->y_l;
	
	return 0;
}

static void QVFB_set_crop(int x, int y, int width, int height)
{
serprintf("QVFB_set_crop(%dx%d  %dx%d)\r\n", x, y, width, height);
}

static unsigned char* QVFB_get_osd_data(void)
{
	return fb.osd_props.addr;
}

static void init_fb_ops( FRAME_BUFFER *fb )
{
	fb->fb_ops.update		= QVFB_update;
	fb->fb_ops.set_osd_active	= QVFB_set_osd_active;
	fb->fb_ops.set_osd_blend	= QVFB_set_osd_blend;
	fb->fb_ops.set_osd_mix		= QVFB_set_osd_mix;
	fb->fb_ops.set_video_active	= QVFB_set_video_active;
	fb->fb_ops.set_alpha_active	= QVFB_set_alpha_active;
	fb->fb_ops.set_display_enable	= QVFB_set_display_enable;
	fb->fb_ops.set_osd_properties	= QVFB_set_osd_props;
	fb->fb_ops.set_video_properties = QVFB_set_video_props;
	fb->fb_ops.set_display_mode	= QVFB_set_display_mode;
	fb->fb_ops.set_crop	        = QVFB_set_crop;
	fb->fb_ops.get_osd_data		= QVFB_get_osd_data;
}

#endif
