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

#include <unistd.h>

#include "global.h"
#include "debug.h"
#include "avos_lifetime.h"
#include "types.h"

static AVOS_REG_CALLBACK *_avos_init_reg_callback = NULL;
static AVOS_REG_CALLBACK *_avos_exit_reg_callback = NULL;
static AVOS_REG_CALLBACK *_avos_suspend_reg_callback = NULL;
static AVOS_REG_CALLBACK *_avos_resume_reg_callback  = NULL;
static AVOS_REG_CLEAN_FILE *_avos_reg_clean_file = NULL;

static int _test = 0;

// ************************************************************
//
//	_register_callback
//
// ************************************************************
int _register_callback( AVOS_REG_CALLBACK *reg, AVOS_REG_CALLBACK **_head )
{
	if( !(*_head) ) {
		*_head = reg;
	} else {
		AVOS_REG_CALLBACK *head = *_head;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

// ************************************************************
//
//	avos_register_init_callback
//
// ************************************************************
int avos_register_init_callback( AVOS_REG_CALLBACK *reg )
{
	_register_callback(reg, &_avos_init_reg_callback);

	return 0;
}

// ************************************************************
//
//	avos_register_exit_callback
//
// ************************************************************
int avos_register_exit_callback( AVOS_REG_CALLBACK *reg )
{
	_register_callback(reg, &_avos_exit_reg_callback);

	return 0;
}

// ************************************************************
//
//	avos_register_suspend_callback
//
// ************************************************************
int avos_register_suspend_callback( AVOS_REG_CALLBACK *reg )
{
	_register_callback(reg, &_avos_suspend_reg_callback);

	return 0;
}

// ************************************************************
//
//	avos_register_resume_callback
//
// ************************************************************
int avos_register_resume_callback ( AVOS_REG_CALLBACK *reg )
{
	_register_callback(reg, &_avos_resume_reg_callback);

	return 0;
}

// ************************************************************
//
//	_run_callbacks
//
// ************************************************************
static void _run_callbacks(AVOS_REG_CALLBACK *_head, int runlevel)
{
	AVOS_REG_CALLBACK *head = _head;

	while( head ) {
		if( head->callback && head->runlevel == runlevel ){
			head->callback();
		}
		head = head->next;
	} 
}

// ************************************************************
//
//	avos_init
//
// ************************************************************
void avos_init( int runlevel )
{
	_run_callbacks(_avos_init_reg_callback, runlevel);
}

// ************************************************************
//
//	avos_exit
//
// ************************************************************
void avos_exit( int runlevel )
{
	_run_callbacks(_avos_exit_reg_callback, runlevel);
}

// ************************************************************
//
//	avos_suspend
//
// ************************************************************
void avos_suspend( void )
{
	_run_callbacks(_avos_suspend_reg_callback, AVOS_RUNLEVEL_APP);
}

// ************************************************************
//
//	avos_resume
//
// ************************************************************
void avos_resume( void )
{
	_run_callbacks(_avos_resume_reg_callback, AVOS_RUNLEVEL_APP);
}

static void _test_init(void)
{
	_test = 7;
}

static void _test_exit(void)
{
}

// ************************************************************
//
//	avos_clean_files
//
// ************************************************************
void avos_clean_files( void )
{
	serprintf("avos_clean_files: ");
	AVOS_REG_CLEAN_FILE *head = _avos_reg_clean_file;

	while( head ) {
		if( head->file ){
			serprintf("%s, ", head->file);
			unlink(head->file);
		}
		head = head->next;
	}
	serprintf("\n");
}

// ************************************************************
//
//	avos_register_clean_file
//
// ************************************************************
int avos_register_clean_file( AVOS_REG_CLEAN_FILE *reg )
{
	if( !_avos_reg_clean_file ) {
		_avos_reg_clean_file = reg;
	} else {
		AVOS_REG_CLEAN_FILE *head = _avos_reg_clean_file;
		while( head->next ) {
			head = head->next;
		} 
		head->next = reg;
	}
	reg->next = NULL;
	return 0;
}

AVOS_REGISTER_INIT(_test_init);
AVOS_REGISTER_EXIT(_test_exit);

DECLARE_DEBUG_COMMAND_VOID("avosi", _test_exit);
