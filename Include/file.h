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

// *****************************************************
//
// *****************************************************
#ifndef _AFILE_H
#define _AFILE_H

#include "types.h"
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <utime.h>

typedef struct dirent 	DIRENT;
typedef struct stat 	STAT;

int 	file_open(const char *pathname, int flags, ...);
int 	file_close(int fd); 
int 	file_close_no_sync(int fd); 
int 	file_sync(void); 
void 	file_set_sync( int sync );
int 	file_get_sync( void );
ssize_t file_read(int fd, void *buf, size_t count);
ssize_t file_write(int fd, const void *buf, size_t count);
off64_t	file_seek(int fildes, off64_t offset, int whence);
off64_t	file_tell(int fildes);
off64_t file_get_size(int fildes);
off64_t	file_get_real_size(const char *file_name);
off64_t file_get_total_size(const char *file_name, int *progressive);
int 	file_stat(const char *file_name, STAT *buf);
int 	file_lstat(const char *file_name, STAT *buf);
int 	file_fstat(int filedes, STAT *buf);
int 	file_can_create( const char * file_name, int can_overwrite );
int	file_exists( const char* file_name, int check_readable );
int 	file_remove(const char * file_name);
int 	file_rename(const char *oldpath, const char *newpath);
int 	file_copy(const char *src_path, const char *dst_path, int (*progress_cb)(void *data, int progress), void *ctx);

ssize_t	file_dump(const char *pathname, const void *data, size_t count);

DIRENT 	*dir_read(DIR *dir); 
DIR 	*dir_open(const char *name);
int 	dir_close(DIR *dir);
int 	dir_remove(const char * name);
int 	dir_recursive_remove(const char * name);
int 	dir_recursive_copy(const char* from, const char* to);
int 	dir_create(const char *pathname, mode_t mode);

#define WAITMESSAGE	if ( drive_power_state() ) {\
				LCD_WaitMessage();\
			}

#if defined( SIM ) || defined( DRIVE_SIM )
void	drive_use( void );
void	drive_init( void );
void	drive_exit( void );
#endif

int  	file_set_speed_limit( int _speed, const char *path );
int     test_and_create_dir(const char* path);

#endif

