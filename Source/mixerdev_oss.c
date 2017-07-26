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
#include "types.h"
#include "util.h"

#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>
#include <errno.h>
#include <string.h>

#define DBG  if(Debug[DBG_AUDIODEVICE])
#define DBG2 if(Debug[DBG_AUDIODEVICE] > 1)
#define ERR  if (1)

static int mixer_fd = -1;

// ************************************************************
//
//      mixer_oss_open
//      
// ***********************************************************
void *mixer_oss_open( int mode )
{
DBG serprintf( "OSSMIX_open\r\n" );
	if ( mixer_fd >= 0 ) {
		serprintf( "mixerdev already open\r\n" );
		return NULL;
	}
	
	mixer_fd = open( "/dev/mixer", O_RDWR );
	if ( mixer_fd < 0 ) {
		mixer_fd = open( "/dev/snd/mixer", O_RDWR );
		if ( mixer_fd < 0 ) {
			serprintf( "mixerdev_open failed: %s\r\n", strerror( errno ) );
			return NULL;
		}
	}

	fcntl( mixer_fd, F_SETFD, FD_CLOEXEC );

	return &mixer_fd;
}

// ************************************************************
//
//      mixer_oss_close
//
// ***********************************************************
int mixer_oss_close( void **handle )
{
DBG serprintf( "OSSMIX_close\r\n" );
	if(!handle)
		return 1;
		
	int *fd = *handle;

	if ( *fd >= 0 ) {
		close( *fd );
		*fd = -1;
	}
	*handle = NULL;
	return 0;
}

// ************************************************************
//
//      mixer_oss_set_volume
//
// ***********************************************************
int mixer_oss_set_volume( void *handle, int left, int right )
{
DBG serprintf( "OSSMIX_set_volume(%3d/%3d)\r\n", left, right );
	int vol = ( right << 8 ) | left;

	int *fd = handle;
	if ( ioctl( *fd, MIXER_WRITE( SOUND_MIXER_VOLUME ), &vol ) < 0 ) {
ERR serprintf( "mixer_set_volume failed: %s\r\n", strerror( errno ) );
		return 1;
	}
	return 0;
}
