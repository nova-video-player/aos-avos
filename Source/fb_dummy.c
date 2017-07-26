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

#include "fb.h"
#include "fb_dummy.h"
#include "debug.h"
#include "platform.h"


static void init_fb_ops( FRAME_BUFFER *fb );

int DUMMY_connect(FRAME_BUFFER *fb)
{
	serprintf("Opening dummy framebuffer\n");

	fb->osd_props.x_l = 800;
	fb->osd_props.y_l = 480;
	fb->osd_props.depth = 32;
	fb->osd_props.x_offset = 0;
	fb->osd_props.y_offset = 0;
	fb->osd_props.x_p = 0;
	fb->osd_props.y_p = 0;
	fb->osd_props.line_length = 800 * 4;

	fb->video0_props = fb->osd_props;

	init_fb_ops(fb);

	return 0;
}

static void DUMMY_update()
{
}

static void DUMMY_set_osd_active(int active)
{
}

static void DUMMY_set_osd_blend(int blend)
{
}

static void DUMMY_set_osd_mix(int mix)
{
}

static void DUMMY_set_video_active(int active)
{
}

static void DUMMY_set_alpha_active(int active)
{
}

static int DUMMY_set_display_mode(int tvmode, int output)
{
	return 0;
}

static int DUMMY_set_osd_properties(const OsdWindowProperties *props)
{	
	return 0;
}

static int DUMMY_set_video_properties(const OsdWindowProperties *props)
{	
	return 0;
}

static void DUMMY_set_display_enable(int active)
{
}


static void init_fb_ops(FRAME_BUFFER *fb){
	fb->fb_ops.update = DUMMY_update;
	fb->fb_ops.set_osd_active = DUMMY_set_osd_active;
	fb->fb_ops.set_osd_blend = DUMMY_set_osd_blend;
	fb->fb_ops.set_osd_mix = DUMMY_set_osd_mix;
	fb->fb_ops.set_video_active = DUMMY_set_video_active;
	fb->fb_ops.set_alpha_active = DUMMY_set_alpha_active;
	fb->fb_ops.set_global_alpha = NULL;
	fb->fb_ops.set_display_mode = DUMMY_set_display_mode;
	fb->fb_ops.set_osd_properties = DUMMY_set_osd_properties;
	fb->fb_ops.set_video_properties = DUMMY_set_video_properties;
	fb->fb_ops.set_display_enable = DUMMY_set_display_enable;
}

