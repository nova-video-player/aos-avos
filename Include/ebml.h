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

/**
    Copyright (C) 2005  Michael Ahlberg, Måns Rullgaård

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation
    files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use, copy,
    modify, merge, publish, distribute, sublicense, and/or sell copies
    of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
**/
#ifndef _EBML_H
#define _EBML_H

#include "types.h"
#include "stream_read_ctx.h"

typedef struct ebml_header {
    unsigned int ebmlversion;
    unsigned int ebmlreadversion;
    unsigned int ebmlmaxidlength;
    unsigned int ebmlmaxsizelength;
    char        *doctype;
    unsigned int doctypeversion;
    unsigned int doctypereadversion;
} ebml_header_t;

#define EBML_ID_EBML                    0x0a45dfa3
#define EBML_ID_EBMLVERSION             0x0286
#define EBML_ID_EBMLREADVERSION         0x02f7
#define EBML_ID_EBMLMAXIDLENGTH         0x02f2
#define EBML_ID_EBMLMAXSIZELENGTH       0x02f3
#define EBML_ID_DOCTYPE                 0x0282
#define EBML_ID_DOCTYPEVERSION          0x0287
#define EBML_ID_DOCTYPEREADVERSION      0x0285
#define EBML_ID_CRC32                   0x3f
#define EBML_ID_CRC32VALUE              0x02fe
#define EBML_ID_VOID                    0x6c

#define EBML_CB_SUCCESS 0
#define EBML_CB_UNKNOWN 1
#define EBML_CB_ERROR   -1

typedef int (*ebml_element_callback_t)(UINT64 id, UINT64 size, void *);

UINT64 ebml_get_vint(STREAM_READ_CTX *src, int *s);
UINT64 ebml_get_uint(STREAM_READ_CTX *src, int size);
INT64  ebml_get_sint(STREAM_READ_CTX *src, int size);
double ebml_get_float(STREAM_READ_CTX *src, int size);
char *ebml_get_string(STREAM_READ_CTX *src, int size);
UINT8 *ebml_get_binary(STREAM_READ_CTX *src, int size);

int ebml_readheader(STREAM_READ_CTX *src, ebml_header_t *eh);
int ebml_element(STREAM_READ_CTX *src, UINT64 *id, UINT64 *size, UINT64 *ps);
int ebml_read_elements(STREAM_READ_CTX *src, UINT64 size, ebml_element_callback_t, void *cbdata);


#endif
