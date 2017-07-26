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

#include "rect.h"
#include "debug.h"
#include "util.h"


Rect Rect_FromCoords( int l, int t, int r, int b )
{
	return (Rect) {
		l, t, 
		r - l + 1, 
		b - t + 1
	};
}

void Rect_Set( Rect *r, int x, int y, int w, int h )
{
	r->x = x;
	r->y = y;
	r->width = w;
	r->height = h;
}

BOOL Rect_IsValid( const Rect *r )
{
	return	r->width  > 0 &&
		r->height > 0;
}

BOOL Rect_IsEmpty( const Rect *r )
{
	return r->width == 0 ||
	       r->height == 0;
}

BOOL Rect_Contains( const Rect *r, int x, int y )
{
	return	x >= r->x && 
		y >= r->y && 
		x < r->x + r->width && 
		y < r->y + r->height;
}

// returns the square of the distance between r and the point (x, y)
int Rect_DistSquared( const Rect *r, int x, int y )
{
	int tr = r->x + r->width  - 1;
	int tb = r->y + r->height - 1;

	int hdist = 0;
	if(x < r->x) {
		hdist = r->x - x;
	}
	else if(x > tr) {
		hdist = x - tr;
	}

	int vdist = 0;
	if(y < r->y) {
		vdist = r->y - y;
	}
	else if(y > tb) {
		vdist = y - tb;
	}

	return hdist * hdist + vdist * vdist;
}

BOOL Rect_Intersects( const Rect *ra, const Rect *rb )
{
	return	rb->x < ra->x + ra->width &&
		rb->y < ra->y + ra->height &&
		rb->x + rb->width  > ra->x &&
		rb->y + rb->height > ra->y;
}

Rect Rect_Intersection( const Rect *ra, const Rect *rb )
{
	int l= MAX( ra->x, rb->x );
	int t= MAX( ra->y, rb->y );
	int r= MIN( ra->x + ra->width,  rb->x + rb->width  );
	int b= MIN( ra->y + ra->height, rb->y + rb->height );

	if ( l > r || t > b ) {
		// Inconsistent values => intersection is empty
		return (Rect) { 0, 0, 0, 0 };
	}
	return (Rect) { l, t, r - l, b - t };
}

Rect Rect_Union( const Rect *ra, const Rect *rb )
{
	if( !Rect_IsValid( ra ) ) {
		return *rb;
	} else if( !Rect_IsValid( rb ) ) {
		return *ra;
	} else {
		int l= MIN( ra->x, rb->x );
		int t= MIN( ra->y, rb->y );
		int r= MAX( ra->x + ra->width,  rb->x + rb->width  );
		int b= MAX( ra->y + ra->height, rb->y + rb->height );
		
		return (Rect) { l, t, r - l, b - t };
	}
}

Rect Rect_Adjusted( const Rect *r, int dl, int dt, int dr, int db )
{
	return Rect_FromCoords ( 
		r->x + dl, 
		r->y + dt, 
		r->x + r->width  - 1 + dr, 
		r->y + r->height - 1 + db
	);
}

BOOL Rect_ContainsRect( const Rect *ra, const Rect *rb )
{
	return Rect_Contains( ra, rb->x, rb->y )
		&& Rect_Contains(ra, rb->x + rb->width - 1, rb->y + rb->height -1 );
}

BOOL Rect_Equals( const Rect *ra, const Rect *rb )
{
	return	rb->x      == ra->x &&
		rb->y      == ra->y &&
		rb->width  == ra->width &&
		rb->height == ra->height;
}

void Rect_Dump( const Rect *r, const char *tag )
{
serprintf("RECT[%s] %4dx%3d  WxH %4dx%3d\r\n", tag, r->x, r->y, r->width, r->height );
}

Rect Rect_Fit( const Rect *ra, const Rect *rb, int par_n, int par_d )
{
	Rect area;

	if( ra->width * rb->height * par_d > rb->width * ra->height * par_n){
		// Image wider than destination
		area.width  = rb->width;
		area.height = area.width * ra->height * par_n / ( ra->width * par_d );

		area.x = 0;
		area.y = ( rb->height - area.height ) / 2;
		
		// Round to 2-pixels boundary
		area.x = area.x & ~1;
		area.width = area.width & ~1;

	} else {
		// Image higher than destination
		area.height = rb->height;
		area.width  = rb->height * ra->width * par_d / ( ra->height * par_n );
		
		area.x = (( rb->width - area.width ) / 2);
		area.y = 0;

		// Round to 2-pixels boundary
		area.x = area.x & ~1;
		area.width = area.width & ~1;
	}

	return area;
}
