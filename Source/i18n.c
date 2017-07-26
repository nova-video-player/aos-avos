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
#include "util.h"
#include "wchar.h"
#include "i18n.h"
#include "file.h"
#include "astdlib.h"

#ifdef CONFIG_I18N
#define CONFIG_ALL_FONTS

#define DBG if(1)

typedef struct {
	wchar cp;
	wchar uni;
} CP_ENTRY;

#include "i18n_932.c"
#include "i18n_936.c"
#include "i18n_949.c"
#include "i18n_950.c"

static CP_ENTRY *codepage_data = NULL;
static int codepage_entries = 0;
static int codepage_loaded = 0;

static int I18N_codepage = 1252;	// default codepage

static void unload_codepage( void )
{
	codepage_data    = NULL;
	codepage_entries = 0;
	codepage_loaded  = 0;
}

static int load_codepage( int codepage ) 
{
	if( codepage_loaded == codepage ) {
		return 0;
	}
	
serprintf("load_codepage: %d\r\n", codepage );
	unload_codepage();
	
	switch( codepage ) {
	case 932:
		codepage_data    = cp932_unicode; 
		codepage_entries = sizeof(cp932_unicode) / sizeof( CP_ENTRY );
		break;
	case 936:
		codepage_data    = cp936_unicode; 
		codepage_entries = sizeof(cp936_unicode) / sizeof( CP_ENTRY );
		break;
	case 949:
		codepage_data    = cp949_unicode; 
		codepage_entries = sizeof(cp949_unicode) / sizeof( CP_ENTRY );
		break;
	case 950:
		codepage_data    = cp950_unicode; 
		codepage_entries = sizeof(cp950_unicode) / sizeof( CP_ENTRY );
		break;
	default:
		break;
	}
serprintf("codepage_entries: %d\r\n", codepage_entries );
	
	codepage_loaded = codepage;
	return 0;
}

static int loaded_codepage_to_unicode( const unsigned char *t, wchar *uc ) 
{
	if( !codepage_data || !codepage_entries )
		return 0;
	
	wchar cp = t[0] << 8 | t[1];
	// a little binary search
	int top    = codepage_entries - 1; 
	int bottom = 0;
		
	while( top >= bottom ) {
		int middle = ( top + bottom ) / 2;
		CP_ENTRY *e = codepage_data + middle;
		int c = cp - e->cp;

		if(c == 0) {
			*uc = e->uni;
//serprintf("cp %04x  uni %04X\r\n", cp, *uc );
			return 2;
		} else if(c < 0) {
			top = middle - 1;
		} else {
			bottom = middle + 1;
		}
	}
//serprintf("cp %04x  uni ----\r\n", cp );
	return 0;
}

// ************************************************
//
//	0000 - UTF-8
//
// ************************************************
static int cp2uc_utf8( const unsigned char *t, wchar *uc )
{
	const unsigned char *t2 = u8_to_u16( uc, t);
	return t2 - t;
}

// ************************************************
//
//	1252 - West European
//
// ************************************************
static const wchar cp1252_unicode[] = {
	0x20AC, 0x0020, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0020, 0x017D, 0x0020,
	0x0020, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0020, 0x017E, 0x0178
};


static int cp2uc_1252( const unsigned char *t, wchar *uc )
{
	if( (*t) < 0x80) {
		*uc = *t;
		return 1;
	}
	if( (*t) < 0xA0) {
		*uc = cp1252_unicode[(*t) - 0x80];
		return 1;
	}
	*uc = *t;
	return 1;
}

#ifdef CONFIG_CP1250
// ************************************************
//
//	1250 - East European
//
// ************************************************
static const wchar cp1250_unicode[] =
{
	0x20AC, 0x0020, 0x201A, 0x0020, 0x201E, 0x2026, 0x2020, 0x2021,
	0x0020, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
	0x0020, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x0020, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
	0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
	0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
	0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
	0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
	0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
	0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
	0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
	0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
	0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
	0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
	0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
	0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9
};

static int cp2uc_1250( const unsigned char *t, wchar *uc )
{
	int cp = *t;
	if( cp >= 0x80 ) {
		*uc = cp1250_unicode[ cp - 0x80 ];
		return 1;
	}
	return 0;
}
#endif

#ifdef CONFIG_CP1251
// ************************************************
//
//	1251 - RUSSIAN CYRILLIC
//
// ************************************************
static int cp2uc_1251( const unsigned char *t, wchar *uc )
{
	if( *t >= 0xC0 ) {
		*uc = *t + 0x410 - 0xC0;
		return 1;
	}
	return 0;
}
#endif

#ifdef CONFIG_CP1253
// ************************************************
//
//	1253 - GREEK CYRILLIC
//
// ************************************************
static const wchar cp1253_unicode[] =
{
	// 0x80
	0x20AC, 0x0020, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x0020, 0x2030, 0x0020, 0x2039, 0x0020, 0x0020, 0x0020, 0x0020,
	// 0x90
	0x0020, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x0020, 0x2122, 0x0020, 0x203A, 0x0020, 0x0020, 0x0020, 0x0020,
	// 0xa0
	0x00a0, 0x0385, 0x0386, 0x00a3, 0x00a4, 0x00a5, 0x00a6, 0x00a7,
	0x00a8, 0x00a9, 0x0020, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x2015,
	// 0xb0
	0x00b0, 0x00b1, 0x00b2, 0x00b3, 0x0384, 0x00b5, 0x00b6, 0x00b7,
	0x0388, 0x0389, 0x038a, 0x00bb, 0x038c, 0x00bd, 0x038e, 0x038f,
	// 0xc0
	0x0390, 0x0391, 0x0392, 0x0393, 0x0394, 0x0395, 0x0396, 0x0397,
	0x0398, 0x0399, 0x039a, 0x039b, 0x039c, 0x039d, 0x039e, 0x039f,
        // 0xd0
	0x03a0, 0x03a1, 0x0020, 0x03a3, 0x03a4, 0x03a5, 0x03a6, 0x03a7,
	0x03a8, 0x03a9, 0x03aa, 0x03ab, 0x03ac, 0x03ad, 0x03ae, 0x03af,
	// 0xe0
	0x03b0, 0x03b1, 0x03b2, 0x03b3, 0x03b4, 0x03b5, 0x03b6, 0x03b7,
	0x03b8, 0x03b9, 0x03ba, 0x03bb, 0x03bc, 0x03bd, 0x03be, 0x03bf,
	// 0xf0
	0x03c0, 0x03c1, 0x03c2, 0x03c3, 0x03c4, 0x03c5, 0x03c6, 0x03c7,
	0x03c8, 0x03c9, 0x03ca, 0x03cb, 0x03cc, 0x03cd, 0x03ce, 0x0020,
};

static int cp2uc_1253( const unsigned char *t, wchar *uc )
{
	int cp = *t;
	if( cp >= 0x80 ) {
		*uc = cp1253_unicode[ cp - 0x80 ];
		return 1;
	}
	return 0;
}
#endif

#ifdef CONFIG_CP1254
// ************************************************
//
//	1254 - Turkish (aka Latin-5 aka iso8859-9)
//
// ************************************************
static const wchar cp1254_unicode[] =
{
	// 0x80
	0x20AC, 0x0020, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0020, 0x0020, 0x0020,
	// 0x90
	0x0020, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0020, 0x0020, 0x0178,
	// 0xa0
	0x00a0, 0x00a1, 0x00a2, 0x00a3, 0x00a4, 0x00a5, 0x00a6, 0x00a7,
	0x00a8, 0x00a9, 0x00aa, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x00af,
	// 0xb0
	0x00b0, 0x00b1, 0x00b2, 0x00b3, 0x00b4, 0x00b5, 0x00b6, 0x00b7,
	0x00b8, 0x00b9, 0x00ba, 0x00bb, 0x00bc, 0x00bd, 0x00be, 0x00bf,
	// 0xc0
	0x00c0, 0x00c1, 0x00c2, 0x00c3, 0x00c4, 0x00c5, 0x00c6, 0x00c7,
	0x00c8, 0x00c9, 0x00ca, 0x00cb, 0x00cc, 0x00cd, 0x00ce, 0x00cf,
        // 0xd0
	0x011e, 0x00d1, 0x00d2, 0x00d3, 0x00d4, 0x00d5, 0x00d6, 0x00d7,
	0x00d8, 0x00d9, 0x00da, 0x00db, 0x00dc, 0x0130, 0x015e, 0x00df,
	// 0xe0
	0x00e0, 0x00e1, 0x00e2, 0x00e3, 0x00e4, 0x00e5, 0x00e6, 0x00e7,
	0x00e8, 0x00e9, 0x00ea, 0x00eb, 0x00ec, 0x00ed, 0x00ee, 0x00ef,
	// 0xf0
	0x011f, 0x00f1, 0x00f2, 0x00f3, 0x00f4, 0x00f5, 0x00f6, 0x00f7,
	0x00f8, 0x00f9, 0x00fa, 0x00fb, 0x00fc, 0x0131, 0x015f, 0x00ff,
};

static int cp2uc_1254( const unsigned char *t, wchar *uc )
{
	int cp = *t;
	if( cp >= 0x80 ) {
		*uc = cp1254_unicode[ cp - 0x80 ];
		return 1;
	}
	return 0;
}
#endif

#ifdef CONFIG_CP1255
// ************************************************
//
//	1255 - HEBREW
//
// ************************************************
static int cp2uc_1255( const unsigned char *t, wchar *uc )
{
	if( *t >= 0xE0 && *t <= 0xFA ) {
		*uc = *t + 0x05D0 - 0xE0;
		return 1;
	}
	return 0;
}
#endif

#ifdef CONFIG_CP1256
// ************************************************
//
//	1256 - Arabic (aka iso8859-6)
//
// ************************************************
static const wchar cp1256_unicode[] =
{
	// 0x80
	0x20AC, 0x067E, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0679, 0x2039, 0x0152, 0x0686, 0x0698, 0x0688,
	// 0x90
	0x06AF, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x06A9, 0x2122, 0x0691, 0x203A, 0x0153, 0x200C, 0x200D, 0x06BA,
	// 0xa0
	0x00A0, 0x060C, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
	0x00A8, 0x00A9, 0x06BE, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
	// 0xb0
	0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
	0x00B8, 0x00B9, 0x061B, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x061f,
	// 0xc0
	0x06C1, 0x0621, 0x0622, 0x0623, 0x0624, 0x0625, 0x0626, 0x0627,
	0x0628, 0x0629, 0x062a, 0x062b, 0x062c, 0x062d, 0x062e, 0x062f,
	// 0xd0
	0x0630, 0x0631, 0x0632, 0x0633, 0x0634, 0x0635, 0x0636, 0x00D7,
	0x0637, 0x0638, 0x0639, 0x063a, 0x0640, 0x0641, 0x0642, 0x0643, 
	// 0xE0
	0x00E0, 0x0644, 0x00E2, 0x0645, 0x0646, 0x0647,	0x0648, 0x00E7,
	0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x0649, 0x064a, 0x00EE, 0x00EF,
	// 0xf0
	0x064b, 0x064c, 0x064d, 0x064e, 0x00F4, 0x064f, 0x0650, 0x00F7,
	0x0651, 0x00F9, 0x0652, 0x00FB, 0x00Fc, 0x200E, 0x200F, 0x06D2,
};

static int cp2uc_1256( const unsigned char *t, wchar *uc )
{
	int cp = *t;
	if( cp >= 0x80 ) {
		*uc = cp1256_unicode[ cp - 0x80 ];
		return 1;
	}
	return 0;
}
#endif

#ifdef CONFIG_CP1257
// ************************************************
//
//	1257 - Baltic (aka iso8859-13)
//
// ************************************************
static const wchar cp1257_unicode[] =
{
	/* 0x80*/
	0x20AC, 0x0020, 0x201A, 0x0020, 0x201E, 0x2026, 0x2020, 0x2021, 0x0020, 0x2030, 0x0020, 0x2039, 0x0020, 0x00A8, 0x02C7, 0x00B8,
	/* 0x90*/
	0x0020, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0020, 0x2122, 0x0020, 0x203A, 0x0020, 0x00AF, 0x02DB, 0x0020,
	/* 0xa0*/
	0x00a0, 0x0020, 0x00a2, 0x00a3, 0x00a4, 0x0020, 0x00a6, 0x00a7, 0x00d8, 0x00a9, 0x0156, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x00c6,
	/* 0xb0*/
	0x00b0, 0x00b1, 0x00b2, 0x00b3, 0x201c, 0x00b5, 0x00b6, 0x00b7, 0x00f8, 0x00b9, 0x0157, 0x00bb, 0x00bc, 0x00bd, 0x00be, 0x00e6,
	/* 0xc0*/
	0x0104, 0x012e, 0x0100, 0x0106, 0x00c4, 0x00c5, 0x0118, 0x0112, 0x010c, 0x00c9, 0x0179, 0x0116, 0x0122, 0x0136, 0x012a, 0x013b,
	/* 0xd0*/
	0x0160, 0x0143, 0x0145, 0x00d3, 0x014c, 0x00d5, 0x00d6, 0x00d7, 0x0172, 0x0141, 0x015a, 0x016a, 0x00dc, 0x017b, 0x017d, 0x00df,
	/* 0xe0*/
	0x0105, 0x012f, 0x0101, 0x0107, 0x00e4, 0x00e5, 0x0119, 0x0113, 0x010d, 0x00e9, 0x017a, 0x0117, 0x0123, 0x0137, 0x012b, 0x013c,
	/* 0xf0*/
	0x0161, 0x0144, 0x0146, 0x00f3, 0x014d, 0x00f5, 0x00f6, 0x00f7, 0x0173, 0x0142, 0x015b, 0x016b, 0x00fc, 0x017c, 0x017e, 0x02D9,
};

static int cp2uc_1257( const unsigned char *t, wchar *uc )
{
	int cp = *t;
	if( cp >= 0x80 ) {
		*uc = cp1257_unicode[ cp - 0x80 ];
		return 1;
	}
	return 0;
}
#endif

#ifdef CONFIG_CP1258
// ************************************************
//
//	1258 - Vietnamese
//
// ************************************************
__attribute__((unused))
static const wchar cp1258_unicode[] =
{
	0x20AC, 0x0020, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0020, 0x2039, 0x0152, 0x0020, 0x0020, 0x0020,
	0x0020, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0020, 0x203A, 0x0153, 0x0020, 0x0020, 0x0178, 
	0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF, 
	0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF, 
	0x00C0, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x0300, 0x00CD, 0x00CE, 0x00CF, 
	0x0110, 0x00D1, 0x0309, 0x00D3, 0x00D4, 0x01A0, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x01AF, 0x0303, 0x00DF, 
	0x00E0, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x0301, 0x00ED, 0x00EE, 0x00EF, 
	0x0111, 0x00F1, 0x0323, 0x00F3, 0x00F4, 0x01A1, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x01B0, 0x20AB, 0x00FF, 
};

static int cp2uc_1258( const unsigned char *t, wchar *uc )
{
	int cp = *t;
	if( cp >= 0x80 ) {
		*uc = cp1257_unicode[ cp - 0x80 ];
		return 1;
	}
	return 0;
}
#endif

#ifdef CONFIG_CP874
// ************************************************
//
//	874 - Thai
//
// ************************************************
static const wchar cp874_unicode[] =
{
	0x20ac, 0x0081, 0x0082, 0x0083, 0x0084, 0x2026, 0x0086, 0x0087, 0x0088, 0x0089, 0x008a, 0x008b, 0x008c, 0x008d, 0x008e, 0x008f, 
	0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x0098, 0x0099, 0x009a, 0x009b, 0x009c, 0x009d, 0x009e, 0x009f, 
	0x00a0, 0x0e01, 0x0e02, 0x0e03, 0x0e04, 0x0e05, 0x0e06, 0x0e07, 0x0e08, 0x0e09, 0x0e0a, 0x0e0b, 0x0e0c, 0x0e0d, 0x0e0e, 0x0e0f, 
	0x0e10, 0x0e11, 0x0e12, 0x0e13, 0x0e14, 0x0e15, 0x0e16, 0x0e17, 0x0e18, 0x0e19, 0x0e1a, 0x0e1b, 0x0e1c, 0x0e1d, 0x0e1e, 0x0e1f, 
	0x0e20, 0x0e21, 0x0e22, 0x0e23, 0x0e24, 0x0e25, 0x0e26, 0x0e27, 0x0e28, 0x0e29, 0x0e2a, 0x0e2b, 0x0e2c, 0x0e2d, 0x0e2e, 0x0e2f, 
	0x0e30, 0x0e31, 0x0e32, 0x0e33, 0x0e34, 0x0e35, 0x0e36, 0x0e37, 0x0e38, 0x0e39, 0x0e3a, 0x00db, 0x00dc, 0x00dd, 0x00de, 0x0e3f, 
	0x0e40, 0x0e41, 0x0e42, 0x0e43, 0x0e44, 0x0e45, 0x0e46, 0x0e47, 0x0e48, 0x0e49, 0x0e4a, 0x0e4b, 0x0e4c, 0x0e4d, 0x0e4e, 0x0e4f, 
	0x0e50, 0x0e51, 0x0e52, 0x0e53, 0x0e54, 0x0e55, 0x0e56, 0x0e57, 0x0e58, 0x0e59, 0x0e5a, 0x0e5b, 0x00fc, 0x00fd, 0x00fe, 0x00ff, 
};

static int cp2uc_874( const unsigned char *t, wchar *uc )
{
	int cp = *t;
	if( cp >= 0x80 ) {
		*uc = cp874_unicode[ cp - 0x80 ];
		return 1;
	}
	return 0;
}
#endif

#ifdef CONFIG_CP936
// ************************************************
//
//	936 - SIMPLIFIED CHINESE - GB18030
//
// ************************************************
static int cp2uc_936( const unsigned char *t, wchar *uc )
{
	return loaded_codepage_to_unicode( t, uc );
}
#endif

#ifdef CONFIG_CP950
// ************************************************
//
//	950 - TRADITIONAL CHINESE - BIG5
//
// ************************************************
static int cp2uc_950( const unsigned char *t, wchar *uc )
{
	return loaded_codepage_to_unicode( t, uc );
}
#endif

#ifdef CONFIG_CP932
// ************************************************
//
//	932 - JAPANESE - JIS0208
//
// ************************************************
static int cp2uc_932( const unsigned char *t, wchar *uc )
{
	return loaded_codepage_to_unicode( t, uc );
}
#endif

#ifdef CONFIG_CP949
// ************************************************
//
//	949 - KOREAN - UNIHAN
//
// ************************************************
static int cp2uc_949( const unsigned char *t, wchar *uc )
{
	return loaded_codepage_to_unicode( t, uc );
}
#endif

//
// what codepages do we support and what are the functions
// to translate from codepage (ID3 TAGs) to unicode
//
struct CP2UC{
	int 	codepage;
	int  	(*cp2uc) ( const unsigned char *t, wchar *uc );
} cp2uc[] = 
{
	   0,	cp2uc_utf8,
	
	1252,	cp2uc_1252,
#ifdef CONFIG_CP1250
	1250,	cp2uc_1250,
#endif
#ifdef CONFIG_CP1251
	1251,	cp2uc_1251,
#endif
#ifdef CONFIG_CP1253
	1253,	cp2uc_1253,
#endif
#ifdef CONFIG_CP1254
	1254,	cp2uc_1254,
#endif
#ifdef CONFIG_CP1255
	1255,	cp2uc_1255,
#endif
#ifdef CONFIG_CP1256
	1256,	cp2uc_1256,
#endif
#ifdef CONFIG_CP1257
	1257,	cp2uc_1257,
#endif
#ifdef CONFIG_CP1258
	1257,	cp2uc_1258,
#endif
#ifdef CONFIG_CP874
	874,	cp2uc_874, 
#endif
#ifdef CONFIG_CP936
	936,	cp2uc_936, 
#endif
#ifdef CONFIG_CP950
	950,	cp2uc_950,
#endif
#ifdef CONFIG_CP932
	932,	cp2uc_932, 
#endif
#ifdef CONFIG_CP949
	949,	cp2uc_949, 
#endif
};

#define CP2UC_NUM (sizeof( cp2uc ) / sizeof( struct CP2UC ))

// ************************************************
//
//	I18N_codepage_to_unicode
//
// ************************************************
int I18N_codepage_to_unicode( const unsigned char *t, wchar *uc )
{
	int codepage = I18N_get_codepage();
	int i;	

	// try to find a translator for the current codepage
	for( i = 0; i < CP2UC_NUM; i++ ) {
		if( cp2uc[i].codepage == codepage ) {
			if( cp2uc[i].cp2uc ) {
				int ret = cp2uc[i].cp2uc( t, uc );
				if( ret ) 
					return ret;
			}
		}
	}
	
	// nothing found, then just translate ASCII -> Unicode
	*uc = *t;
//serprintf("AS: %02X -> %04X\r\n", *t, *uc );
	return 1;
}

// ************************************************************
//
//	I18N_codepage_to_utf8
//
// ************************************************************
void I18N_codepage_to_utf8( char *utf8, const char *cp, int max )
{
	static wchar utf16[1024];
	wchar *uc = utf16;

	int count = 0;
	while( *cp != '\0' && count++ < sizeof(utf16) - 1 ) {
		// take care about wide codepage chars!
		cp += I18N_codepage_to_unicode( cp, uc );
		uc++; 
	}
	*uc = '\0';
	
	// convert to utf8
	utf16_to_utf8( (UCHAR*)utf8, utf16, max );
	return; 
}

// ************************************************
//
//	I18N_codepage_supported
//
// ************************************************
int I18N_codepage_supported( int codepage )
{
	int i;

	for( i = 0; i < CP2UC_NUM; i++ ) {
		if( cp2uc[i].codepage == codepage ) {
			return 1;
		}
	}
	return 0;
}

// ************************************************
//
//	I18N_set_codepage
//
// ************************************************
int I18N_set_codepage( int codepage )
{
	if( I18N_codepage_supported( codepage ) ) {
		I18N_codepage = codepage;
	} else {
		I18N_codepage = 1252;
	}
	return load_codepage( I18N_codepage );
}

// ************************************************
//
//	I18N_get_codepage
//
// ************************************************
int I18N_get_codepage( void )
{
	return I18N_codepage;
}

// ************************************************
//
//	I18N_load
//
// ************************************************
int I18N_load( void )
{
serprintf("I18N_load\r\n");
	return 0;
}

// ************************************************
//
//	I18N_unload
//
// ************************************************
int I18N_unload( void )
{
serprintf("I18N_unload\r\n");
	unload_codepage();
	return 0;
}

struct enc_ctx {
	int count;
	int score;
	int utf8;
	int total;
};

void *I18N_check_encoding_init( void )
{
	int *ctx = acalloc( 1, sizeof(struct enc_ctx) );
	return ctx;
}

//	UTF-8 is :
//	U-00000000 - U-0000007F:  0xxxxxxx  
//	U-00000080 - U-000007FF:  110xxxxx 10xxxxxx  
//	U-00000800 - U-0000FFFF:  1110xxxx 10xxxxxx 10xxxxxx  
//	U-00010000 - U-001FFFFF:  11110xxx 10xxxxxx 10xxxxxx 10xxxxxx  
//	U-00200000 - U-03FFFFFF:  111110xx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx  
//	U-04000000 - U-7FFFFFFF:  1111110x 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx  

void I18N_check_encoding_update( void *ctx, const unsigned char *text, int size ) 
{
	struct enc_ctx *c = ctx;
	
	if( !text )
		return;
	
	while( size ) {
		unsigned char t = *text;
		c->total ++;
//serprintf("%02X ", t );
		if( c->score == 0 ) {
			c->count = 0;
			if( t < 0x80 ) {
//serprintf("ascii\n");
				c->utf8++;
			} else if ( (t & 0xFE) == 0xFC ) {
				c->score = 5;
			} else if ( (t & 0xFC) == 0xF8 ) {
				c->score = 4;
			} else if ( (t & 0xF8) == 0xF0 ) {
				c->score = 3;
			} else if ( (t & 0xF0) == 0xE0 ) {
				c->score = 2;
			} else if ( (t & 0xE0) == 0xC0 ) {
				c->score = 1;
			} else {
//serprintf("  err\n");
				// error
			}
		} else {
//serprintf("  %d/%d", c->count, c->score);
			if( (t & 0xC0) == 0x80 ) {
				c->count++;
				if( c->count == c->score ) {
//serprintf("  utf8\n");
					c->utf8 += c->score + 1;
					c->score = 0;
				} else {
//serprintf("\n");
				}
			} else {
//serprintf("  err\n");
				// error
				c->score = 0;
			}
		}
		text ++;
		size --;
	}
	
	return;
}

void I18N_check_encoding_finish( void *ctx, int *utf8 )
{
	struct enc_ctx *c = ctx;
serprintf("total: %d  utf %d  ", c->total, c->utf8 );
	// do not allow any error, all chars must conform to UTF-8!
	if( c->utf8 == c->total ) {
serprintf("UTF8!\n");		
		*utf8 = 1;
	} else {
serprintf("ASCII/CODEPAGE!\n");		
		*utf8 = 0;
	}
	afree( ctx );
}


#ifdef DEBUG_MSG
static void test( void )
{
	unsigned char cp[] = {0xB8, 0xF1, 0xBF, 0xE4, 0xB9, 0xCC, 0xBD, 0xBA, 0xC5, 0xCD, 0xB8, 0xAE, 0xB1, 0xD8, 0xC0, 0xE5, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0x00 };
	unsigned char utf1[] = { 0xEB, 0x82, 0xB4, 0x20, 0xEC, 0x9D, 0xB4, 0xEB, 0xA6, 0x84, 0xEC, 0x9D, 0x00};
	unsigned char utf2[] = { 0x80, 0x20, 0xEC, 0x97, 0x90, 0xEB, 0x8F, 0x84, 0xEA, 0xB0, 0x80, 0xEC, 0x99, 0x80, 0x20, 0xEC, 0xBD, 0x94, 0xEB, 0x82, 0x9C, 0x00};
	int utf8;

	void *ctx = I18N_check_encoding_init();
	I18N_check_encoding_update( ctx, cp, strlen( cp ) );
	I18N_check_encoding_finish( ctx, &utf8 );
serprintf("utf: %d\n", utf8 );
	ctx = I18N_check_encoding_init();
	I18N_check_encoding_update( ctx, utf1, strlen( utf1 ) );
	I18N_check_encoding_update( ctx, utf2, strlen( utf2 ) );
	I18N_check_encoding_finish( ctx, &utf8 );
serprintf("utf: %d\n", utf8 );
}
DECLARE_DEBUG_COMMAND_VOID( "tcp", test );

static void set_codepage( int argc, char **argv )
{
	if( argc > 1 ) {
		int cp = atoi( argv[1] );
		I18N_set_codepage( cp );
	}
	serprintf("loaded codepage: %d\r\n", I18N_get_codepage() );
}

static void dump_codepage( int argc, char **argv )
{
	int i;	
	for( i = 0; i < CP2UC_NUM; i++ ) {
		serprintf("[%2d] %4d\n", i, cp2uc[i].codepage );
	}

}

DECLARE_DEBUG_COMMAND( "scp", set_codepage );
DECLARE_DEBUG_COMMAND( "dcp", dump_codepage );

#endif
#endif
