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

#ifndef WCHAR_H
#define WCHAR_H

#include <stdlib.h>

typedef unsigned short int wchar;

size_t wstrlen(const wchar *string);

wchar *wstrdup(const wchar *src);

wchar *wstrcpy(register wchar *dest, register const wchar *src);
wchar *c2wstrcpy(register wchar *dest, register const char *src);

wchar *wstrncpy(register wchar *dest, register const wchar *src, register size_t n);
wchar *c2wstrncpy(register wchar *dest, register const char *src, register size_t n);

wchar *wstrcat(wchar *string1, const wchar *string2);
wchar *c2wstrcat(wchar *string1, const char *string2);
wchar *c2wstrncat(wchar *string1, const char *string2, register size_t n);
wchar *wstrncat(wchar *dest, const wchar *src, register size_t n);

wchar *wstrchr(const wchar *string, wchar c);

const char *w2c( const wchar *wide );
#endif
