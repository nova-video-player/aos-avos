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

#ifndef RECT_H
#define RECT_H

#include "types.h"


typedef struct {
	int x;
	int y;
} Point;

typedef struct {
	int width;
	int height;
} Size;

typedef struct {
	int x;
	int y;
	int width;
	int height;
} Rect;

typedef Rect RECT;
typedef Rect rect;


void Rect_Set( Rect *r, int x, int y, int w, int h );
Rect Rect_FromCoords( int l, int t, int r, int b );
BOOL Rect_IsValid( const Rect *r );
BOOL Rect_IsEmpty( const Rect *r );
BOOL Rect_Contains( const Rect *r, int x, int y );
BOOL Rect_ContainsRect( const Rect *ra, const Rect *rb );
BOOL Rect_Intersects( const Rect *ra, const Rect *rb );
Rect Rect_Intersection( const Rect *ra, const Rect *rb );
Rect Rect_Union( const Rect *ra, const Rect *rb );
Rect Rect_Adjusted( const Rect *r, int dl, int dt, int dr, int db );
BOOL Rect_Equals( const Rect *ra, const Rect *rb );
Rect Rect_Fit( const Rect *ra, const Rect *rb, int par_n, int par_d );
int  Rect_DistSquared( const Rect *r, int x, int y );
void Rect_Dump( const Rect *r, const char *tag );


#endif // RECT_H
