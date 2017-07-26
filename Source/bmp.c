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
#include "debug.h"
#include "bmp.h"

#include <unistd.h>

typedef struct {
	char  ida, idb;
	int   filesize;
	short reserveda;
	short reservedb;
	int   data_offset;
} BmpFileHeader;

#define FILE_HEADER_SIZE ( 2 * 1 + 4 + 2 * 2 + 4 )

typedef struct {
	int   ihsize;
	int   width;
	int   height;
	short planes;
	short bpp;
	int   compression;
	int   datasize;
	int   hppm;
	int   vppm;
	int   colors;
	int   important_colors;
} BmpInfoHeader;

#define INFO_HEADER_SIZE ( 3 * 4 + 2 * 2 + 6 * 4 )

// ********************************************************
//
//	BMP_write_header
//
// *********************************************************
void BMP_write_header( int fd, int width, int height, int bits_per_pixel )
{
	// init file header
	BmpFileHeader fh = {
		'B', 'M',
		( width * height ) * bits_per_pixel / 8 + FILE_HEADER_SIZE + INFO_HEADER_SIZE,
		0,
		0,
		FILE_HEADER_SIZE + INFO_HEADER_SIZE
	};

	// init info header
	BmpInfoHeader ih = {
		INFO_HEADER_SIZE,
		width,
		height,
		1,
		bits_per_pixel,
		0,
		0,
		0,
		0,
		0,
		0
	};

	// write file & info header
	int idx = 0;
	unsigned char buf[FILE_HEADER_SIZE + INFO_HEADER_SIZE];

	*((char*) &buf[idx]) = fh.ida;			idx += sizeof(char);
	*((char*) &buf[idx]) = fh.idb;			idx += sizeof(char);
	*((int*)  &buf[idx]) = fh.filesize;		idx += sizeof(int);
	*((short*)&buf[idx]) = fh.reserveda;		idx += sizeof(short);
	*((short*)&buf[idx]) = fh.reservedb;		idx += sizeof(short);
	*((int*)  &buf[idx]) = fh.data_offset;		idx += sizeof(int);

	*((int*)  &buf[idx]) = ih.ihsize;		idx += sizeof(int);
	*((int*)  &buf[idx]) = ih.width;		idx += sizeof(int);
	*((int*)  &buf[idx]) = ih.height;		idx += sizeof(int);
	*((short*)&buf[idx]) = ih.planes;		idx += sizeof(short);
	*((short*)&buf[idx]) = ih.bpp;			idx += sizeof(short);
	*((int*)  &buf[idx]) = ih.compression;		idx += sizeof(int);
	*((int*)  &buf[idx]) = ih.datasize;		idx += sizeof(int);
	*((int*)  &buf[idx]) = ih.hppm;			idx += sizeof(int);
	*((int*)  &buf[idx]) = ih.vppm;			idx += sizeof(int);
	*((int*)  &buf[idx]) = ih.colors;		idx += sizeof(int);
	*((int*)  &buf[idx]) = ih.important_colors;	idx += sizeof(int);

	write( fd, buf, FILE_HEADER_SIZE + INFO_HEADER_SIZE );
}
