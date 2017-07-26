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
#include "stream.h"

extern int stream_drive_wake_sleep;

static void _set_platform_specific( void ) __attribute__((constructor));
static void _set_platform_specific( void )
{
	if( 0 ) {
		// on these machines the drive might take longer to wake up!
		stream_drive_wake_sleep = 7000;
	}
}
