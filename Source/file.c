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
#include "atime.h"
#include "file.h"
#include "astdlib.h"
#include "util.h"
#include "file_type.h"
#include "fs.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "athread.h"
#include <dirent.h>
#include <stdio.h>
#include <limits.h>

#define DBGF  if(Debug[DBG_FILE])
#define DBGFF if(Debug[DBG_FILE] > 1)
#define DBGPM if(Debug[DBG_PM])
#define ERR   if(1)

#if defined( DRIVE_SIM )
	#define USE_DRIVE drive_use();
#else
	#define USE_DRIVE
#endif

int file_real_size = 0;

int next_sync = 0;

int file_sync( void )
{
	int t = atime();
	if( t <  next_sync ) {
		return 0;
	}
	next_sync = t + 1000;
//serprintf("sync\r\n" );
#ifndef SIM	
	sync();
#endif
	return 0;
}

static int _do_sync = 1;

void file_set_sync( int sync )
{
	_do_sync = sync;
}

int file_get_sync( void )
{
	return _do_sync;
}

// all *write* operations to the drive should sync!
#define SYNC if( _do_sync ) file_sync();

// *******************************************
//
// 	SIMULATE slow network connections
//	by liming the read/write speed
// 
// *******************************************
static int  speed_limit = 0;
static int  speed;
static int  speed_fd = 0;
static char speed_path[MAX_NAME_LEN + 1] = "";

// *******************************************
//
// 	file_set_speed_limit
// 
// *******************************************
int file_set_speed_limit( int _speed, const char *path )
{
	if( _speed == -1 ) {
serprintf("file_set_speed_limit: OFF\r\n" );
		speed_limit = 0;
		return 0;
	}
	
	speed_limit = 1;
	speed = _speed;
	strnZcpy( speed_path, path, MAX_NAME_LEN );
serprintf("file_set_speed_limit: %d [%s]\r\n", speed, speed_path );
	
	return 0;
}

#ifdef DEBUG_MSG
static void _file_set_speed( int argc, char *argv[] )
{
	int speed = -1;
	char *path = "";
	if (argc > 1) {
		speed = atoi( argv[1] );
	}
	if (argc > 2) {
		path = argv[2];
	}
	if( speed == 0 )
		speed = -1;

	file_set_speed_limit( speed, path );
}

DECLARE_DEBUG_COMMAND("fss", _file_set_speed );
#endif

// *******************************************
//
// 	file_open
// 
// *******************************************
int file_open(const char *pathname, int flags, ...) 
{
	mode_t mode = 0600;

	if( flags & O_CREAT ) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
    		va_end(ap);
	}

DBGF serprintf("file_open( %s, %d, %d )\r\n", pathname, flags, mode);
	USE_DRIVE

	int fd = open( pathname, flags, mode );
	
	if (fd >= 0) {
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		if( speed_limit && !strncmp( pathname, speed_path, strlen(speed_path) ) ) {
serprintf("SPEED_LIMIT for %s\r\n", pathname );
			speed_fd = fd;
		}
	} else {
		int error = errno;
serprintf("file_open %s: %s\r\n", pathname, strerror(error) );
	}
	return fd;
}

// *******************************************
//
// 	file_close_no_sync
// 
// *******************************************
int file_close_no_sync(int fd) 
{
	int err;

DBGF serprintf("file_close_no_sync( %d )\r\n", fd );
	
	err = close( fd );

	int error = errno;
serprintf("file_close_no_sync: %s\n", strerror(error) );

	if( fd == speed_fd )
		speed_fd = 0;
	
	return err;
}

// *******************************************
//
// 	file_close
// 
// *******************************************
int file_close(int fd)
{
	int err;
	int sync = 0;
	
	if( fcntl( fd, F_GETFL ) & (O_WRONLY | O_RDWR) )
		sync = 1;

DBGF serprintf("file_close( %d ), sync: %d\r\n", fd, sync);
		
	err = close( fd );
	
	if (err != 0) {
		int error = errno;
		serprintf("file_close: %s\n", strerror(error) );
	}

	if( fd == speed_fd )
		speed_fd = 0;
		
	// if the file was written to "sync" the drive to prevent an
	// inconsistent fs in case of power loss
	if ( sync ) {
		SYNC
	}

	return err;
}


// *******************************************
//
// 	file_read
// 
// *******************************************
ssize_t file_read(int fd, void *buf, size_t count)
{
	ssize_t err;
	
DBGFF serprintf("file_read( %d, %08X, %d )\r\n", fd, buf, count);
	USE_DRIVE
	
	int start = 0;
	if( speed_limit && fd == speed_fd ) {
		start = atime();
	}
	err = read( fd, buf, count );
	if (err == -1){
		int error = errno;
serprintf("file_read: %s\n", strerror(error) );
	}

	if( speed_limit && fd == speed_fd ) {
		int wait = start + count * 8 / speed - atime();
		if( wait > 0 ) {
//serprintf("wait %5d -> %d\r\n", count, wait );
			msec_sleep( wait );
		}
	}	
	return err;
}

// *******************************************
//
// 	file_write
// 
// *******************************************
ssize_t file_write(int fd, const void *buf, size_t count)
{
	ssize_t err;
	
DBGFF serprintf("file_writ( %d, %08X, %d )\r\n", fd, buf, count);
	USE_DRIVE
	
	int start = 0;
	if( speed_limit && fd == speed_fd ) {
		start = atime();
	}
	err = write( fd, buf, count );
	if (err == -1){
		int error = errno;
serprintf("file_write: %s\n", strerror(error) );
	}
	
	if( speed_limit && fd == speed_fd ) {
		int wait = start + count * 8 / speed - atime();
		if( wait > 0 ) {
//serprintf("wait %5d -> %d\r\n", count, wait );
			msec_sleep( wait );
		}
	}	
	
	return err;
}

// *******************************************
//
// 	file_seek
// 
// *******************************************
off64_t file_seek(int fildes, off64_t offset, int whence)
{
	USE_DRIVE
	off64_t ret = lseek64( fildes, offset, whence );
DBGFF serprintf("file_seek( %d, %lld, %d ) -> %lld  \r\n", fildes, offset, whence, ret );
	return ret;
}

// *******************************************
//
// 	file_tell
// 
// *******************************************
off64_t file_tell(int fildes)
{
	USE_DRIVE
	off64_t ret = lseek64( fildes, 0, SEEK_CUR );
DBGFF serprintf("file_tell( %d ) --> %lld \r\n", fildes, ret );
	return ret;
}

// *******************************************
//
// 	file_get_size
// 
// *******************************************
off64_t file_get_size(int fildes)
{
	STAT st;
	if( fstat( fildes, &st ) )
		return -1;
		
DBGFF serprintf("file_get_size( %d ) -> %lld \r\n", fildes, st.st_size );
	return st.st_size;
}

// *******************************************
//
// 	file_get_total_size
// 
// *******************************************
off64_t file_get_total_size(const char *file_name, int *progressive)
{
	STAT st;

	if( progressive ) {
		*progressive = 0;
	}
DBGF serprintf("file_get_total_size\r\n");
	if( stat( file_name, &st ) )
		return -1;
		
DBGFF serprintf("file_get_total_size( %s ) -> %lld \r\n", file_name, st.st_size );
	return st.st_size;
}

// *******************************************
//
// 	file_get_real_size
// 
// *******************************************
off64_t	file_get_real_size(const char *file_name)
{
	STAT	st;

	if( file_real_size )
		return file_real_size;

	if( file_stat( file_name, &st ) )
		return -1;

	return st.st_size;
}

// *******************************************
//
// 	file_dump
// 
// *******************************************
ssize_t file_dump(const char *name, const void *buf, size_t count)
{
	int fd = file_open( name, O_WRONLY | O_CREAT | O_TRUNC, 0600 );
	if( fd == -1 )
		return 0;
	
	ssize_t ret = file_write( fd, buf, count );
	
	file_close( fd );
	return ret;
}

// *******************************************
//
// 	dir_read
// 
// *******************************************
DIRENT *dir_read(DIR *dir) 
{
DBGFF serprintf("dir_read( %08X )\r\n", dir);
	USE_DRIVE
	return readdir( dir );
}

// *******************************************
//
// 	dir_open
// 
// *******************************************
DIR *dir_open(const char *name)
{
	DIR *dir;
	USE_DRIVE
	dir = opendir( name );
DBGF serprintf("dir_open( %s ) = %08X\r\n", name, dir);
	
	return dir;
}

// *******************************************
//
// 	dir_close
// 
// *******************************************
int dir_close(DIR *dir)
{
	int err;
DBGF serprintf("dir_close( %08X )\r\n", dir);
	err = closedir( dir );

	return err;
}

// *******************************************
//
// 	file_stat
// 
// *******************************************
int file_stat(const char *file_name, STAT *buf) 
{
DBGF serprintf("file_stat( %s )\r\n", file_name );
	USE_DRIVE
	return stat(file_name, buf);
}

// *******************************************
//
// 	file_lstat
// 
// *******************************************
int file_lstat(const char *file_name, STAT *buf) 
{
DBGF serprintf("file_lstat( %s )\r\n", file_name );
	USE_DRIVE
	return lstat(file_name, buf);
}

// *******************************************
//
// 	file_fstat
// 
// *******************************************
int file_fstat(int filedes, STAT *buf)
{
DBGF serprintf("file_fstat( %d )\r\n", filedes );
	return fstat(filedes, buf);
}

// *******************************************
//
// 	file_can_create
// 
// returns 1 if the filename is correct and can be used to create a file on the filesystem
// if can_overwrite is 0, also check that the file does not exist yet
// use this before a record operation
// *******************************************
int file_can_create( const char * file_name, int can_overwrite )
{
	STAT st;

	// fixme - should check the validity of the filename also and try to create it really
	if ( file_stat( file_name, &st ) )
		return 1;

	return 0;
}

// *******************************************
//
// 	file_remove
// 
// *******************************************
int file_remove(const char * file_name)
{
DBGF serprintf("file_remove( %s )\r\n", file_name );
	USE_DRIVE
	int ret = unlink( file_name );
	if(ret < 0){
		int error = errno;
serprintf("file_remove %s: %s\n", file_name, strerror(error));
	}
	SYNC
	return ret;
}

// *******************************************
//
// 	file_rename
// 
// *******************************************
int file_rename(const char *oldpath, const char *newpath)
{
DBGF serprintf("file_rename( %s -> %s )\r\n", oldpath, newpath );
	USE_DRIVE
	int ret = rename( oldpath, newpath );
	if(ret < 0){
		int error = errno;
serprintf("file_rename %s -> %s: %s\r\n", oldpath, newpath, strerror(error));
	}
	SYNC
	return ret;
}

// *******************************************
//
// 	file_copy
// 
// *******************************************
int file_copy(const char *src_path, const char *dst_path, int (*progress_cb)(void *data, int progress), void *ctx)
{
DBGF serprintf("file_copy( %s -> %s )\r\n", src_path, dst_path );
	int src = -1;
	int dst = -1;
	int ret = 0;
	STAT st;

	const int block_size = 1024 * 1024;
	UCHAR *buffer = amalloc(block_size);
	if( !buffer ){
		ret = 1;
		goto exit;
	}
 
	UINT64	copied	= 0;
	ssize_t got, written;

	if((src = file_open( src_path, O_RDONLY, S_IRUSR )) < 0) {
		ret = 1;
		goto exit_free_buffer;
	}
	
	if ( file_fstat(src, &st) ) {
		ret = 1;
		goto exit_close_src;
	}

	if ((dst = file_open(dst_path, O_WRONLY|O_CREAT|O_TRUNC, st.st_mode )) < 0) {
		ret = 1;
		goto exit_close_src;
	}
	
	while (copied < st.st_size) {
		if( progress_cb ){
			int abort = progress_cb(ctx, ( (UINT64)copied * (UINT64)100 / (UINT64)st.st_size ));
			if( abort ){
serprintf("file_copy: abort\r\n");
				goto abort;
			}
		}

		got = file_read(src, buffer, block_size);
		if (got < 0) {
			ret = 1;
			goto exit_close_dst;
		}

		while( got  ){
			written = file_write(dst, buffer, got);
			if (written < 0) {
				ret = 1;
				goto exit_close_dst;
			}
			copied += written;
			got -= written;
		}
	}

	if( progress_cb )
		progress_cb(ctx, 100);

exit_close_dst:
	file_close(dst);
exit_close_src:
	file_close(src);
exit_free_buffer:
	afree(buffer);
exit:
	return ret;

abort:

	file_close(src);
	file_close(dst);

	afree(buffer);

	if ( file_remove( dst_path ) ) {
serprintf("%s/%i: unlink err: %s\r\n", __FUNCTION__, __LINE__, strerror( errno ) );
	}
	return 0;
}

// *******************************************
//
// 	dir_create
// 
// *******************************************
int dir_create(const char *pathname, mode_t mode)
{
DBGF serprintf("dir_create( %s, %d )\r\n", pathname, mode );
	USE_DRIVE
	int ret = mkdir( pathname, mode );

	SYNC

	return ret;
}

// *******************************************
//
// 	dir_remove
// 
// *******************************************
int dir_remove(const char * name)
{
DBGF serprintf("dir_remove( %s )\r\n", name );
	USE_DRIVE
	int ret = rmdir( name );
	SYNC
	return ret;
}

// *******************************************
//
// 	dir_recursive_remove
// 
// *******************************************
int dir_recursive_remove(const char * name)
{
	DIR 	*dh;
	DIRENT 	*d;
	int ret = 0;
	char filename[1024];

DBGF serprintf("dir_recursive_remove( %s )\r\n", name );
	USE_DRIVE

	dh = dir_open( name );
	if ( !dh ) {
		ret = -1;
		goto exit_dir_recursive_remove;
	}

	while( (d = dir_read( dh )) != NULL ) {
		STAT st;

		// skip . and ..
		if (!strcmp(d->d_name, "."))
			continue;
		if (!strcmp(d->d_name, ".."))
			continue;

		snprintf(filename, 512, "%s/%s", name, d->d_name);

		// if directory, dive in
		if (file_lstat(filename, &st) >= 0) {
			if (!S_ISLNK(st.st_mode) && S_ISDIR(st.st_mode)) {
				dir_recursive_remove(filename);
				continue;
			}

DBGF	 		serprintf("file to be removed: %s\n",filename);
			file_remove(filename);
		}
	}
	dir_close( dh );

	if (ret >= 0) {
DBGF 		serprintf("dir to be removed: %s\n",name);
		/* finally remove the empty dir */
		ret = dir_remove(name);
	}

exit_dir_recursive_remove:
	SYNC
	return ret;
}

/**
 *  dir_recursive copy
 * 
 *  copy all files from directory @from to directory @to.
 *  @to must exist before.
 */
int dir_recursive_copy(const char* from, const char* to)
{
	DIR 	*dh;
	DIRENT 	*d;
	int ret = 0;
	char f_name[MAX_NAME_LEN * 2];
	char t_name[MAX_NAME_LEN * 2];

DBGF serprintf("dir_recursive_copy( %s => %s )\r\n", from, to);
	USE_DRIVE

	dh = dir_open( from );
	if ( !dh ) {
		ret = -1;
		goto exit_dir_recursive_copy;
	}

	while( (d = dir_read( dh )) != NULL ) {
		STAT st;

		// skip . and ..
		if (!strcmp(d->d_name, "."))
			continue;
		if (!strcmp(d->d_name, ".."))
			continue;

		snprintf(f_name, sizeof(f_name), "%s/%s", from, d->d_name);
		snprintf(t_name, sizeof(t_name), "%s/%s", to, d->d_name);

		// if directory, dive in
		if (file_lstat(f_name, &st) >= 0) {
			if (!S_ISLNK(st.st_mode) && S_ISDIR(st.st_mode)) {
				dir_create(t_name, st.st_mode);
				dir_recursive_copy(f_name, t_name);
				continue;
			}

			file_copy(f_name, t_name, NULL, NULL);
		}
	}
	dir_close( dh );

 exit_dir_recursive_copy:
	SYNC
	return ret;
}

// WARNING GEN6 : this function is not case sensitive on the device
int file_exists( const char* file_name, int check_readable )
{
	int flags = F_OK;

	if (check_readable)
		flags = R_OK;

	if ( access( file_name, flags ) == 0 )
		return 1; // ok, exists
	return 0; // doesn't exist
}

int test_and_create_dir(const char* path)
{
	struct stat st_buf;

	int err = stat(path, &st_buf);
	if (err) {
		if (mkdir(path, 0777)) {
			return -1;
		}
	}
	else if (!S_ISDIR(st_buf.st_mode)) {
		if (!remove(path) && mkdir(path, 0777)) {
			return -1;
		}
	}
	return 0;
}

#ifdef DEBUG_MSG
// ********************************************************************************
//
//	D E B U G stuff
//	_write_file
//
// ********************************************************************************
static void _write_file( int argc, char *argv[] )
{
	int chunk = atoi(argv[3]) ? atoi(argv[3]) : 1024;
	int size  = 1024 * 1024 * atoi(argv[2]);

	char *path = argv[1];
	unsigned long b_time;

serprintf("writing %s, size %d MB, chunk %d\r\n", path, atoi(argv[2]), chunk);

	int flags = O_WRONLY | O_CREAT | O_TRUNC;
	int fh;
	unsigned char *data;
	unsigned char *buf;

	if( argc >= 5 && atoi(argv[4]) ){
serprintf("\tusing O_DIRECT\r\n");
		flags |= O_DIRECT;
	} else {
serprintf("\tusing STANDARD\r\n");
	}

	fh = file_open( path, flags, 0600 );
	if( fh == -1 ){
serprintf(":-(\r\n");
	}

	data = amalloc(chunk+512);
	if(!data){
serprintf(":-(:-(\r\n");
	}
	buf = (unsigned char*)(512*(((size_t)data + 512)/512));

serprintf("buf is @%p\r\n", buf);

	b_time = atime();

	while( size > 0 ){
		int to_write = MIN( size, chunk );

		if( to_write < chunk )
			to_write = 512 * (to_write + 512 ) / 512;
		
		file_write( fh, buf, to_write );
		size -= to_write;
	}

	file_close(fh);

serprintf("time : %d ms\r\n", atime() - b_time);
	afree(data);
}

// ********************************************************************************
//
//	D E B U G stuff
//	_file_real_size
//
// ********************************************************************************
static void _file_real_size( int argc, char *argv[] )
{ 
	if( argc > 1 ){
		file_real_size = atoi( argv[1] );
serprintf("file_real_size %d\r\n", file_real_size );
	}
}

DECLARE_DEBUG_COMMAND("fwr", _write_file );
DECLARE_DEBUG_COMMAND("frs", _file_real_size );
#endif
