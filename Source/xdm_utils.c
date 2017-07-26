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

#include "debug.h"
#include "av.h"
#include "xdm_utils.h"

void XDM_id_flush( struct XDM_ctx *ctx )
{
	int i;
	for( i = 0; i < ID_MAX; i++ ) {
		ctx->itf[i].id   = -1;
		ctx->itf[i].type = -1;
		ctx->itf[i].time = -1;
	}
	ctx->id_now = 0;
}

void XDM_id_put( struct XDM_ctx *ctx, int id, int type, int time )
{
	ctx->itf[ctx->id_now].id   = id;
	ctx->itf[ctx->id_now].type = type;
	ctx->itf[ctx->id_now].time = time;
	
	ctx->id_now++;
	if( ctx->id_now == ID_MAX )
		ctx->id_now = 0;
}

int XDM_id_get( struct XDM_ctx *ctx, int id, int *type, int *time )
{
	int idx = ctx->id_now - 1;
	if( idx < 0 ) 
		idx = ID_MAX - 1;

	while( idx != ctx->id_now ) {
		if( ctx->itf[idx].id == id ) {
			if( type )
				*type = ctx->itf[idx].type;
			if( time )
				*time = ctx->itf[idx].time;
			return 0;	
		}
		idx--;
		if( idx < 0 ) 
			idx = ID_MAX - 1;
	}
	
	if( type )
		*type = -1;
	if( time )
		*time = -1;
	
	return 1;
}

void XDM_ts_put( struct XDM_ctx *ctx, int time )
{
	int free = ctx->ts_read - ctx->ts_write;
	if( free <= 0 )
		free += TS_MAX;
	free -= 1;
	if( free <= 0 )
		return;

	ctx->ts[ctx->ts_write] = time;
	
	ctx->ts_write++;
	if( ctx->ts_write == TS_MAX )
		ctx->ts_write = 0;
}

int XDM_ts_get( struct XDM_ctx *ctx )
{
	if( ctx->ts_read == ctx->ts_write )
		return -1;
		
	int time = ctx->ts[ctx->ts_read];
	
	ctx->ts_read++;
	if( ctx->ts_read == TS_MAX )
		ctx->ts_read = 0;
	
	return time;
}

void XDM_ts_flush( struct XDM_ctx *ctx )
{
	ctx->ts_read  = 0;
	ctx->ts_write = 0;
}
