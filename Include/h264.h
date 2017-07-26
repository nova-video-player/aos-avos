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

#ifndef H264_H
#define H264_H

#include "h264_sps.h"
#include "stream.h"
#include "cbe.h"

int H264_parse_slice_header( const H264_SPS *sps, const UCHAR *p, int *type, int *_tbf, int *_frame_num );
int H264_parse_SPS( H264_SPS *sps, const UCHAR *p, int len );
int H264_parse_PPS( H264_SPS *sps, const UCHAR *p, int len );

int  H264_parse_avcc( VIDEO_PROPERTIES *video, CBE *cbe, int *out_size, int *nal_unit_size );
int  H264_parse_NAL ( UCHAR *p, int size, CBE *cbe, int *out_size, int  nal_unit_size );
void H264_end_NAL( CBE *cbe, int *out_size );

int  H264_find_NAL( const UCHAR *p, int len, int *NAL );
int  H264_find_NAL2( const UCHAR *p, int len, int *NAL, int *nri, int *more );
int  H264_find_AUD( const UCHAR *p, int len );
int  H264_find_SLICE( const UCHAR *p, int len, int *sps );
int  H264_get_video_props( VIDEO_PROPERTIES *video, const UCHAR *p, int len, H264_SPS *sps );

#define NAL_SLICE                1
#define NAL_DPA                  2
#define NAL_DPB                  3
#define NAL_DPC                  4
#define NAL_IDR_SLICE            5
#define NAL_SEI                  6
#define NAL_SPS                  7
#define NAL_PPS                  8
#define NAL_AUD                  9
#define NAL_END_SEQUENCE        10
#define NAL_END_STREAM          11
#define NAL_FILLER_DATA         12
#define NAL_SPS_EXT             13
#define NAL_AUXILIARY_SLICE     19

#define EXTENDED_SAR          	255

enum {
	H264_PROFILE_BASELINE 	= 66,
	H264_PROFILE_MAIN 	= 77,
	H264_PROFILE_EXTENDED 	= 88,
	H264_PROFILE_HIGH 	= 100,
	H264_PROFILE_HIGH10 	= 110,
	H264_PROFILE_HIGH422 	= 122,
	H264_PROFILE_HIGH444 	= 144,
};	
	
enum {
	H264_TOP_FIELD = 0,  
	H264_BOT_FIELD,      
	H264_FRAME,         
};

#endif	// H264_H
