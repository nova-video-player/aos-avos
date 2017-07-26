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
#include "hevc.h"
#include "util.h"

#include <string.h>
#include <stdio.h>

#ifdef CONFIG_HEVC

#ifdef STANDALONE
#define DBG if(0)
#else
#define DBG    if(Debug[DBG_STREAM] > 1)
#define DBGP4  if(Debug[DBG_PARSER] > 3)
#define DBGMNG if(Debug[DBG_MANGLER])
#endif

int HEVC_convert_nal_units(UCHAR *data, int data_size, 
			   UCHAR *out,  int out_size, 
			   int *_sps_pps_size, int *_nal_size)
{
	UCHAR *end = data + data_size;
	int sps_pps_size = 0;
	int nal_size;

	if( data_size <= 3 || ( !data[0] && !data[1] && data[2] <= 1 ) ) {
		return -1;
	}

	if( end - data < 23 ) {
		serprintf("extradata too small\n" );
		return -1;
	}

	data += 21;

	nal_size = (*data & 0x03) + 1;

	serprintf("nal_size %d\n", nal_size );
	if( _nal_size )
		*_nal_size = nal_size;
	data++;

	int num_arrays = *data++;
	serprintf("num_arrays %d\n", num_arrays );

	int i;
	for( i = 0; i < num_arrays; i++ ) {
		if( end - data < 3 ) {
			serprintf("extradata too small\n");
			return -1;
		}
		__attribute__((unused)) int type = *(data++) & 0x3f;

		int cnt = data[0] << 8 | data[1];
		data += 2;

		int j;
		for( j = 0; j < cnt; j++ ) {
			int nal_size;

			if( end - data < 2 ) {
				serprintf("extradata too small\n");
				return -1;
			}

			// nal unit size always 2 for HVCC
			nal_size = data[0] << 8 | data[1];
			serprintf("nal_size %d\n", nal_size );
			
			data += 2;

			if( nal_size < 0 || end - data < nal_size ) {
				serprintf("NAL unit size does not match\n");
				return -1;
			}

			if( sps_pps_size + 4 + nal_size > out_size ) {
				serprintf("outbuf too small\n");
				return -1;
			}

			out[sps_pps_size++] = 0;
			out[sps_pps_size++] = 0;
			out[sps_pps_size++] = 0;
			out[sps_pps_size++] = 1;

			memcpy(out + sps_pps_size, data, nal_size);
			data += nal_size;

			sps_pps_size += nal_size;
		}
	}

	*_sps_pps_size = sps_pps_size;

	return 0;
}

static const UCHAR sync_word[4] = { 0x00, 0x00, 0x00, 0x01 };

static int _convert_extradata(UCHAR *data, int data_size, 
			   int *_out_size, int *_nal_unit_size)
{
	UCHAR *p   = data;
	UCHAR *end = data + data_size;

	UCHAR *out;
	int out_max  = data_size;
	int out_size = 0;

	if( data_size <= 3 || ( !p[0] && !p[1] && p[2] <= 1 ) ) {
		return -1;
	}

	if( end - p < 23 ) {
		serprintf("extradata too small\n" );
		return -1;
	}

	out = amalloc( out_max );
	if( !out ) {
		return -1;
	}

	p += 21;

	int nal_unit_size = (*p & 0x03) + 1;

DBG serprintf("nal_size %d\n", nal_unit_size );
	if( _nal_unit_size ) {
		*_nal_unit_size = nal_unit_size;
	}
	p++;

	int num_arrays = *p++;
DBG serprintf("num_arrays %d\n", num_arrays );

	int i;
	for( i = 0; i < num_arrays; i++ ) {
		if( end - p < 3 ) {
			serprintf("extradata too small\n");
			goto ErrorExit;
		}
		int type = *(p++) & 0x3f;

		int cnt = p[0] << 8 | p[1];
DBG serprintf("[%d] type %02X  count %d\n", i, type, cnt );
		p += 2;

		int j;
		for( j = 0; j < cnt; j++ ) {
			int nal_size;

			if( end - p < 2 ) {
				serprintf("hvcc data too small\n");
				goto ErrorExit;
			}

			// nal unit size always 2 for HVCC
			nal_size = p[0] << 8 | p[1];
DBG serprintf("\t\t\tnal_size %d\n", nal_size );

			p += 2;

			if( nal_size < 0 || end - p < nal_size ) {
				serprintf("NAL unit size does not match\n");
				goto ErrorExit;
			}

			if( out_size + 4 + nal_size > out_max ) {
				serprintf("outbuf too small\n");
				goto ErrorExit;
			}

			memcpy(out + out_size, sync_word, 4);

			out_size += 4;

			memcpy(out + out_size, p, nal_size);
			p += nal_size;

			out_size += nal_size;
		}
	}
DBG serprintf("in %d  out %d\n", data_size, out_size );

	memcpy( data, out, out_size );

	if( _out_size ) {
		*_out_size = out_size;
	}

	afree(out);
	return 0;

ErrorExit:
	afree(out);
	return 1;
}

int HEVC_convert_extradata( VIDEO_PROPERTIES *video )
{
	UCHAR *p;
	int size;
DBG serprintf("HEVC_convert_extradata\r\n");

	if( video->extraDataSize ) {
		p    = video->extraData;
		size = video->extraDataSize;
	} else if ( video->extraDataSize2 ) {
		p    = video->extraData2;
		size = video->extraDataSize2;
	} else {
		return 1;
	}
	return _convert_extradata(p, size, &video->extraDataSize, &video->nal_unit_size );
}

static void _end_NAL( CBE *cbe, int *out_size ) {}

int HEVC_parse_NAL( UCHAR *d, int size, CBE *cbe, int *out_size, int nal_unit_size )
{
DBGP4 serprintf("HEVC_parse_NAL: %d\r\n", size);
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
		_end_NAL( cbe, out_size );
	}
	return 0;
}
#endif
