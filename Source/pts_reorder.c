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

#include "astdlib.h"
#include "debug.h"

int pts_force_reorder = 0;

#ifndef STANDALONE
#ifdef DEBUG_MSG
DECLARE_DEBUG_TOGGLE("ptfr", pts_force_reorder );
#endif
#endif

typedef struct PTS_RO_CTX {
	int *ts;
	int num;
	int run;
} PTS_RO_CTX;

void pts_ro_init( void *_c )
{
	PTS_RO_CTX *c = (PTS_RO_CTX*)_c;
	
	c->num = 0;
}

void pts_ro_run( void *_c )
{
	PTS_RO_CTX *c = (PTS_RO_CTX*)_c;
	
	c->run = 1;
}

void pts_ro_stop( void *_c )
{
	PTS_RO_CTX *c = (PTS_RO_CTX*)_c;
	
	c->run = 0;
}

void *pts_ro_alloc( int count )
{
	PTS_RO_CTX *c = acalloc( sizeof(PTS_RO_CTX), 1 );
	c->ts = amalloc( sizeof(int) * count );
	
	pts_ro_init( c );
	
	return c;
}

void pts_ro_free( void *_c )
{
	PTS_RO_CTX *c = (PTS_RO_CTX*)_c;

	if( c ) {
		afree(c->ts);
		afree(c);
	}
}

void pts_ro_put( void *_c, int ts )
{
	PTS_RO_CTX *c = (PTS_RO_CTX*)_c;

	if(!c->run)
		return;

	int i = 0;
	if( c->num ) {
		for( i = 0; i < c->num; i++ ) {
			if( ts < c->ts[i] ) {
				// insert
//serprintf("insert at %d  count %d\n", i, c->num - i );
				memmove( &(c->ts[i + 1]), &(c->ts[i]), sizeof(int) * (c->num - i) );
				break;
			}
		}
	}
	c->ts[i] = ts;
	c->num ++;
/*	
	printf("%8d -> RO[%4d]", ts, c->num );
	for( i = 0; i < c->num; i++ ) {
		printf("  %d", c->ts[i]);
	}
	printf("\n");
*/	
}

int pts_ro_get( void *_c)
{
	PTS_RO_CTX *c = (PTS_RO_CTX*)_c;

	if(!c->run)
		return -1;

	int ts = c->ts[0];
	
	memmove( &(c->ts[0]), &(c->ts[1]), sizeof(int) * (c->num - 1)  );
	c->num--; 
	
	return ts;
}
