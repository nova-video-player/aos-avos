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
#include "stream.h"
#include "cbe.h"
#include "h264.h"
#include "bits.h"
#include "util.h"

#include <string.h>
#include <stdio.h>

#ifdef CONFIG_H264

#ifdef STANDALONE
#define DBG if(0)
#else
#define DBG    if(Debug[DBG_STREAM] > 1)
#define DBGP4  if(Debug[DBG_PARSER] > 3)
#define DBGMNG if(Debug[DBG_MANGLER])
#endif

static const UCHAR sync_word[4] = { 0x00, 0x00, 0x00, 0x01 };

void H264_end_NAL( CBE *cbe, int *out_size ) {}

static void H264_get_profile_name( int profile_idc, char *name, int max )
{
	char *n = "Unknown";
	
	switch( profile_idc ) {
	case H264_PROFILE_BASELINE:	n = "Baseline"; 	break;
	case H264_PROFILE_MAIN:		n = "Main"; 		break;
	case H264_PROFILE_EXTENDED:	n = "Extended"; 	break;
	case H264_PROFILE_HIGH:		n = "High"; 		break;
	case H264_PROFILE_HIGH10:	n = "High10"; 		break;
	case H264_PROFILE_HIGH422:	n = "High 4:2:2"; 	break;
	case H264_PROFILE_HIGH444:	n = "High 4:4:4"; 	break;
	}
	
	strnZcpy( name, n, max );
}

static void H264_get_level_name( int level_idc, char *name, int max )
{
	snprintf( name, max, "%1.1f", ((float)level_idc)/10 );
}

static int _parse_avcc( VIDEO_PROPERTIES *video, UCHAR *p, int rest, CBE *cbe, int *out_size, int *nal_unit_size )
{
	int version    = p[0];
	video->profile = p[1];
	int compat     = p[2];
	video->level   = p[3];

	H264_get_profile_name( video->profile, video->profile_name, AV_NAME_LEN );
	H264_get_level_name(   video->level,   video->level_name,   AV_NAME_LEN );

DBG serprintf("\r\nH264_parse_avcc, size %d\r\n\r\n", rest);	
DBG serprintf("version       : %03d \r\n", version);
DBG serprintf("profile       : %03d [%s]\r\n", video->profile, video->profile_name);
DBG serprintf("compat        : %03X \r\n", compat);
DBG serprintf("avc_level     : %03d [%s]\r\n", video->level, video->level_name);

	if( video->level == 1 ) {
DBG serprintf("force avc = 10!\r\n");
		p[3] = 10;
	}
	p    += 4;
	rest -= 4;
	
	if( version != 1 )
		return 1;
	
	*nal_unit_size = 1 + (*p++ & 0x03);
	rest --;
DBG serprintf("nal_unit_size : %d \r\n", *nal_unit_size);

	int count = *p++ & 0x1F;
	rest --;
DBG serprintf("sps count     : %d  \r\n", count);
	int i;
	for( i = 0; i < count; i++ ) {
		int j;
		int size = *p++ << 8;
		size    += *p++;
		rest    -= 2;
DBG serprintf("\tsize %4d  ", size );
		if( !size || size > rest ) {
serprintf("AVCC SPS error!\r\n" );
			return 1;
		}
int avc_level = p[3];
if( avc_level == 1 ) {
DBG serprintf("F! ");
		p[3] = 10;
}
		if( i == 0) {
			H264_parse_SPS( &video->sps, p+1, size-1 );
		}
		for( j = 0; j < MIN(size, 32); j++ ) {
DBG serprintf("%02X ", p[j] );		
		}
DBG serprintf("\r\n");		
		if( cbe ) {
			cbe_write( cbe, sync_word, 4 );
			cbe_write( cbe, p, size );
			if( out_size ) {
				*out_size += 4 + size;
			}
		}
		p    += size;
		rest -= size;
	}	
DBG serprintf("\r\n");	

	count = *p++ & 0x1F;
	rest --;
DBG serprintf("pps count     : %d  \r\n", count);

	for( i = 0; i < count; i++ ) {
		int j;
		int size = *p++ << 8;
		size    += *p++;
		rest    -= 2;
DBG serprintf("\tsize %4d  ", size );
		if( !size || size > rest ) {
serprintf("AVCC PPS error!\r\n" );
			return 1;
		}
		if( i == 0) {
			H264_parse_PPS( &video->sps, p+1, size-1 );
		}
		for( j = 0; j < MIN(size, 32); j++ ) {
DBG serprintf("%02X ", p[j] );		
		}
DBG serprintf("\r\n");		
		if( cbe ) {
			cbe_write( cbe, sync_word, 4 );
			cbe_write( cbe, p, size );
			if( out_size ) {
				*out_size += 4 + size;
			}
		}
		p    += size;
		rest -= size;
	}	
DBG serprintf("\r\n");	
	if( video->sps.sar_num && video->sps.sar_den ) {
		video->aspect_n = video->sps.sar_num;
		video->aspect_d = video->sps.sar_den;
	}
	
	return 0;
}

int H264_parse_avcc( VIDEO_PROPERTIES *video, CBE *cbe, int *out_size, int *nal_unit_size )
{
	UCHAR *p;
	int size;
	if( video->extraDataSize ) {
		p    = video->extraData;
		size = video->extraDataSize;
	} else if( video->extraData2 && video->extraDataSize2 ) {
		p    = video->extraData2;
		size = video->extraDataSize2;
	} else 
		return 1;

	if(_parse_avcc( video, p, size, cbe, out_size, nal_unit_size )) {
		return 1;
	}
	
	return 0;
}

int H264_parse_NAL( UCHAR *d, int size, CBE *cbe, int *out_size, int nal_unit_size )
{
DBGP4 serprintf("H264_parse_NAL: %d\r\n", size);	
	int need_end = 0;
	while( size > 0 ) {
		int nal_size = *d++;
		int i;
		for( i = 1; i < nal_unit_size; i ++ ) {
			nal_size = (nal_size << 8) | *d++;
		}
		size -= nal_unit_size;
		// be robust!
		nal_size = MAX( 0, MIN( nal_size, size ));
DBGP4 serprintf("\tsize %5d  nal_size %d\r\n", size, nal_size );
		if( nal_size > 0 ) {	
			cbe_write( cbe, sync_word, 4 );
			cbe_write( cbe, d, nal_size );
			*out_size += 4 + nal_size;
			need_end = 1;

			d += nal_size;
		}
		size -= nal_size;
	} 
	if( need_end ) {
		H264_end_NAL( cbe, out_size );
	}
	return 0;
}

static UINT32 get_ue_golomb( BITS *bits )
{
	UINT exp = 1;
	int leadingZeroBits = -1;
	int b;
	for( b = 0; !b; leadingZeroBits++, exp *= 2 ) {
		b = BITS_get1( bits );
	}
	exp /= 2;
		
	UINT32 num = exp - 1 + BITS_get( bits, leadingZeroBits );
//serprintf("lzb %d  exp %d  num %d\r\n", leadingZeroBits, exp, num );
	return num;
}

static int get_se_golomb( BITS * bits )
{
	int value = 0;
	int leadingZeroBits = 0;

	unsigned int temp = BITS_get1( bits );
	while ( !temp ) {
		leadingZeroBits++;
		temp = BITS_get1( bits );
	}
	temp = BITS_get( bits, leadingZeroBits );
	value = ( ( 1 << leadingZeroBits ) + temp ) >> 1;
	return value;
}

int H264_parse_AUD( UCHAR *p )
{
DBG serprintf("H264_parse_AUD\r\n");
	return 1;	
}

int H264_parse_SEI( UCHAR *p )
{
DBG serprintf("H264_parse_SEI\r\n");
	return 0;	
}

static const int pixel_aspect[][2] = {
	{0,    1},
	{1,    1},
	{12,  11},
	{10,  11},
	{16,  11},
	{40,  33},
	{24,  11},
	{20,  11},
	{32,  11},
	{80,  33},
	{18,  11},
	{15,  11},
	{64,  33},
	{160, 99},
	{  4,  3},
	{  3,  2},
	{  2,  1},
};

static int decode_HRD( BITS *bits )
{
	int cpb_cnt_minus1 = get_ue_golomb( bits );
	BITS_get( bits, 4); //  bit_rate_scale 
	BITS_get( bits, 4); //  cpb_size_scale 
	
	int i;
	for (i = 0; i <= cpb_cnt_minus1; i++) {
		get_ue_golomb(bits); // bit_rate_value_minus1[i]
		get_ue_golomb(bits); // cpb_size_value_minus1[i]
		get_ue_golomb(bits); // cbr_flag[i]
	}

	BITS_get( bits, 5); // initial_cpb_removal_delay_length_minus1
	BITS_get( bits, 5); // cpb_removal_delay_length_minus1
	BITS_get( bits, 5); // dpb_output_delay_length_minus1
     
	BITS_get( bits, 5); // time_offset_length

	return 0; // 0 for success
}

static int parse_VUI( H264_SPS *sps, BITS *bits )
{
DBG serprintf("\nH264: parse_VUI\r\n");
	int aspect_ratio_info_present_flag = BITS_get1( bits );
	
	if ( aspect_ratio_info_present_flag ) {
//serprintf("aspect_ratio_info_present_flag\r\n");
		int aspect_ratio_idc = BITS_get( bits, 8 );
		if ( aspect_ratio_idc == EXTENDED_SAR ) {
			sps->sar_num = BITS_get( bits, 16 );
			sps->sar_den = BITS_get( bits, 16 );
		} else if ( aspect_ratio_idc < 17 ) {
			sps->sar_num = pixel_aspect[aspect_ratio_idc][0];
			sps->sar_den = pixel_aspect[aspect_ratio_idc][1];
		}
	} 
DBG serprintf("\taspect             %d:%d\r\n", sps->sar_num, sps->sar_den );

	if ( BITS_get1( bits) ) {	/* overscan_info_present_flag */
//serprintf("overscan_info_present_flag\r\n");
		BITS_get1( bits);	/* overscan_appropriate_flag */
	}

	if ( BITS_get1( bits) ) {	/* video_signal_type_present_flag */
		char *formats[] = {
			"Comp",
			"PAL",
			"NTSC",
			"SECAM",
			"MAC",
			"Unspec.",
			"Reserved",
			"Reserved",
		};
//serprintf("video_signal_type_present_flag\r\n");
		int vformat = BITS_get ( bits, 3 );	/* video_format */
DBG serprintf("\tvideo_format:      %s\r\n", formats[vformat]);  
		BITS_get1( bits);	/* video_full_range_flag */
		if ( BITS_get1( bits) ) {	/* colour_description_present_flag */
//serprintf("colour_description_present_flag\r\n");
			BITS_get( bits, 8 );	/* colour_primaries */
			BITS_get( bits, 8 );	/* transfer_characteristics */
			BITS_get( bits, 8 );	/* matrix_coefficients */
		}
	}

	if ( BITS_get1( bits) ) {	/* chroma_location_info_present_flag */
//serprintf("chroma_location_info_present_flag\r\n");
		get_ue_golomb( bits );	/* chroma_sample_location_type_top_field */
		get_ue_golomb( bits );	/* chroma_sample_location_type_bottom_field */
	}

	sps->timing_info_present = BITS_get1( bits);
	if ( sps->timing_info_present ) {
		sps->num_units_in_tick = BITS_get ( bits, 32 );
		sps->time_scale        = BITS_get ( bits, 32 );
		sps->fixed_frame_rate  = BITS_get1( bits );
DBG serprintf("\tnum_units_in_tick  %d \r\n", sps->num_units_in_tick );
DBG serprintf("\ttime_scale         %d \r\n", sps->time_scale );
DBG serprintf("\tfixed_frame_rate   %d \r\n", sps->fixed_frame_rate );
	}

     	int nal_hrd = BITS_get1( bits);	// nal_hrd_parameters_present_flag
	if( nal_hrd ) {
		decode_HRD(bits);
	}
 
 	int vcl_hrd = BITS_get1( bits); // vcl_hrd_parameters_present_flag
	if( vcl_hrd ) {
		decode_HRD(bits);
	}

	if( nal_hrd || vcl_hrd ) {
		BITS_get1( bits); // low_delay_hrd_flag
	}

	BITS_get1( bits); // pic_struct_present_flag
	
	int restriction_flag =  BITS_get1( bits ); // restriction_flag
	if (restriction_flag) {
		BITS_get1(bits); 	// motion_vectors_over_pic_boundaries_flag
		get_ue_golomb( bits ); 	// max_bytes_per_pic_denom
		get_ue_golomb( bits ); 	// max_bits_per_mb_denom
		get_ue_golomb( bits ); 	// log2_max_mv_length_horizontal
		get_ue_golomb( bits ); 	// log2_max_mv_length_vertical
		sps->num_reorder_frames     = get_ue_golomb(bits); 
		int max_dec_frame_buffering = get_ue_golomb(bits);
		
DBG serprintf("\tnum_reorder_frames %d \r\n", sps->num_reorder_frames );
DBG serprintf("\tmax_dec_frame_bufr %d \r\n", max_dec_frame_buffering );
	}

	return 0;
}

static void skip_scaling_list( int list_size, BITS *bits )
{
	int delta_scale = 0, lastScale = 8, nextScale = 8;
	int i;
	for ( i = 0; i < list_size; i++ ) {
		if ( nextScale ) {
			delta_scale = get_se_golomb( bits );
			nextScale = ( ( lastScale + delta_scale + 256 ) & 0xff );
		}
		lastScale = nextScale ? nextScale : lastScale;
	}
}

static void skip_scaling_matrices( BITS *bits )
{
	if ( BITS_get1( bits ) ) {
		int i;
		for ( i = 0; i < 8; i++ ) {
			if( BITS_get1( bits ) ) {	// scaling_list_present
				if ( i < 6 ) {
					skip_scaling_list( 16, bits );
				} else {
					skip_scaling_list( 64, bits );
				}
			}
		}
	}
}

int H264_parse_SPS( H264_SPS *sps, const UCHAR *p, int len )
{
DBG serprintf("\nH264: parse_SPS, len %d\r\n", len);
	if (len <= 0 )
		return 1;

	BITS _bits;
	BITS *bits = &_bits;

	BITS_init_h264( bits, (UCHAR*)p, len * 8 );
		
	sps->num_reorder_frames = -1; 

	sps->profile_idc = BITS_get( bits, 8 );
	BITS_get1( bits );   	// constraint_set0_flag
	BITS_get1( bits );   	// constraint_set1_flag
	BITS_get1( bits );   	// constraint_set2_flag
	BITS_get1( bits );   	// constraint_set3_flag
	BITS_get( bits, 4 ); 	// reserved
	sps->level_idc = BITS_get( bits, 8 );
	
	int sps_id = get_ue_golomb( bits );

	if( sps->profile_idc >= 100){ 		// high profile
		if(get_ue_golomb( bits ) == 3) 	// chroma_format_idc
			BITS_get1( bits );  	// residual_color_transform_flag
		get_ue_golomb( bits );  	// bit_depth_luma_minus8
		get_ue_golomb( bits );  	// bit_depth_chroma_minus8
		int UNUSED transform_bypass = BITS_get1( bits ); // qpprime_y_zero_transform_bypass_flag
	
		skip_scaling_matrices( bits );	// skip decode scaling matrices here
	} 
	sps->log2_max_frame_num = get_ue_golomb( bits ) + 4;
	int poc_type            = get_ue_golomb( bits );

	if( poc_type == 0 ){ 
		int UNUSED log2_max_poc_lsb = get_ue_golomb( bits ) + 4;
	} else if( poc_type == 1 ){ 
		int UNUSED delta_pic_order_always_zero_flag = BITS_get1( bits );
		int UNUSED offset_for_non_ref_pic           = get_se_golomb( bits );
		int UNUSED offset_for_top_to_bottom_field   = get_se_golomb( bits );
		int UNUSED poc_cycle_length                 = get_ue_golomb( bits );
		int i;
		for(i = 0; i < poc_cycle_length; i++) {
	    		int UNUSED offset_for_ref_frame = get_se_golomb( bits );
		}
	}

	sps->num_ref_frames = get_ue_golomb( bits );

	int UNUSED gaps_in_frame_num_allowed_flag = BITS_get1( bits );
	sps->width  = 16 * (get_ue_golomb( bits ) + 1);
	sps->height = 16 * (get_ue_golomb( bits ) + 1);

	sps->frame_mbs_only_flag = BITS_get1( bits );
	if ( !sps->frame_mbs_only_flag ) {
		sps->mb_aff = BITS_get1( bits );
	}
	sps->height *= (2 - sps->frame_mbs_only_flag);
	
	int UNUSED direct_8x8_inference_flag = BITS_get1( bits );

	int crop = BITS_get1( bits );
	if ( crop ) {
		int UNUSED crop_left   = get_ue_golomb( bits );
		int UNUSED crop_right  = get_ue_golomb( bits );
		int UNUSED crop_top    = get_ue_golomb( bits );
		int UNUSED crop_bottom = get_ue_golomb( bits );
	} 

	sps->sar_num = 1;
	sps->sar_den = 1;

DBG serprintf("\tprofile_idc        %d \r\n", sps->profile_idc );
DBG serprintf("\tlevel_idc          %d \r\n", sps->level_idc );
DBG serprintf("\tsps_id             %02X \r\n", sps_id );
DBG serprintf("\tlog2_max_frame_num %d \r\n", sps->log2_max_frame_num  );
DBG serprintf("\tnum_ref_frames     %d \r\n", sps->num_ref_frames  );
DBG serprintf("\tmbs_only           %d \r\n", sps->frame_mbs_only_flag  );
DBG serprintf("\tmb_aff             %d \r\n", sps->mb_aff  );
DBG serprintf("\twidth              %d \r\n", sps->width  );
DBG serprintf("\theight             %d \r\n", sps->height );
	
	int vui_parameters_present_flag = BITS_get1( bits );
	if ( vui_parameters_present_flag ) {
		parse_VUI( sps, bits );
	}
	
	sps->valid = 1;
DBG serprintf("\n");

	return 0;	
}

int H264_parse_PPS( H264_SPS *sps, const UCHAR *p, int len )
{
DBG serprintf("H264: parse_SPS, len %d\r\n", len);
	BITS _bits;
	BITS *bits = &_bits;

	BITS_init_h264( bits, (UCHAR*)p, len * 8 );

	int pic_parameter_set_id     = BITS_get1( bits );   	
	int seq_parameter_set_id     = BITS_get1( bits );   	
	int entropy_coding_mode_flag = BITS_get1( bits );

DBG serprintf("pps id %d  seq id %d  coding %d\r\n", pic_parameter_set_id, seq_parameter_set_id, entropy_coding_mode_flag );
	if( sps ) {
		sps->entropy_coding_mode_flag = entropy_coding_mode_flag;
	}
	return 0;	
}

int H264_parse_slice_header( const H264_SPS *sps, const UCHAR *p, int *type, int *_tbf, int *_frame_num )
{
DBGP4 serprintf("H264: parse_slice_header: ");
	BITS _bits;
	BITS *bits = &_bits;

	BITS_init( bits, (UCHAR*)p, 16 * 8 );
	int tbf = H264_FRAME;
	
	char *slice_name[5] = { "P", "B", "I", "SP", "SI" };
	int   slice_type[5] = { P_VOP, B_VOP, I_VOP, P_VOP, I_VOP };

	int first_mb_in_slice = get_ue_golomb( bits );
DBGP4 serprintf("first_mb %d  ", first_mb_in_slice);

	int slice = get_ue_golomb( bits );
	if ( slice > 4 ) {
		slice -= 5;
	}

	if( slice < 0 || slice > 4 ){
		return 1;
	}

DBGP4 serprintf("type %d / %s  ", slice, slice_name[slice]);
	
	int pps_id = get_ue_golomb( bits );
DBGP4 serprintf("pps %d  ", pps_id);

	int frame_num = BITS_get( bits, sps->log2_max_frame_num );
DBGP4 serprintf("num %2d  ", frame_num);

	if ( sps->frame_mbs_only_flag ) {
		///
DBGP4 serprintf("FRAME only");
	} else {
		if ( BITS_get1( bits ) ) {	//field_pic_flag
			int bottom = BITS_get1( bits );	//bottom_field_flag
DBGP4 serprintf("%s  ", bottom ? "BOT" : "TOP" );
			tbf = bottom ? H264_BOT_FIELD : H264_TOP_FIELD;

		} else {
DBGP4 serprintf("FRAME ");
		}
	}
DBGP4 serprintf("\r\n");

	if( type )
		*type = slice_type[slice]; 
	if( _tbf )
		*_tbf = tbf;
	if( _frame_num )
		*_frame_num = frame_num;
	return 0;
}

int H264_get_video_props( VIDEO_PROPERTIES *video, const UCHAR *p, int len, H264_SPS *sps )
{
	const UCHAR *end = p + len;
	UCHAR sync[4] = { 0x00, 0x00, 0x00, 0x01 };

	while( p < end  ) {
		if( !memcmp( p, sync, 4 ) ) {
			int nal = *(p + 4) & 0x1F;
			if( nal == NAL_SPS ) {
				H264_parse_SPS( sps, p + 5, end - (p + 5) );
				
				video->profile  = sps->profile_idc;
				H264_get_profile_name( video->profile, video->profile_name, AV_NAME_LEN );
				video->level    = sps->level_idc;
				H264_get_level_name( video->level, video->level_name, AV_NAME_LEN );
				
				video->width    = sps->width;
				video->height   = sps->height;
				video->aspect_n = sps->sar_num;
				video->aspect_d = sps->sar_den;

				video->scale    = sps->num_units_in_tick * 2;
				video->rate     = sps->time_scale;

				if( !video->scale || !video->rate ) {
					video->scale = 1;
					video->rate = 25;
				}	
				video->bytesPerSec = 0;
				video->fourcc = VIDEO_FOURCC_H264;
				video->format = VIDEO_FORMAT_H264;
				video->valid  = 1;
				
				return 0;
			}
			p += 4;
		} else {
			p++;
		}
	}

	return 1;
}

int H264_find_NAL2( const UCHAR *p, int len, int *NAL, int *nri, int *more )
{
	int i;
	UCHAR sync[4] = { 0x00, 0x00, 0x00, 0x01 };
	
	for (i = 0; i < len - 4; i++, p++ ) {
		if( !memcmp( p, sync, 4 ) && !(*(p + 4) & 0x80) ) {
			if( NAL )
				*NAL  =  *(p + 4) & 0x1F;
			if( nri )
				*nri  = (*(p + 4) >> 5) & 0x03;
			if( more )
				*more =  *(p + 5);
			return i;
		}
	}
	return -1;
}

int H264_find_NAL( const UCHAR *p, int len, int *NAL )
{
	return H264_find_NAL2( p, len, NAL, NULL, NULL );
}

int H264_find_AUD( const UCHAR *p, int len )
{
	int i;
	UCHAR sync[4] = { 0x00, 0x00, 0x00, 0x01 };
	
	for (i = 0; i < len; ) {
		if( !memcmp( p, sync, 4 ) ) {
			int nal = *(p + 4) & 0x1F;
			if( nal == NAL_AUD ) {
				return i;
			}
			i += 4;
			p += 4;
		} else {
			i++;
			p++;
		}
	}
	return -1;
}

int H264_find_SLICE( const UCHAR *p, int len, int *sps )
{
	int i;
	UCHAR sync[4] = { 0x00, 0x00, 0x00, 0x01 };
	
	for (i = 0; i < len; ) {
		if( !memcmp( p, sync, 4 ) ) {
			int nal = *(p + 4) & 0x1F; 
			if( nal == NAL_SLICE || nal == NAL_IDR_SLICE ) {
				return i;
			}
			if( nal == NAL_SPS && sps ) {
				*sps = 1;
				return i;
			}
			i += 4;
			p += 4;
		} else {
			i++;
			p++;
		}
	}
	return -1;
}

static int _h264_init( STREAM *s, int time )
{
DBGMNG serprintf("init_mangler_H264( %d )\r\n", time );	
	return 0;
}

static int _h264_pre( STREAM *s, CBE *cbe, STREAM_CDATA *cdata )
{
	int nal;
	UCHAR *p = cbe_get_p( cbe );
	int rest = cdata->size;
	
	while( rest > 0 ) {
		int nri;
		int more;
		int ofs = H264_find_NAL2( p, rest, &nal, &nri, &more );
		if( ofs != -1 ) {
			 if( nal == NAL_SLICE || nal == NAL_IDR_SLICE ) {
				int fnum;
				int idr  = (more >> 6) & 0x01;
				int prid =  more       & 0x3F;
				H264_parse_slice_header( &s->video->sps, p + ofs + 5, &cdata->frm_type, NULL, &fnum );
DBGMNG serprintf("type: %c  nri %X  idr %d  prid %2d  frame_num %2d\r\n", frame_type( cdata->frm_type ), nri, idr, prid, fnum );
				if ( cdata->frm_type == B_VOP && nri ) {
					cdata->frm_type = BR_VOP;
				}
				
				break;
			} else {
				p    += ofs + 1;
				rest -= ofs + 1;
			}
		} else {
			break;
		}
	}

	return 0;
}

static int _h264_post( STREAM *s, VIDEO_FRAME **p_frame )
{
	return 0;
}

static int _h264_flush( STREAM *s )
{
	return 0;
}


STREAM_VIDEO_MANGLER stream_video_mangler_H264 = 
{
	"H264",
	_h264_init,
	_h264_pre,
	_h264_post,
	_h264_flush,
};

#endif

