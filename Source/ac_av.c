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
#include "app_av.h"
#include "fs.h"
#include "av.h"
#include "util.h"
#include "stream.h"
#include "id3tag.h"

#include <string.h>

typedef struct {
	AV_STATE 	state;
	int		type;
	int		power_veto;
	int		fs;
	ID3_TAG 	tag;
	void		*ctx;
	AV_CALLBACKS	*cbs;
} AV;

static AV av = { 	
	.state          = AV_STOPPED, 
	.type           = 0, 
	.power_veto     = 0, 
	.fs             = DEV_UNAVAIL,
	.tag            = {0},
	.ctx            = NULL,
	.cbs            = NULL
};

// ************************************************
//
//	AV_stop_android_player
//
// ************************************************
void AV_stop_android_player( void )
{
}

// ************************************************
//
//	AV_set_state
//
// ************************************************
void AV_set_state( int state, int type, int fs, void *ctx, AV_CALLBACKS *callbacks )
{
	if( state == AV_STOPPED ) {
		// we can power down
		av.power_veto = 0;
	} else {
		// no power down when we are playing
		av.power_veto = 1;
	}

	av.state = state;
	av.type  = type;
	av.fs    = fs;
	av.ctx   = ctx;
	av.cbs   = callbacks;
	memset( &av.tag, 0, sizeof( av.tag ) );
}

// ************************************************
//
//	AV_set_tag
//
// ************************************************
void AV_set_tag( ID3_TAG *tag )
{
	if( tag ) {
		memcpy( &av.tag, tag, sizeof( av.tag ) );
	} else {
		memset( &av.tag, 0, sizeof( av.tag ) );
	}
}

// ************************************************
//
//	AV_get_state
//
// ************************************************
int AV_get_state( void )
{
	return av.state;
}

// ************************************************
//
//	AV_get_type
//
// ************************************************
int AV_get_type( void )
{
	return av.type;
}

// ************************************************
//
//	AV_get_power_veto
//
// ************************************************
int AV_get_power_veto( void )
{
	return av.power_veto;
}

// ************************************************
//
//	AV_get_ctx
//
// ************************************************
void *AV_get_ctx( void )
{
	return av.ctx;
}

// ************************************************
//
//	AV_get_tag
//
// ************************************************
void AV_get_tag( ID3_TAG *tag )
{
	if( tag ) {
		memcpy( tag, &av.tag, sizeof( av.tag ) );
	}
}

// ************************************************
//
//	AV_stop
//
//	Stop AUDIO or VIDEO playback
//
// ************************************************
void AV_stop( void )
{
serprintf("%s %d\n", __FUNCTION__, AV_get_state());
	if ( AV_get_state() != AV_STOPPED ) {
		if( av.cbs && av.cbs->stop )
			return av.cbs->stop( av.ctx, TRUE );
		AV_set_state( AV_STOPPED, 0, 0, NULL, NULL );
	}
}

// ************************************************
//
//	AV_stop_set_resume
//
// ************************************************
void AV_stop_set_resume( BOOL allow_resuming )
{
serprintf("%s %d\n", __FUNCTION__, AV_get_state());
	if ( AV_get_state() != AV_STOPPED ) {
		if( av.cbs && av.cbs->stop )
			return av.cbs->stop( av.ctx, allow_resuming );
		AV_set_state( AV_STOPPED, 0, 0, NULL, NULL );
	}
}

// ************************************************
//
//	AV_pause
//
//	Pause AUDIO or VIDEO playback
//
// ************************************************
int AV_pause( void )
{
	if( AV_get_state() == AV_PLAYING) {
		if( av.cbs && av.cbs->pause )
			return av.cbs->pause( av.ctx );
	}

	return 0;
}

// ************************************************
//
//	AV_un_pause
//
//	Un-pause AUDIO or VIDEO playback, depending on "was_paused"
//
// ************************************************
void AV_un_pause( int was_paused )
{
	if( AV_get_state() == AV_PLAYING) {
		if( av.cbs && av.cbs->un_pause )
			av.cbs->un_pause( av.ctx, was_paused );
	}
}

// ************************************************
//
//	AV_seek
//
// ************************************************
int AV_seek( int time, int pos )
{
	if( AV_get_state() == AV_PLAYING) {
		if( av.cbs && av.cbs->seek )
			return av.cbs->seek( av.ctx, time, pos );
	}

	return 0;
}

// ************************************************
//
//	AV_is_paused
//
//	Returns AUDIO or VIDEO pause status
//
// ************************************************
int AV_is_paused( void )
{
	if( AV_get_state() == AV_PLAYING) {
		if( av.cbs && av.cbs->is_paused )
			return av.cbs->is_paused( av.ctx );
	}

	return 0;
}

// ************************************************
//
//	AV_get_fs
//
//	get the filesystem of AUDIO or VIDEO playback
//
// ************************************************
int AV_get_fs( void )
{
	if( AV_get_state() == AV_PLAYING) {
		return av.fs;
	}

	return DEV_UNAVAIL;
}

// ************************************************
//
//	AV_get_current_time
//
//	get the current and total time of AUDIO or VIDEO playback
//
// ************************************************
int AV_get_current_time( int *total )
{
	if( AV_get_state() == AV_PLAYING) {
		if( av.cbs && av.cbs->get_current_time )
			return av.cbs->get_current_time( av.ctx, total );
	}

	if( total )
		*total = 0;
	return 0;
}

// ************************************************
//
//	AV_get_current_path
//
//	get the current path and BROWSE_ENTRY
//
// ************************************************
const char *AV_get_current_path( BROWSE_ENTRY *br )
{
	if( AV_get_state() == AV_PLAYING) {
		if( av.cbs && av.cbs->get_current_path )
			return av.cbs->get_current_path( av.ctx, br );
	}

	return NULL;
}

#ifdef DEBUG_MSG
static void _get_time( void )
{
	int total;
	int time = AV_get_current_time( &total );
	
	serprintf("AV: %d/%d\r\n", time, total );
}

void _pause( void )
{
	if ( AV_is_paused() ) {
		AV_un_pause( 0 );
	} else {
		AV_pause();
	}
}

DECLARE_DEBUG_COMMAND_VOID("avs", AV_stop );
DECLARE_DEBUG_COMMAND_VOID("avp", _pause );
DECLARE_DEBUG_COMMAND_VOID("avt", _get_time );

#endif
