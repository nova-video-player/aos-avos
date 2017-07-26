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
#include "debug.h"
#include "av.h"
#include "stream_resizer.h"
#include "util.h"
#include "astdlib.h"

#include <string.h>

#define DBGS if(Debug[DBG_STREAM])
#define DBG  if(Debug[DBG_RESIZER])

#define MAX_DOWNSCALING_FACTOR	4
#define MAX_UPSCALING_FACTOR	8

#ifdef CONFIG_VIDEO

static int rsz_force_width     = 0;
static int rsz_force_height    = 0;
static UNUSED int rsz_force_one_field = 1;

DECLARE_DEBUG_TOGGLE("rfw", rsz_force_width );
DECLARE_DEBUG_TOGGLE("rfh", rsz_force_height );
DECLARE_DEBUG_TOGGLE("rff", rsz_force_one_field );

typedef struct
{
	int 	src_width; /* please use the src_video_width after aspect ratio n , aspect ratio d */
	int 	src_height;
	float 	src_dar;
	float 	src_par;
	
	int 	dst_width;
	int 	dst_height;
	float 	dst_dar;
	float 	dst_par;
	
	int 	format;
} PARAMS;

/*============================================================================*/
/**
 \brief   Round a float value
 \param   val float value
 \return  round int value
 */ 
/*============================================================================*/
static int vr_round(float val)
{
	int ret = (int)val;
	float tmp = (val - (float)ret);
	if(tmp >= 0.5f){
		ret +=1;
	}
	return(ret);
}

/*============================================================================*/
/**
 \brief   Crop the source content according to the display format setting
 \param   crop_src_width   src vid width cropped
 \param   crop_src_height  src vid height cropped
 \param   crop_src_dar     new src display aspect ratio

 */ 
/*============================================================================*/
static int get_src_cropped_size(STREAM_SCREEN_PARAMS *screen, PARAMS *param, int *crop_src_width, int *crop_src_height, float *crop_src_dar)
{
	int ret = 0;

	*crop_src_width  = param->src_width;
	*crop_src_height = param->src_height;
	*crop_src_dar    = param->src_dar;
DBG{
serprintf( "get_src_cropped_size:\n");
serprintf( "\tsrc:    %4d x %4d  par %f  dar %f\n", param->src_width, param->src_height, param->src_par, param->src_dar);
serprintf( "\tdst:    %4d x %4d  par %f  dar %f\n", param->dst_width, param->dst_height, param->dst_par, param->dst_dar);
}
	switch(param->format){
		case DISPFMT_FULL_SCREEN:
DBG serprintf( "\tFULL_SCREEN  ");
			if (param->dst_dar >= param->src_dar) {
DBG serprintf("\ndst_dar >= src_dar\n");
				*crop_src_width  = param->src_width;
				*crop_src_height = vr_round((float)param->src_width * (float)param->src_par / (float)param->dst_dar);
			} else {
DBG serprintf("\ndst_dar < src_dar\n");
				*crop_src_width  = vr_round((float)param->src_height / (float)param->src_par * (float)param->dst_dar);
				*crop_src_height = param->src_height;
			}
			break;
		default:
		case DISPFMT_ORIGINAL_PICTURE:
			// nothing to do
			break;
	}
	//verify round errors
	if (screen->zoom > 1.0f) {
		*crop_src_height = *crop_src_height / screen->zoom;
		*crop_src_width  = *crop_src_width / screen->zoom;
	}
	if(*crop_src_height > param->src_height ){
		*crop_src_height = param->src_height;
serprintf( "\tCropped H > src H fix\n");
	}
	if(*crop_src_width > param->src_width ){
		*crop_src_width = param->src_width;
serprintf( "\tCropped W > src W fix\n");
	}
	
DBG serprintf( "-->\tcrop:   %4d x %4d\n", *crop_src_width, *crop_src_height );
	return(ret);
}

/*============================================================================*/
/**
 \brief   Caculate the theoritical size of the resize video source
 \param   pResizerParams  theoritical resizer parameters struct
 \param   resized_width  width of the resized video source.
 \param   resized_height  height of the resized video source.

 */ 
/*============================================================================*/
static int get_resized_size(STREAM_SCREEN_PARAMS *screen, PARAMS *param, int *resized_width, int *resized_height)
{
	int ret = 0;
	float coef_zoom_w = 1.0f;
	float coef_zoom_h = 1.0f;

DBG {
serprintf( "get_resized_size:\n");
serprintf( "\tsrc:    %4d x %4d  par %f  dar %f\n", param->src_width, param->src_height, param->src_par, param->src_dar);
serprintf( "\tdst:    %4d x %4d  par %f  dar %f\n", param->dst_width, param->dst_height, param->dst_par, param->dst_dar);
}

	switch(param->format) {
		case DISPFMT_ORIGINAL_PICTURE:
		case DISPFMT_FULL_SCREEN:
		default:
DBG serprintf( "\tAUTO ADJUST\n");
			// test source and destination display aspect ratio to know what to do
			if(param->src_dar < param->dst_dar) {
DBG serprintf( "\tsrc_dar < dst_dar\n");
				// in that case the picture limit will be the height
				coef_zoom_h     = (float)param->dst_height / (float) param->src_height;
				coef_zoom_w     = coef_zoom_h / (float)param->dst_par;
				*resized_width  = vr_round((float)param->src_width * param->src_par * coef_zoom_w );
				*resized_height = (float)param->dst_height;
			} else {
DBG serprintf( "\tsrc_dar >= dst_dar\n");
				// picture limit will be the width
				coef_zoom_w     = (float)param->dst_width / ((float) param->src_width * param->src_par);
				coef_zoom_h     = coef_zoom_w * param->dst_par;
				*resized_width  = param->dst_width;
				*resized_height = vr_round((float)param->src_height * coef_zoom_h);
			}
			break;
	}
	if (screen->zoom < 1.0f) {
		*resized_width  = *resized_width  * screen->zoom;
		*resized_height = *resized_height * screen->zoom;
	}
DBG serprintf( "-->\trsz:    %4d x %4d  (coef w=%f h=%f)\n", *resized_width, *resized_height,
								coef_zoom_w, coef_zoom_h  );
	return ret;
}

// *****************************************************************************
//
//	compute_resizer
//
//	This function get the native video format and ouputs
//	the target ouput width and high to be used with x,y offsets
//	parameters to be passed to resizer engine, ONLY for HDMI formats !
//
//	Note that tis function should also work for LCDs ...
// *****************************************************************************
static void compute_resizer( STREAM_SCREEN_PARAMS *screen, VIDEO_PROPERTIES *video, STREAM_RSZ_PARAMS *rsz )
{
	// src video info 
	int src_width    = rsz_force_width  ? rsz_force_width  : video->width;
	int src_height   = rsz_force_height ? rsz_force_height : video->height;
	int src_aspect_n = video->aspect_n;
	int src_aspect_d = video->aspect_d;
	
	// screen parameters 
	int screen_x;
	int screen_y;
	int screen_w; 	
	int screen_h; 	
	int screen_h_ofs = 0;
	int screen_n; 	
	int screen_d; 	

	int format = screen->format;
	
DBG serprintf( "\ncompute_resizer:\n" );
	if( rsz_force_one_field && video->interlaced == VIDEO_INTERLACED_ONE_FIELD ) {
		// one field only, half the height and force the aspect ratio
//		src_height   /= 2;
		src_aspect_d *= 2;
	}
	if (src_width == 1024 || src_width == 1025) {
		// for some reason DSS does not like to downscale 1024/1025
		// so force 1023 in this case..
serprintf("\tRSZ: forcing width to 1023!\n");
		src_width = 1023;
	}


	switch( screen->rotation ) {
	case 0:
	case 180:
		screen_x = screen->screen.x;
		screen_y = screen->screen.y;
		screen_w = screen->screen.width; 	
		screen_h = screen->screen.height; 	
		screen_n = screen->aspect_n; 	
		screen_d = screen->aspect_d; 	
		break;
	case 90:
	case 270:
	default:
		screen_x = screen->screen.y;
		screen_y = screen->screen.x;
		screen_w = screen->screen.height; 	
		screen_h = screen->screen.width; 	
		screen_n = screen->aspect_d; 	
		screen_d = screen->aspect_n; 	
		break;
	}

	// resizer parameters
	PARAMS param = { 0 };
	
	rsz->src = ( RECT ) { 0, 0, src_width, src_height };
	rsz->dst = ( RECT ) { 0, 0, src_width, src_height };

	// make sure aspect makes sense
	if ( !src_aspect_n || !src_aspect_d ) {
		src_aspect_n = 1;
		src_aspect_d = 1;
	}	
	
	// init resizer param 
	param.src_width  = src_width;
	param.src_height = src_height;
	param.src_par    = (float)((float)src_aspect_n / (float)src_aspect_d);
	param.src_dar    = (float)((float)src_width * param.src_par ) / (float)src_height;

	param.dst_width  = screen_w;
	param.dst_height = screen_h; 
	param.dst_par    = (float)((float)screen_n / (float)screen_d);
	param.dst_dar    = (float)((float)screen_w * param.dst_par ) / (float)screen_h;
	param.format     = format;
	
DBG serprintf( "\tscreen: %4d x %4d, aspect %3d/%3d  offset %4d x %4d\r\n", screen_w, screen_h, screen_n, screen_d, screen_x, screen_y);
DBG serprintf( "\tvideo:  %4d x %4d, aspect %3d/%3d\r\n", src_width, src_height, src_aspect_n, src_aspect_d );

	// crop the src video if needed 
	int cropped_src_width;
	int cropped_src_height;
	float cropped_src_dar = 1.0f;
	get_src_cropped_size(screen, &param, &cropped_src_width, &cropped_src_height, &cropped_src_dar);
	
	// change src video info according to the previous cropping
	param.src_width  = cropped_src_width;
	param.src_height = cropped_src_height;
	param.src_dar    = cropped_src_dar;
			
	// resized video 
	int dst_width;
	int dst_height;
	
	get_resized_size(screen, &param, &dst_width, &dst_height);
	
	// whatever was computed above, make sure that we do
	// not overwrite the out buffer
	dst_width  = MIN( screen_w, dst_width  );
 	dst_height = MIN( screen_h, dst_height );

	dst_width  = dst_width  - (dst_width  % 2);
	dst_height = dst_height - (dst_height % 2);

	/*
	 * we can max downscale by 4!
	 * we can max upscale by 8!
	 */
	if( cropped_src_height > MAX_DOWNSCALING_FACTOR * dst_height ) {
serprintf("RESIZER: too much VERTICAL downscale %d > %d * %d\n", cropped_src_height, MAX_DOWNSCALING_FACTOR, dst_height);
		int temp_h = MAX(cropped_src_height, 1);
		cropped_src_height = MAX_DOWNSCALING_FACTOR * dst_height;
		cropped_src_width = (cropped_src_width * cropped_src_height) / temp_h;
	} else if ( dst_height > MAX_UPSCALING_FACTOR * cropped_src_height ) {
serprintf("RESIZER: too much VERTICAL upscale %d > %d * %d\n", dst_height, MAX_UPSCALING_FACTOR, cropped_src_height);
		int temp_h = MAX(dst_height, 1);
		dst_height = MAX_UPSCALING_FACTOR * cropped_src_height;
		dst_width = (dst_width * dst_height) / temp_h;
	}
	if( cropped_src_width > MAX_DOWNSCALING_FACTOR * dst_width ) {
serprintf("RESIZER: too much HORIZONTAL downscale %d > %d * %d\n", cropped_src_width, MAX_DOWNSCALING_FACTOR, dst_width);
		int temp_w = MAX(cropped_src_width, 1);
		cropped_src_width = MAX_DOWNSCALING_FACTOR * dst_width;
		cropped_src_height = (cropped_src_height * cropped_src_width) / temp_w;
	} else if ( dst_width > MAX_UPSCALING_FACTOR * cropped_src_width ) {
serprintf("RESIZER: too much HORIZONTAL upscale %d > %d * %d\n", dst_width, MAX_UPSCALING_FACTOR, cropped_src_width);
		int temp_w = MAX(dst_width, 1);
		dst_width = MAX_UPSCALING_FACTOR * cropped_src_width;
		dst_height = (dst_height * dst_width) / temp_w;
	}

	// center the clipped part of the source image
	int src_offset_x = ( ( src_width  - cropped_src_width  ) / 2 ) & ~0x01;	// in pixel
	int src_offset_y = ( ( src_height - cropped_src_height ) / 2 ) & ~0x01;

	// center the clipped part of the dest image
	int dst_offset_y = MAX( 0, ( ( (screen_h - dst_height ) / 2 ) + screen_y + screen_h_ofs ) );
	int dst_offset_x = MAX( 0, ( ( (screen_w - dst_width  ) / 2 ) + screen_x ) );

	// Store back value computed.
	rsz->src      = (RECT) { src_offset_x, src_offset_y, cropped_src_width, cropped_src_height };
	rsz->dst      = (RECT) { dst_offset_x, dst_offset_y, dst_width,         dst_height         };
	rsz->rotation = screen->rotation;

	DBG {
		serprintf( "final:\n" );
		serprintf( "\tsrc:    %4d x %4d\n",                 src_width,         src_height );
		serprintf( "\tcrop:   %4d x %4d  ofs: %4d x %4d\n", cropped_src_width, cropped_src_height, src_offset_x, src_offset_y );
		serprintf( "\tdst:    %4d x %4d  ofs: %4d x %4d\n", dst_width,         dst_height,         dst_offset_x, dst_offset_y );
		serprintf( "\n" );
	}
}


static int _setup( STREAM_RESIZER *r, STREAM_SCREEN_PARAMS *screen, VIDEO_PROPERTIES *video, STREAM_RSZ_PARAMS *rsz )
{
//	priv->can_change_format = 1;
DBG serprintf( "rszDSS: setup\r\n");
	compute_resizer( screen, video, rsz );

DBG serprintf( "rotation: %d\r\n", rsz->rotation );
DBG Rect_Dump( &rsz->src, "src" );
DBG Rect_Dump( &rsz->dst, "dst" );
			
	return 0;
}

// resizer using the DSS resizer
STREAM_RESIZER video_resizer_DSS = {
	"DSS resizer",
	_setup,
};


#endif	// ifdef CONFIG_VIDEO
