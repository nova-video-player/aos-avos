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

#ifndef _AVOS_LIFETIME_H_
#define _AVOS_LIFETIME_H_

void avos_init( int runlevel );
void avos_exit( int runlevel );
void avos_suspend( void );
void avos_resume ( void );
void avos_clean_files( void );

enum {
	AVOS_RUNLEVEL_PLATFORM,	// <- In this runlevel, you can rely on :
				//      * nothing !
				//    Put init methods that setup virtual methods here.

	AVOS_RUNLEVEL_BC,	// <- In this runlevel, you can rely on :
				//      * virtual methods defined
				//    Put init methods that setup broadcasters here.

	AVOS_RUNLEVEL_APP,	// <- In this runlevel, you can rely on :
				//      * audio
				//      * timers
				//    Put init methods here by default.
};

typedef void (*AVOS_CALLBACK)(void);

struct AVOS_REG_CALLBACK;

typedef struct AVOS_REG_CALLBACK {
	AVOS_CALLBACK 			callback;
	int 				runlevel;

	struct AVOS_REG_CALLBACK 	*next;
} AVOS_REG_CALLBACK;

struct AVOS_REG_CLEAN_FILE;

typedef struct AVOS_REG_CLEAN_FILE {
	const char			*file;
	struct AVOS_REG_CLEAN_FILE	*next;
} AVOS_REG_CLEAN_FILE;

int avos_register_init_callback( AVOS_REG_CALLBACK *reg );
int avos_register_exit_callback( AVOS_REG_CALLBACK *reg );
int avos_register_suspend_callback( AVOS_REG_CALLBACK *reg );
int avos_register_resume_callback ( AVOS_REG_CALLBACK *reg );
int avos_register_clean_file( AVOS_REG_CLEAN_FILE *reg );

#define AVOS_REGISTER_INIT_RUNLEVEL( callback, runlevel ) \
		static AVOS_REG_CALLBACK _avos_init_##callback = { \
			callback, \
			runlevel, \
			NULL\
		}; \
		static void _fn_reg_avos_init_##callback( void ) __attribute__((constructor));\
		static void _fn_reg_avos_init_##callback( void )\
		{ \
			avos_register_init_callback( &_avos_init_##callback ); \
		}

#define AVOS_REGISTER_INIT( callback )  AVOS_REGISTER_INIT_RUNLEVEL( callback, AVOS_RUNLEVEL_APP )

#define AVOS_REGISTER_EXIT_RUNLEVEL( callback, runlevel ) \
		static AVOS_REG_CALLBACK _avos_exit_##callback = { \
			callback, \
			runlevel, \
			NULL\
		}; \
		static void _fn_reg_avos_exit_##callback( void ) __attribute__((constructor));\
		static void _fn_reg_avos_exit_##callback( void )\
		{ \
			avos_register_exit_callback( &_avos_exit_##callback ); \
		}

#define AVOS_REGISTER_EXIT( callback ) AVOS_REGISTER_EXIT_RUNLEVEL( callback, AVOS_RUNLEVEL_APP )

#define AVOS_REGISTER_SUSPEND_RUNLEVEL( callback, runlevel ) \
		static AVOS_REG_CALLBACK _avos_suspend_##callback = { \
			callback, \
			runlevel, \
			NULL\
		}; \
		static void _fn_reg_avos_suspend_##callback( void ) __attribute__((constructor));\
		static void _fn_reg_avos_suspend_##callback( void )\
		{ \
			avos_register_suspend_callback( &_avos_suspend_##callback ); \
		}

#define AVOS_REGISTER_SUSPEND( callback )  AVOS_REGISTER_SUSPEND_RUNLEVEL( callback, AVOS_RUNLEVEL_APP )

#define AVOS_REGISTER_RESUME_RUNLEVEL( callback, runlevel ) \
		static AVOS_REG_CALLBACK _avos_resume_##callback = { \
			callback, \
			runlevel, \
			NULL\
		}; \
		static void _fn_reg_avos_resume_##callback( void ) __attribute__((constructor));\
		static void _fn_reg_avos_resume_##callback( void )\
		{ \
			avos_register_resume_callback( &_avos_resume_##callback ); \
		}

#define AVOS_REGISTER_RESUME( callback ) AVOS_REGISTER_RESUME_RUNLEVEL( callback, AVOS_RUNLEVEL_APP )

#define AVOS_REGISTER_CLEAN_FILE(name, file) \
		static AVOS_REG_CLEAN_FILE _avos_clean_file_##name =  { \
			file, \
			NULL\
		}; \
		static void _fn_reg_clean_file_##name( void ) __attribute__((constructor));\
		static void _fn_reg_clean_file_##name( void )\
		{ \
			avos_register_clean_file( &_avos_clean_file_##name ); \
		}


#endif //_AVOS_LIFETIME_H_

