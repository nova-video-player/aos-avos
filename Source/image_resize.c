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
#include "file.h"
#include "astdlib.h"
#include "image.h"
#include "av.h"
#include "util.h"
#include "atime.h"

#define PIXEL_WIDTH_OFF		2
#define PIXEL_HEIGHT_OFF	2

#define DBG  if(Debug[DBG_IMG])
#define DBG2 if(Debug[DBG_IMG] > 1)
#define ERR  if(1)

static int image_use_resizer = 1;

#define IMAGE_RESIZE image_resize_arm

// ****************************************************
//
//	image_resize_accel
//
// 	hardware accelerated stretch-blit
//      Not a static, needed in image_rgb16.c
// ****************************************************
int _image_resize_accel( const IMAGE *src, IMAGE *dst, unsigned int *cmd_tag, int vblsync, int mode_hor, int mode_ver, float coef )
{
	return 1;
}

// ****************************************************
//
//	_mode_to_string
//
// ****************************************************
static unsigned char * _mode_to_string( int mode )
{
	switch(mode){
		case RSZ_NORMAL:
			return "norm";
		case RSZ_BLUR:
			return "blur";
		case RSZ_NEAREST_NEIGHBOUR:
			return "near";
		default:
			return "?";
	}
}

// ****************************************************
//
//	image_resize_arm
//
// ****************************************************
static void image_resize_arm( const IMAGE *src, IMAGE *dst, int sync, int vblsync, int mode, float coef )
{
	unsigned int tag;
	int start = atime();
	int err;
	int mode_hor, mode_ver;

	if( !image_use_resizer ){
		image_software_resize( src, dst );
		return;
	}

	if ( mode == RSZ_HORIZONTAL_BLUR ) {
		mode_hor = RSZ_BLUR;
		mode_ver = RSZ_NORMAL;
	}
	else {
		mode_hor = mode;
		mode_ver = mode;
	}
	err = _image_resize_accel( src, dst, &tag, vblsync, mode_hor, mode_ver, coef );
	if( err ){
		// we could not do it by hardware, do it in software
		image_software_resize( src, dst );
		return;
	}

DBG2 serprintf("resize_arm %4d|%4d x %4d (%3d|%3d) --> %4d|%4d x %4d (%3d|%3d)  time %8d sync %d vbl %d mode %s tag %d\r\n",
		src->window.width, src->linestep[0], src->window.height, src->window.x, src->window.y,
		dst->window.width, dst->linestep[0], dst->window.height, dst->window.x, dst->window.y,
		atime() - start, sync == RSZ_SYNC, vblsync == RSZ_VBL,
		_mode_to_string(mode), tag);
	
	if( sync == RSZ_SYNC ) {
		//_image_resize_wait(tag);	
	}
}

// ****************************************************
//
//	_check_params
//
// ****************************************************
static int image_check_params(const IMAGE *image)
{
	if( image->window.x < 0 || image->window.y < 0 ) {
serprintf("image_check_params: window.x %d  window.y %d\r\n", image->window.x, image->window.y );
		return 1;
	}
	
	if( image->window.width < 0 || image->window.height < 0 ) {
serprintf("image_check_params: window.width %d  window.height %d\r\n", image->window.width, image->window.height );
		return 1;
	}
	
/* hmm, for some reason 800 wide images have a width of 720 only :-(
	if( image->window.x + image->window.width > image->width ) {
serprintf("x %d %d %d\r\n", image->window.x, image->window.width, image->width );
		return 1;
	}
	if( image->window.y + image->window.height > image->height ) {
serprintf("x %d %d %d\r\n", image->window.y, image->window.height, image->height );
		return 1;
	}
*/	
	return 0;
}

// ****************************************************
//
//	_to_yuv
//
// ****************************************************
static inline UINT16 _to_yuv( UINT16 *in_ptr, int in_x, int out_x )
{
	if( out_x % 2 ){
		if( in_x % 2 )
			return *in_ptr;
		else
			return (( *in_ptr & 0xff00 ) | (*(in_ptr + 1 ) & 0xff ));
	} else {
		if( in_x % 2 )
			return (( *in_ptr & 0xff00 ) | (*(in_ptr + 1 ) & 0xff ));
		else
			return *in_ptr;
	}
}

// ****************************************************
//
//	_to_rgb16
//
// ****************************************************
static inline UINT16 _to_rgb16( UINT16 *in_ptr, int in_x, int out_x )
{
	UCHAR y, u, v;

	y = (( *in_ptr & 0xff00 ) >> 8 );

	if( in_x % 2 ){
		v = ( *in_ptr       & 0xff );
		u = (*(in_ptr + 1 ) & 0xff );
	} else {
		u = ( *in_ptr       & 0xff );
		v = (*(in_ptr + 1 ) & 0xff );
	}

	return yuv_to_rgb16( y, u, v );
}

// ****************************************************
//
//	image_software_resize
//
// ****************************************************
void image_software_resize( const IMAGE *src_img, IMAGE *dst_img )
{
	unsigned long b_time = 0;
	if(Debug[DBG_IMG] > 1) {
		b_time = atime();
	}

	int out_x, out_y;
	int in_x, in_y;
	int fine_in_x, fine_in_y;
	
	int in_w = src_img->window.width;
	int in_h = src_img->window.height;
	int out_w = dst_img->window.width;
	int out_h = dst_img->window.height;

	if(image_check_params(src_img)){
ERR serprintf("image_software_resize: bad source\n");
		return;
	}

	if(image_check_params(dst_img)){
ERR serprintf("image_software_resize: bad destination\n");
		return;
	}

	if( out_h == 0 || out_w == 0 )
		return;

DBG2 serprintf( "IMG: %s %dx%d|%d --> %dx%d|%d \r\n", __FUNCTION__, in_w , in_h, src_img->colorspace, out_w, out_h, dst_img->colorspace );

	int h_rsz = ( in_w << 10 ) / out_w;
	int v_rsz = ( in_h << 10 ) / out_h;
	fine_in_y = 0;
	for( out_y = 0 ; out_y < out_h ; out_y++ ) {
		fine_in_x = 0;
		USHORT* out_ptr = (USHORT*)( PIXELPTR( dst_img, dst_img->window.x, dst_img->window.y + out_y ));

		for (out_x = 0 ; out_x < out_w ; out_x++ ) { 
			
			in_x = fine_in_x >> 10;
			in_y = fine_in_y >> 10;

			if( src_img->colorspace == AV_IMAGE_NV12 ) {
				UCHAR*  in_ptrY  = (UCHAR*) ( PIXELPTR_N( src_img, 0, src_img->window.x + in_x, src_img->window.y + in_y ));
				USHORT* in_ptrUV = (USHORT*)( PIXELPTR_N( src_img, 1, src_img->window.x + in_x / 2 * 2, (src_img->window.y + in_y) / 2 ));
				if( dst_img->colorspace == AV_IMAGE_YUV_422 ){
					if( out_x % 2 ){
						*out_ptr++ = *in_ptrY << 8 | (*in_ptrUV >> 8);
					} else {
						*out_ptr++ = *in_ptrY << 8 | (*in_ptrUV & 0xFF);
					}
				}
			} else {
				USHORT* in_ptr = (USHORT*)( PIXELPTR( src_img, src_img->window.x + in_x, src_img->window.y + in_y ));

				if( src_img->colorspace == AV_IMAGE_RGB_16 && dst_img->colorspace == AV_IMAGE_RGB_16 ){
					*out_ptr++ = *in_ptr;
				} else if( dst_img->colorspace == AV_IMAGE_YUV_422 ){
					*out_ptr++ = _to_yuv( in_ptr, in_x, out_x );
				} else if( src_img->colorspace == AV_IMAGE_BGRA_32 && dst_img->colorspace == AV_IMAGE_BGRA_32 ){
					*((UINT32*)out_ptr) = *((UINT32*)in_ptr);
					out_ptr += 2;
				} else if( src_img->colorspace == AV_IMAGE_RGBA_32 && dst_img->colorspace == AV_IMAGE_RGBA_32 ){
					*((UINT32*)out_ptr) = *((UINT32*)in_ptr);
					out_ptr += 2;
				} else {
					*out_ptr++ = _to_rgb16( in_ptr, in_x, out_x );
				}
			}

			fine_in_x += h_rsz;
		}

		fine_in_y += v_rsz;
	}

DBG2 serprintf("IMG: %s time %d\r\n", __FUNCTION__, atime() - b_time );
}

static int
get_component(int pixel, int c)
{
	return ( pixel>>(c*8) & 0x000000FF ) ;
} 

// ****************************************************
//
//	image_resize
//
// ****************************************************
void image_resize( const IMAGE *src_img, IMAGE *dst_img )
{
	unsigned long b_time = atime();

	if( !src_img->bpp[0] || !dst_img->bpp[0] ){
serprintf("image_resize: bpp %d != %d\n", src_img->bpp[0], dst_img->bpp[0] );
	}
 
	IMAGE_RESIZE( src_img, dst_img, RSZ_SYNC, RSZ_ASAP, RSZ_NORMAL, 1 );

DBG2 serprintf("IMG: resize time : %d\r\n", atime() - b_time );
}

#ifdef DEBUG_MSG
DECLARE_DEBUG_TOGGLE("irsz", image_use_resizer);

static void _test_resizer( int argc, char *argv[] )
{
	IMAGE *src = image_alloc( 320, 240, AV_IMAGE_YUV_422 );
	IMAGE *dst = image_alloc( 640, 480, AV_IMAGE_YUV_422 );
	
	src->window = ( RECT ) { 0, 0, 320, 240 };
	dst->window = ( RECT ) { 0, 0, 640, 480 };
	
	int count = argc > 1 ? atoi( argv[1] ) : 1;
	int i;	
	for( i = 0; i < count; i ++ ) {
		image_resize( src, dst );
	}	
	image_free( src );
	image_free( dst );
}

#ifdef CONFIG_RESIZE_ACCEL
static void _close_resizer( void )
{
	if( resz_fd != -1 ) {
		close( resz_fd );
		resz_fd = -1;
	}
}

DECLARE_DEBUG_COMMAND_VOID("crsz", _close_resizer );
#endif
DECLARE_DEBUG_COMMAND("trsz", _test_resizer );


#endif
