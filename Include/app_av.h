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

#ifndef _APP_AV_H
#define _APP_AV_H

#include "browse.h"
#include "id3tag.h"

#define MAX_PATH_LEN 512

typedef enum {
	AV_STOPPED = 0,
	AV_PLAYING,
} AV_STATE;

typedef void (*AV_STOP_CB)     (void *ctx, int);
typedef int  (*AV_PAUSE_CB)    (void *ctx);
typedef void (*AV_UN_PAUSE_CB) (void *ctx, int);
typedef int  (*AV_IS_PAUSED_CB)(void *ctx);
typedef int  (*AV_GET_TIME_CB) (void *ctx, int*);
typedef const char *(*AV_GET_PATH) ( void *ctx, BROWSE_ENTRY *br ); 
typedef int  (*AV_SEEK_CB) (void *ctx, int time, int pos);

typedef struct {
	AV_STOP_CB	stop;
	AV_PAUSE_CB	pause;
	AV_UN_PAUSE_CB	un_pause;
	AV_IS_PAUSED_CB	is_paused;
	AV_GET_TIME_CB  get_current_time;
	AV_GET_PATH	get_current_path; 
	AV_SEEK_CB	seek;
} AV_CALLBACKS;

void AV_set_state( int state, int type, int fs, void *ctx, AV_CALLBACKS *callbacks );
void AV_set_tag  ( ID3_TAG *tag );
int  AV_get_state( void );
int  AV_get_type ( void );
void AV_get_tag  ( ID3_TAG *tag );
int  AV_get_power_veto( void );
void *AV_get_ctx ( void );
void AV_stop     ( void );
void AV_stop_set_resume( BOOL allow_resuming );
int  AV_pause    ( void );
void AV_un_pause ( int was_paused );
int  AV_is_paused( void );
int  AV_get_fs   ( void );
int  AV_get_current_time( int *total );
int AV_seek( int time, int pos );
const char *AV_get_current_path( BROWSE_ENTRY *br );
void AV_stop_android_player( void );

#endif	// _APP_AV_H
