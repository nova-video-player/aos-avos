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
#include "astdlib.h"
#include "awchar.h"

#include <stddef.h>

size_t wstrlen(const wchar *string)
{
   size_t       n = (size_t)-1;
   const wchar *s = string - 1;

   do n++; while (*++s != 0x0000);
   return n;
}

wchar *wstrdup(const wchar *src)
{
	int len = wstrlen(src);
	wchar *dest = amalloc((len + 1) * sizeof(wchar));
	if (!dest) {
		return NULL;
	}
	return wstrcpy(dest, src);
}

wchar *wstrcpy(register wchar *dest, register const wchar *src)
{
     register wchar       *d = dest - 1;     
     register const wchar *s = src  - 1;     

     while ((*++d = *++s)!=0x0000);
     return dest;
}

wchar *c2wstrcpy(register wchar *dest, register const char *src)
{
     register wchar       *d = dest - 1;     
     register const char *s = src  - 1;     

     while ((*++d = (wchar)*++s)!=0x0000);
     return dest;
}

wchar *wstrncpy(register wchar *dest,
		     register const wchar *src,
		     register size_t n)
{
     if (n) 
     {
	 register wchar       *d = dest - 1;
	 register const wchar *s = src - 1;
	 while ((*++d = *++s)!=0x0000 && --n);              /* COPY STRING         */
	 if (n-- > 1) do *++d = 0x0000; while (--n);  /* TERMINATION PADDING */
     }
     return dest;
}

wchar *c2wstrncpy(register wchar *dest,
		     register const char *src,
		     register size_t n)
{
     if (n) 
     {
	 register wchar       *d = dest - 1;
	 register const char *s = src - 1;
	 while ((*++d = (wchar)*++s)!=0x0000 && --n);              /* COPY STRING         */
	 if (n-- > 1) do *++d = 0x0000; while (--n);  /* TERMINATION PADDING */
     }
     return dest;
}

wchar *wstrcat(wchar *string1, const wchar *string2)
{
   wchar       *s1 = string1 - 1;
   const wchar *s2 = string2 - 1;

   while (*++s1!=0x0000);		     /* FIND END OF STRING   */
   s1--;  			     /* BACK UP OVER NULL    */
   while ((*++s1 = *++s2)!=0x0000);	     /* APPEND SECOND STRING */
   return string1;
}

wchar *c2wstrcat(wchar *string1, const char *string2)
{
   wchar       *s1 = string1 - 1;
   const char *s2 = string2 - 1;

   while (*++s1!=0x0000);		     /* FIND END OF STRING   */
   s1--;  			     /* BACK UP OVER NULL    */
   while ((*++s1 = (wchar)*++s2)!=0x0000);	     /* APPEND SECOND STRING */
   return string1;
}

wchar *c2wstrncat(wchar *string1, const char *string2, register size_t n)
{
	if (n) {
		wchar       *s1 = string1 - 1;
		const char *s2 = string2 - 1;

		while (*++s1!=0x0000);		     /* FIND END OF STRING   */
		s1--; 			     /* BACK UP OVER NULL    */
		while (n--)
		  if ((*++s1= (wchar)*++s2)==0x0000) return s1; /* APPEND SECOND STRING */
		*++s1= 0x0000;
	}
	return string1;
}

wchar *wstrncat(wchar *dest, const wchar *src, register size_t n)
{
    if (n)
    {
	wchar       *d = dest - 1;
	const wchar *s = src  - 1;

	while (*++d!=0x0000);                      /* FIND END OF STRING   */
	d--;                               /* BACK UP OVER NULL    */

	while (n--)
	  if ((*++d = *++s)==0x0000) return dest; /* APPEND SECOND STRING */
	*++d = 0x0000;
    }
    return dest;
}

wchar *wstrchr(const wchar *string, wchar c)
{
   wchar        tch, ch  = c;
   const wchar *s        = string - 1;

   for (;;)
   {
       if ((tch = *++s) == ch) return (wchar *) s;
       if (!tch)               return (wchar *) 0;
   }
}

const char *w2c( const wchar *wide )
{
	static char out[256 + 1];
	char *c = out;
	int len = 0;
	
	while( *wide && len++ < 256 )
		*c++ = *wide++;
	*c = 0;
	return out;
}
