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

#ifndef _XDM_UTILS_H
#define _XDM_UTILS_H

#define ID_MAX 32
typedef struct {
	int id;
	int type;
	int time;
} ITF;

#define TS_MAX 32

struct XDM_ctx {
	ITF itf[ID_MAX];
	int id_now;
	int ts[TS_MAX];
	int ts_write;
	int ts_read;
};

void XDM_id_flush( struct XDM_ctx *ctx );
void XDM_id_put( struct XDM_ctx *ctx, int id, int type, int time );
int  XDM_id_get( struct XDM_ctx *ctx, int id, int *type, int *time );
void XDM_ts_put( struct XDM_ctx *ctx, int time );
int  XDM_ts_get( struct XDM_ctx *ctx );
void XDM_ts_flush( struct XDM_ctx *ctx );

#endif
