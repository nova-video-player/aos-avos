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

#ifndef _DUMP_H_
#define _DUMP_H_

//------------------------------------------------------------------------
//	DumpLine
//------------------------------------------------------------------------
void DumpLine( unsigned char *buf, int size, int max)
{
	char printable[] = "abcdefghijklmnopqrstuvwxyz!/\\\"#$%&'()*?+,-=|[]ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:^._<>{} ";
	char *c;
	int j;
	for ( j = 0; j < max; j++ ) {
		if ( j < size )
			printf("%02X", buf[j] );		
		else
			printf("  ");		
		if ( ( ( j - 3 ) % 4 ) == 0 ) {
			printf("|");
		} else {
			printf(" ");
		}
	}
	printf(" |");

	for ( j = 0; j < max; j++ ) {
		c = printable;
		while ( *c != ' ' ) {
			if (*c == buf[j] ) {
				break;
			}
			c++;
		}
		if ( j < size )
			printf("%c", *c );
		else
			printf(" ");
	}
	printf("|\r\n");
}

//------------------------------------------------------------------------
//	Dump
//------------------------------------------------------------------------
void Dump( unsigned char *buf , int size)
{
	int i = 0;

	while( i < size ) {
		if ((i % 512) == 0) {
			printf("\r\n[%08X] 00 01 02 03|04 05 06 07|08 09 0A 0B|0C 0D 0E 0F\r\n", (unsigned int)(buf + i) );
			printf("           -----------+-----------+-----------+-----------\r\n" );
		}		
		printf("    [%04X] ", i );
		DumpLine( buf + i, ((size - i) < 16) ? size - i : 16, 16 );
		i += 16;
	}
	printf("\r\n\r\n");
}

#endif
