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

#ifndef _I18N_H
#define _I18N_H

#include "awchar.h"

int  I18N_load( void );
int  I18N_unload( void );
int  I18N_codepage_supported( int codepage );
int  I18N_set_codepage( int codepage );
int  I18N_get_codepage( void );
int  I18N_codepage_to_unicode( const unsigned char *t, wchar *uc );
void I18N_codepage_to_utf8( char *utf8, const char *cp, int max );

void *I18N_check_encoding_init ( void );
void I18N_check_encoding_update( void *ctx, const unsigned char *text, int size );
void I18N_check_encoding_finish( void *ctx, int *utf8 );

#endif
