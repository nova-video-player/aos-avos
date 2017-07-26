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

// ***************************************
//
// ISO 639 -1 and -2 languge code mappings
//
// more codes here: http://etext.virginia.edu/tei/iso639.html
//
// ***************************************
#include "global.h"
#include "types.h"
#include "debug.h"

#include <string.h>
#include <ctype.h>

typedef struct ISO639 {
	const char *iso;
	const char *name;
} ISO639;

//	2/3 letter ISO	AVOS verbose name (possibly translatable)
//	{ "",		"" },
static const ISO639 map[] = {
	{ "ar",		"s_arabic" },

	{ "bg",		"s_bulgarian" },
	{ "bul",	"s_bulgarian" },
	
	{ "bs",		"Bosnian" },
	{ "bos",	"Bosnian" },

	{ "cs",		"s_czech" },
	{ "ces"		"s_czech" },
	{ "cze",	"s_czech" },

	{ "da",		"s_danish" },
	{ "dan",	"s_danish" },

	{ "de",		"s_german" },
	{ "deu",	"s_german" },
	{ "ger",	"s_german" },
	
	{ "en",		"s_english" },
	{ "eng",	"s_english" },
	
	{ "el",		"s_greek" },
	{ "gre",	"s_greek" },
	{ "ell",	"s_greek" },

	{ "es",		"s_spanish" },
	{ "esl",	"s_spanish" },
	{ "spa",	"s_spanish" },
	
	{ "fi",		"s_finnish" },
	{ "fin",	"s_finnish" },

	{ "fr",		"s_french" },
	{ "fre",	"s_french" },
	{ "fra",	"s_french" },
	
	{ "he",		"s_hebrew" },
	{ "heb",	"s_hebrew" },
	{ "iw",		"s_hebrew" },
	
	{ "hu",		"s_hungarian" },
	{ "hun",	"s_hungarian" },

	{ "it",		"s_italian" },
	{ "ita",	"s_italian" },

	{ "ja",		"s_japanese" },
	{ "jpn",	"s_japanese" },
	
	{ "ko",		"s_korean" },
	{ "kor",	"s_korean" },
	
	{ "nl",		"s_dutch" },
	{ "nld",	"s_dutch" },
	{ "dut",	"s_dutch" },
	
	{ "no",		"s_norwegian" },
	{ "nor",	"s_norwegian" },

	{ "pl",		"s_polish" },
	{ "pol",	"s_polish" },

	{ "pt",		"s_portuguese" },
	{ "por",	"s_portuguese" },
	
	{ "ru",		"s_russian" },
	{ "rus",	"s_russian" },
	
	{ "ro",		"Romanian" },
	{ "ron",	"Romanian" },
	{ "rum",	"Romanian" },
	
	{ "scr",	"Serbo-Croatian" },
	
	{ "sr",		"Serbian" },
	{ "srp",	"Serbian" },

	{ "sv",		"s_swedish" },
	{ "sve",	"s_swedish" },
	{ "swe",	"s_swedish" },

	{ "tr",		"s_turkish" },
	{ "tur",	"s_turkish" },

	{ "zh",		"s_chinese" },
	{ "zho",	"s_chinese" },
	{ "chi",	"s_chinese" },

	{ "hrv",	"Croatian" },
};

// ***************************************
//
// 	map_ISO639_code
//
//	maps the first part of an RFC 1766 coded
// 	language "code" to an AVOS internal language name
//
//	if no mapping is found, the original "code"
//	is returned
//
// ***************************************
const char *map_ISO639_code( const char *code )
{
	int 	i;
	const char *c = code;
	static char prim[9];
	char 	*p = prim;
	int 	p_len = 0;
	
	// no first tag
	if( !*c )
		return "";
		
	// from RFC 1766:
	// Language-Tag = Primary-tag *( "-" Subtag )
	// Primary-tag = 1*8ALPHA
	// Subtag = 1*8ALPHA
	//
	// extract the primary tag:
	while( *c && isalpha(*c) && *c != '-' && p_len < 8 ) {
		*p++ = *c++;
		p_len ++;
	}
	
	if( !p_len || (*c != '\0' && *c != '-' && p_len < 8) )
		return code;
		
	*p = '\0';
//serprintf("prim: [%s] ", prim );	

	for ( i = 0; i < sizeof( map ) / sizeof( ISO639 ); i++ ) {
		int map_len = strlen( map[i].iso );
		if( map_len == p_len && !strncasecmp( prim, map[i].iso, p_len ) ) {
//serprintf("name: %s ", map[i].name );	
			return map[i].name;
		} 
	}
	return prim;
}


