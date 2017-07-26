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

#include "types.h"
#include "global.h"
#include "stream_global.h"
#include "debug.h"

#ifdef CONFIG_STREAM

static char *user_agent 	= NULL;
static char *rtsp_user_agent 	= NULL;

// ************************************************************
//
//	stream_global_set_user_agent
//
// ***********************************************************
void stream_global_set_user_agent( char *new_user_agent )
{
	user_agent = new_user_agent;
}

// ************************************************************
//
//	stream_global_get_user_agent
//
// ***********************************************************
char *stream_global_get_user_agent( void )
{
	return user_agent ;
}

// ************************************************************
//
//	stream_global_set_rtsp_user_agent
//
// ***********************************************************
void stream_global_set_rtsp_user_agent( char *new_rtsp_user_agent )
{
	rtsp_user_agent = new_rtsp_user_agent;
}

// ************************************************************
//
//	stream_global_get_rtsp_user_agent
//
// ***********************************************************
char *stream_global_get_rtsp_user_agent( void )
{
	return rtsp_user_agent ;
}

#endif
