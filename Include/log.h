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

/* ****************************

 Defines for DEBUG options

 ****************************
*/
#ifndef _LOG_H_
#define _LOG_H_

#include "global.h"
#include <stdarg.h>

#ifdef DEBUG_MSG
void LOG_open (void);
void LOG_open_name (const char *name);
void LOG_close(void);
int serprintf (const char *fmt, ...)  /*__attribute__ ((format (printf, 1, 2)))*/;
int vserprintf(const char *fmt, va_list va );
#else
static inline void LOG_open (void) {}
static inline void LOG_open_name (const char *name) {};
static inline void LOG_close(void) {}
static inline int  serprintf (const char *fmt, ...) { return 0; }
static inline int  vserprintf(const char *fmt, va_list va ) { return 0; }
#endif

#endif // _LOG_H_

