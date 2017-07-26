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

#ifndef _COLOR_H
#define _COLOR_H

#include "types.h"

// rgb 16 bit code based on DM320 LUT 
#define TRANSPARENT_COLOR	0x0000

#define BLACK			0x0821
#define WHITE			0xFFFF
#define RED			0xF800
#define GREEN			0x07E0
#define BLUE			0x001F
#define CYAN			0x07FF
#define YELLOW			0xFFE0
#define GRAY			0x8410
#define PURPLE			0xF81F

#define DK_BLUE			0x0010
#define DK_GRAY			0x3186

#define MEDIUM_GRAY		0xA514
#define MEDIUM_BLUE		0x6D39

#define LT_GRAY			0xC618
#define LT_BLUE			0xA65E
#define LT_GREEN		0xC6F8
#define LT_YELLOW		0xE6EB
#define LT_GRAY_BLUE		0xB63A

typedef struct rgba_str {
	unsigned int b:8, g:8, r:8, a:8;
} rgba;


#define RGBA( R, G, B, A ) (rgba){ .r=(R), .g=(G), .b=(B), .a=(A) }

#define BGRA( B, G, R, A ) (bgra){ .r=(R), .g=(G), .b=(B), .a=(A) }

typedef struct argb_str {
	unsigned int a:8, r:8, g:8, b:8;
} argb;

typedef struct bgra_str {
        unsigned int b:8, g:8, r:8, a:8;
} bgra;

typedef struct {
	float h, s, v;
} hsv;

extern const rgba RGBA_TRANSPARENT;
extern const rgba RGBA_BLACK;
extern const rgba RGBA_WHITE;
extern const rgba RGBA_GRAY;

extern const rgba RGBA_RED;
extern const rgba RGBA_GREEN;
extern const rgba RGBA_BLUE;
extern const rgba RGBA_CYAN;
extern const rgba RGBA_YELLOW;
extern const rgba RGBA_MAGENTA;

extern const rgba RGBA_MEDIUM_GRAY;
extern const rgba RGBA_MEDIUM_BLUE;

extern const rgba RGBA_DK_GRAY;
extern const rgba RGBA_DK_BLUE;
extern const rgba RGBA_DK_RED;

extern const rgba RGBA_LT_GRAY;
extern const rgba RGBA_LT_BLUE;
extern const rgba RGBA_LT_GREEN;
extern const rgba RGBA_LT_YELLOW;
extern const rgba RGBA_LT_GRAY_BLUE;

extern const rgba RGBA_LLT_GRAY;

// Specific application colors
extern const rgba RGBA_MSGBOX_BACKGND;
extern const rgba RGBA_FONT_DARK_COLOR;
extern const rgba RGBA_FONT_LIGHT_COLOR;

// Reference color for YUV bitmaps ---------------------------------
#define YUV_BITMAP_DEFAULT_Y		0xBF
#define YUV_BITMAP_DEFAULT_U		0x17
#define YUV_BITMAP_DEFAULT_V		0xAD

USHORT	rgb32_to_rgb16(rgba c);

static inline rgba convert_rgb16_to_rgb32(USHORT rgb, UCHAR alpha) {
	return RGBA( ( rgb >>  ( 11 - 3 ) )& ( 0x1F << 3 ),
		( rgb >>  (  5 - 2 ) )& ( 0x3F << 2 ),
		( rgb << -(  0 - 3 ) )& ( 0x1F << 3 ),
		alpha );
}

#endif	// _COLOR_H
