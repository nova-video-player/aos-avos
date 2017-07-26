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
// ISO 3166 country codes
//
// ***************************************
#include "global.h"
#include "types.h"
#include "debug.h"
#include "util.h"

#include <string.h>

typedef struct ISO3166 {
	const char *name;
	const char iso[3];
} ISO3166;

static const ISO3166 map[] = {
	{ "AFGHANISTAN", "AF" },
	{ "ALAND ISLANDS", "AX" },
	{ "ALBANIA", "AL" },
	{ "ALGERIA", "DZ" },
	{ "AMERICAN SAMOA", "AS" },
	{ "ANDORRA", "AD" },
	{ "ANGOLA", "AO" },
	{ "ANGUILLA", "AI" },
	{ "ANTARCTICA", "AQ" },
	{ "ANTIGUA AND BARBUDA", "AG" },
	{ "ARGENTINA", "AR" },
	{ "ARMENIA", "AM" },
	{ "ARUBA", "AW" },
	{ "AUSTRALIA", "AU" },
	{ "AUSTRIA", "AT" },
	{ "AZERBAIJAN", "AZ" },
	{ "BAHAMAS", "BS" },
	{ "BAHRAIN", "BH" },
	{ "BANGLADESH", "BD" },
	{ "BARBADOS", "BB" },
	{ "BELARUS", "BY" },
	{ "BELGIUM", "BE" },
	{ "BELIZE", "BZ" },
	{ "BENIN", "BJ" },
	{ "BERMUDA", "BM" },
	{ "BHUTAN", "BT" },
	{ "BOLIVIA", "BO" },
	{ "BOSNIA AND HERZEGOVINA", "BA" },
	{ "BOTSWANA", "BW" },
	{ "BOUVET ISLAND", "BV" },
	{ "BRAZIL", "BR" },
	{ "BRITISH INDIAN OCEAN TERRITORY", "IO" },
	{ "BRUNEI DARUSSALAM", "BN" },
	{ "BULGARIA", "BG" },
	{ "BURKINA FASO", "BF" },
	{ "BURUNDI", "BI" },
	{ "CAMBODIA", "KH" },
	{ "CAMEROON", "CM" },
	{ "CANADA", "CA" },
	{ "CAPE VERDE", "CV" },
	{ "CAYMAN ISLANDS", "KY" },
	{ "CENTRAL AFRICAN REPUBLIC", "CF" },
	{ "CHAD", "TD" },
	{ "CHILE", "CL" },
	{ "CHINA", "CN" },
	{ "CHRISTMAS ISLAND", "CX" },
	{ "COCOS (KEELING) ISLANDS", "CC" },
	{ "COLOMBIA", "CO" },
	{ "COMOROS", "KM" },
	{ "CONGO", "CG" },
	{ "CONGO, THE DEMOCRATIC REPUBLIC OF THE", "CD" },
	{ "COOK ISLANDS", "CK" },
	{ "COSTA RICA", "CR" },
	{ "COTE D'IVOIRE", "CI" },
	{ "CROATIA", "HR" },
	{ "CUBA", "CU" },
	{ "CYPRUS", "CY" },
	{ "CZECH REPUBLIC", "CZ" },
	{ "DENMARK", "DK" },
	{ "DJIBOUTI", "DJ" },
	{ "DOMINICA", "DM" },
	{ "DOMINICAN REPUBLIC", "DO" },
	{ "ECUADOR", "EC" },
	{ "EGYPT", "EG" },
	{ "EL SALVADOR", "SV" },
	{ "EQUATORIAL GUINEA", "GQ" },
	{ "ERITREA", "ER" },
	{ "ESTONIA", "EE" },
	{ "ETHIOPIA", "ET" },
	{ "FALKLAND ISLANDS (MALVINAS)", "FK" },
	{ "FAROE ISLANDS", "FO" },
	{ "FIJI", "FJ" },
	{ "FINLAND", "FI" },
	{ "FRANCE", "FR" },
	{ "FRENCH GUIANA", "GF" },
	{ "FRENCH POLYNESIA", "PF" },
	{ "FRENCH SOUTHERN TERRITORIES", "TF" },
	{ "GABON", "GA" },
	{ "GAMBIA", "GM" },
	{ "GEORGIA", "GE" },
	{ "GERMANY", "DE" },
	{ "GHANA", "GH" },
	{ "GIBRALTAR", "GI" },
	{ "GREECE", "GR" },
	{ "GREENLAND", "GL" },
	{ "GRENADA", "GD" },
	{ "GUADELOUPE", "GP" },
	{ "GUAM", "GU" },
	{ "GUATEMALA", "GT" },
	{ "GUERNSEY", "GG" },
	{ "GUINEA", "GN" },
	{ "GUINEA-BISSAU", "GW" },
	{ "GUYANA", "GY" },
	{ "HAITI", "HT" },
	{ "HEARD ISLAND AND MCDONALD ISLANDS", "HM" },
	{ "HOLY SEE (VATICAN CITY STATE)", "VA" },
	{ "HONDURAS", "HN" },
	{ "HONG KONG", "HK" },
	{ "HUNGARY", "HU" },
	{ "ICELAND", "IS" },
	{ "INDIA", "IN" },
	{ "INDONESIA", "ID" },
	{ "IRAN, ISLAMIC REPUBLIC OF", "IR" },
	{ "IRAQ", "IQ" },
	{ "IRELAND", "IE" },
	{ "ISLE OF MAN", "IM" },
	{ "ISRAEL", "IL" },
	{ "ITALY", "IT" },
	{ "JAMAICA", "JM" },
	{ "JAPAN", "JP" },
	{ "JERSEY", "JE" },
	{ "JORDAN", "JO" },
	{ "KAZAKHSTAN", "KZ" },
	{ "KENYA", "KE" },
	{ "KIRIBATI", "KI" },
	{ "KOREA, DEMOCRATIC PEOPLE'S REPUBLIC OF", "KP" },
	{ "KOREA, REPUBLIC OF", "KR" },
	{ "KUWAIT", "KW" },
	{ "KYRGYZSTAN", "KG" },
	{ "LAO PEOPLE'S DEMOCRATIC REPUBLIC", "LA" },
	{ "LATVIA", "LV" },
	{ "LEBANON", "LB" },
	{ "LESOTHO", "LS" },
	{ "LIBERIA", "LR" },
	{ "LIBYAN ARAB JAMAHIRIYA", "LY" },
	{ "LIECHTENSTEIN", "LI" },
	{ "LITHUANIA", "LT" },
	{ "LUXEMBOURG", "LU" },
	{ "MACAO", "MO" },
	{ "MACEDONIA, THE FORMER YUGOSLAV REPUBLIC OF", "MK" },
	{ "MADAGASCAR", "MG" },
	{ "MALAWI", "MW" },
	{ "MALAYSIA", "MY" },
	{ "MALDIVES", "MV" },
	{ "MALI", "ML" },
	{ "MALTA", "MT" },
	{ "MARSHALL ISLANDS", "MH" },
	{ "MARTINIQUE", "MQ" },
	{ "MAURITANIA", "MR" },
	{ "MAURITIUS", "MU" },
	{ "MAYOTTE", "YT" },
	{ "MEXICO", "MX" },
	{ "MICRONESIA, FEDERATED STATES OF", "FM" },
	{ "MOLDOVA, REPUBLIC OF", "MD" },
	{ "MONACO", "MC" },
	{ "MONGOLIA", "MN" },
	{ "MONTSERRAT", "MS" },
	{ "MOROCCO", "MA" },
	{ "MOZAMBIQUE", "MZ" },
	{ "MYANMAR", "MM" },
	{ "NAMIBIA", "NA" },
	{ "NAURU", "NR" },
	{ "NEPAL", "NP" },
	{ "NETHERLANDS", "NL" },
	{ "NETHERLANDS ANTILLES", "AN" },
	{ "NEW CALEDONIA", "NC" },
	{ "NEW ZEALAND", "NZ" },
	{ "NICARAGUA", "NI" },
	{ "NIGER", "NE" },
	{ "NIGERIA", "NG" },
	{ "NIUE", "NU" },
	{ "NORFOLK ISLAND", "NF" },
	{ "NORTHERN MARIANA ISLANDS", "MP" },
	{ "NORWAY", "NO" },
	{ "OMAN", "OM" },
	{ "PAKISTAN", "PK" },
	{ "PALAU", "PW" },
	{ "PALESTINIAN TERRITORY, OCCUPIED", "PS" },
	{ "PANAMA", "PA" },
	{ "PAPUA NEW GUINEA", "PG" },
	{ "PARAGUAY", "PY" },
	{ "PERU", "PE" },
	{ "PHILIPPINES", "PH" },
	{ "PITCAIRN", "PN" },
	{ "POLAND", "PL" },
	{ "PORTUGAL", "PT" },
	{ "PUERTO RICO", "PR" },
	{ "QATAR", "QA" },
	{ "REUNION", "RE" },
	{ "ROMANIA", "RO" },
	{ "RUSSIAN FEDERATION", "RU" },
	{ "RWANDA", "RW" },
	{ "SAINT HELENA", "SH" },
	{ "SAINT KITTS AND NEVIS", "KN" },
	{ "SAINT LUCIA", "LC" },
	{ "SAINT PIERRE AND MIQUELON", "PM" },
	{ "SAINT VINCENT AND THE GRENADINES", "VC" },
	{ "SAMOA", "WS" },
	{ "SAN MARINO", "SM" },
	{ "SAO TOME AND PRINCIPE", "ST" },
	{ "SAUDI ARABIA", "SA" },
	{ "SENEGAL", "SN" },
	{ "SERBIA AND MONTENEGRO", "CS" },
	{ "SEYCHELLES", "SC" },
	{ "SIERRA LEONE", "SL" },
	{ "SINGAPORE", "SG" },
	{ "SLOVAKIA", "SK" },
	{ "SLOVENIA", "SI" },
	{ "SOLOMON ISLANDS", "SB" },
	{ "SOMALIA", "SO" },
	{ "SOUTH AFRICA", "ZA" },
	{ "SOUTH GEORGIA AND THE SOUTH SANDWICH ISLANDS", "GS" },
	{ "SPAIN", "ES" },
	{ "SRI LANKA", "LK" },
	{ "SUDAN", "SD" },
	{ "SURINAME", "SR" },
	{ "SVALBARD AND JAN MAYEN", "SJ" },
	{ "SWAZILAND", "SZ" },
	{ "SWEDEN", "SE" },
	{ "SWITZERLAND", "CH" },
	{ "SYRIAN ARAB REPUBLIC", "SY" },
	{ "TAIWAN, POC", "TW" },
	{ "TAJIKISTAN", "TJ" },
	{ "TANZANIA, UNITED REPUBLIC OF", "TZ" },
	{ "THAILAND", "TH" },
	{ "TIMOR-LESTE", "TL" },
	{ "TOGO", "TG" },
	{ "TOKELAU", "TK" },
	{ "TONGA", "TO" },
	{ "TRINIDAD AND TOBAGO", "TT" },
	{ "TUNISIA", "TN" },
	{ "TURKEY", "TR" },
	{ "TURKMENISTAN", "TM" },
	{ "TURKS AND CAICOS ISLANDS", "TC" },
	{ "TUVALU", "TV" },
	{ "UGANDA", "UG" },
	{ "UKRAINE", "UA" },
	{ "UNITED ARAB EMIRATES", "AE" },
	{ "UNITED KINGDOM", "GB" },
	{ "UNITED STATES", "US" },
	{ "UNITED STATES MINOR OUTLYING ISLANDS", "UM" },
	{ "URUGUAY", "UY" },
	{ "UZBEKISTAN", "UZ" },
	{ "VANUATU", "VU" },
	{ "VENEZUELA", "VE" },
	{ "VIET NAM", "VN" },
	{ "VIRGIN ISLANDS, BRITISH", "VG" },
	{ "VIRGIN ISLANDS, U.S.", "VI" },
	{ "WALLIS AND FUTUNA", "WF" },
	{ "WESTERN SAHARA", "EH" },
	{ "YEMEN", "YE" },
	{ "ZAMBIA", "ZM" },
	{ "ZIMBABWE", "ZW" },
};

// ***************************************
//
// 	map_ISO3166_code
//
//	maps the second part of an RFC 1766 coded
// 	language "code" to an AVOS internal country name
//
//	if no mapping is found, the original second part is returned
//
// ***************************************
const char *map_ISO3166_code( const char *code )
{
	int 	i;
	const char *c = code;
	static char tag[9];
	char 	*t = tag;
	int 	t_len = 0;
	
	// from RFC 1766:
	// Language-Tag = Primary-tag *( "-" Subtag )
	// Primary-tag = 1*8ALPHA
	// Subtag = 1*8ALPHA

	// find the first tag:
	while( *c && *c != '-' ) {
		c++;
	}
	
	// no second part
	if( !*c )
		return "";
	
	c++;	
	// extract the seconds tag:
	while( *c && *c != '-' && t_len < 8 ) {
		*t++ = *c++;
		t_len ++;
	}
	
	if( !t_len )
		return "";
	
	*t = '\0';
// serprintf("tag: %s ", tag );	

	for ( i = 0; i < sizeof( map ) / sizeof( ISO3166 ); i++ ) {
		int map_len = strlen( map[i].iso );
		if( map_len == t_len && !strncmpNC( tag, map[i].iso, t_len ) ) {
// serprintf("name: %s ", map[i].name );	
			return map[i].name;
		} 
	}
	return tag;
}


// ***************************************
//
// 	const char *map_ISO3166_getCountryFromCode( const char *P_ptCode, INT32 * P_ptIndex )
//
//	function to get array index in map table and pointer on full length country name from country code (FR)
//
//	param P_ptCode input, pointer on the (string) code to find.
//	param P_ptIndex ouput, set to index if code is found, don't change if not found.
//
//	ret NULL if no mapping is found
//
// ***************************************
const char *map_ISO3166_getCountryFromCode( const char *P_ptCode, int * P_ptIndex )
{
	INT32 i;
	
	int map_len = strlen( map[0].iso );
	const ISO3166 * ptMap = map; // local pointer to speed up function (to not use array index [])
	
	for ( i = 0 ; i < (sizeof( map ) / sizeof( ISO3166 )) ; i++ ) {
		if( !strncmpNC( P_ptCode, ptMap->iso, map_len) ) {
			//set index
			*P_ptIndex = i;
			return ptMap->name;
		} 
		//go to next item
		ptMap++;
	}

	return NULL;
}

// ***************************************
//
// 	int32 map_ISO3166_getLastIndex( void )
//
//	function to get number of items in map table
//
//	ret number of items /size
//
// ***************************************
INT32 map_ISO3166_getCountryListSize( void )
{
	return (sizeof( map ) / sizeof( ISO3166 ));
}

// ***************************************
//
// 	void map_ISO3166_getCountryFromIndex( INT32 P_Index, const char ** P_ptptCode, const char ** P_ptptName)
//
//	function to get country code and name pointer from index
//	Do nothing if incorrect index 
//
//	param P_Index input, country index in map table 
//	param P_ptptCode output, adress of the pointer where the adress of the country code string corresponding to index will be set. (set an external function pointer)
//	param P_ptptName output, adress of the pointer where the adress of the country name string corresponding to index will be set. (set an external function pointer)
//
// ***************************************
void map_ISO3166_getCountryFromIndex( INT32 P_Index, const char ** P_ptptCode, const char ** P_ptptName)
{
	// check index before use
	if((sizeof( map ) / sizeof( ISO3166 ))>P_Index)
	{
		*P_ptptCode = map[P_Index].iso;
		*P_ptptName = map[P_Index].name; 
	}
}

