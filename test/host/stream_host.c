// Copyright 2026 Courville Software
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/* See stream_host.h: a fake STREAM around the REAL external-subtitle code. */
#include "stream_host.h"
#include "stream_subtitle.h"
#include "stream_buffer.h"
#include "athread.h"
#include "stream_alloc.h"
#include "fake_backend.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *stream_sub_dec_thread( void *data );     /* stream_subtitle.c */

/* ---------------------------------------------------------------------------------------------
 * Stand-ins for what the subtitle code only calls out to. All weak, so a test that links the real
 * thing wins; none of them decides anything about subtitles.
 * ------------------------------------------------------------------------------------------- */
#define WEAK __attribute__((weak))

WEAK int   stream_pause( STREAM *s )                            { (void)s; return 0; }
WEAK void  stream_un_pause( STREAM *s, int was_paused )         { (void)s; (void)was_paused; }
WEAK int   stream_seek_time( STREAM *s, int t, int dir, int f ) { (void)s; (void)t; (void)dir; (void)f; return 0; }
WEAK int   stream_get_current_time( STREAM *s, int *total )     { (void)s; if( total ) *total = 0; return 0; }
WEAK void  stream_buffer_fix_subs( STREAM_BUFFER *b )           { (void)b; }
WEAK void  stream_parser_clear_subtitle_chunks( STREAM *s )     { (void)s; }
WEAK STREAM_DEC_SUB *stream_get_new_dec_sub( int format )       { (void)format; return NULL; }
WEAK void  stream_yield_RT( void )                              { usleep( 100 ); }
WEAK float audio_interface_get_audio_speed()                    { return 1.0f; }
WEAK int   audio_interface_is_audio_speed_enabled()             { return 0; }

/* ---------------------------------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------------------------------- */
static char g_path[512];

static const char *write_cues( HOST_STREAM *h, const char *tag, const char *ext, char label, int n, int vtt )
{
    snprintf( g_path, sizeof g_path, "%s/movie.%s.%s", h->dir, tag, ext );
    FILE *f = fopen( g_path, "w" );
    if( !f ) { perror( g_path ); exit( 3 ); }
    if( vtt ) fputs( "WEBVTT\n\n", f );
    for( int i = 0; i < n; i++ ) {
        int a = i * 1000, b = a + 900;
        if( !vtt ) fprintf( f, "%d\n", i + 1 );
        fprintf( f, "%02d:%02d:%02d%c%03d --> %02d:%02d:%02d%c%03d\n%c%d first line\n%c%d second\n\n",
                 a / 3600000, a / 60000 % 60, a / 1000 % 60, vtt ? '.' : ',', a % 1000,
                 b / 3600000, b / 60000 % 60, b / 1000 % 60, vtt ? '.' : ',', b % 1000,
                 label, i, label, i );
    }
    fclose( f );
    return g_path;
}
const char *hs_write_srt( HOST_STREAM *h, const char *tag, char label, int n ) { return write_cues( h, tag, "srt", label, n, 0 ); }
const char *hs_write_vtt( HOST_STREAM *h, const char *tag, char label, int n ) { return write_cues( h, tag, "vtt", label, n, 1 ); }

/* ---------------------------------------------------------------------------------------------
 * The stream
 * ------------------------------------------------------------------------------------------- */
HOST_STREAM *hs_create( void )
{
    HOST_STREAM *h = calloc( 1, sizeof *h );
    snprintf( h->dir, sizeof h->dir, "/tmp/avos-host-subs.XXXXXX" );
    if( !mkdtemp( h->dir ) ) { perror( "mkdtemp" ); exit( 3 ); }
    snprintf( h->video, sizeof h->video, "%s/movie.mkv", h->dir );
    FILE *v = fopen( h->video, "w" );          /* placeholder: only its name is used for discovery */
    if( v ) { fputs( "x", v ); fclose( v ); }

    STREAM *s = calloc( 1, sizeof *s );
    h->s = s;
    pthread_mutex_init( &s->subtitle_owner_lock, NULL );
    pthread_cond_init(  &s->subtitle_owner_cond, NULL );
    pthread_mutex_init( &s->subtitle_table_lock, NULL );
    thread_state_init( &s->parser_tstate, THREAD_RUNNING, "parser" );
    thread_state_init( &s->sub_tstate,    THREAD_RUNNING, "sub" );
    thread_state_init( &s->engine_tstate, THREAD_RUNNING, "engine" );
    s->open      = 1;
    s->video_time = 0;
    s->src.url   = strdup( h->video );

    h->eng = sub_engine_create();
    sub_engine_start( h->eng, fake_clock, NULL );
    s->sub_engine = h->eng;
    return h;
}

/* The demuxer thread's only job as far as the subtitle code can tell: be acknowledging
 * thread-state requests (stream_set_subtitle_stream() idles it before swapping an external track). */
static void *parser_standin( void *arg )
{
    STREAM *s = arg;
    while( thread_state_get( &s->parser_tstate ) != THREAD_EXIT ) {
        thread_state_ack( &s->parser_tstate );
        usleep( 200 );
    }
    return NULL;
}

int hs_start( HOST_STREAM *h )
{
    STREAM *s = h->s;
    stream_sub_ext_check( s );
    int n = s->av.subs_max;
    if( n > 0 ) {
        s->av.subs  = 0;
        s->subtitle = s->av.sub + 0;
    } else {
        s->subtitle = s->av.sub;
    }
    pthread_create( &h->parser_th, NULL, parser_standin, s );
    pthread_create( &h->sub_th, NULL, stream_sub_dec_thread, s );
    h->started = 1;
    return n;
}

int hs_track( HOST_STREAM *h, const char *tag )
{
    char want[64];
    snprintf( want, sizeof want, "movie.%s.", tag );
    STREAM *s = h->s;
    for( int i = 0; i < s->av.subs_max; i++ ) {
        if( s->av.sub[i].ext && strstr( s->av.sub[i].path, want ) ) return i;
    }
    return -1;
}

int hs_select( HOST_STREAM *h, int idx )
{
    return stream_set_subtitle_stream( h->s, idx );
}

void hs_idle_sub_thread( HOST_STREAM *h, int hold_us )
{
    thread_state_set( &h->s->sub_tstate, THREAD_IDLE );
    if( hold_us > 0 ) usleep( (useconds_t)hold_us );
    thread_state_set( &h->s->sub_tstate, THREAD_RUNNING );
}

static void remove_dir( const char *dir )
{
    DIR *d = opendir( dir );
    if( d ) {
        struct dirent *e;
        while( (e = readdir( d )) ) {
            if( !strcmp( e->d_name, "." ) || !strcmp( e->d_name, ".." ) ) continue;
            char p[600];
            snprintf( p, sizeof p, "%s/%s", dir, e->d_name );
            unlink( p );
        }
        closedir( d );
    }
    rmdir( dir );
}

void hs_destroy( HOST_STREAM *h )
{
    STREAM *s = h->s;
    if( h->started ) {
        thread_state_set( &s->sub_tstate, THREAD_EXIT );
        pthread_join( h->sub_th, NULL );
        thread_state_set( &s->parser_tstate, THREAD_EXIT );
        pthread_join( h->parser_th, NULL );
    }
    stream_sub_ext_close( s );                 /* as stream_stop() does: stops the parse workers, frees the tracks */
    sub_engine_destroy( h->eng );
    pthread_mutex_destroy( &s->subtitle_owner_lock );
    pthread_cond_destroy(  &s->subtitle_owner_cond );
    pthread_mutex_destroy( &s->subtitle_table_lock );
    for( int i = 0; i < SUB_TRACK_MAX + 2; i++ ) free( s->sub_url[i] );
    free( s->src.url );
    frame_free( s->subtitle_frame );
    free( s );
    remove_dir( h->dir );
    free( h );
}

/* ---------------------------------------------------------------------------------------------
 * Observation
 * ------------------------------------------------------------------------------------------- */
int hs_wait_cues( int n, int timeout_ms )
{
    for( int t = 0; t < timeout_ms; t++ ) {
        if( fake_backend_active_since_flush() == n ) return 1;
        usleep( 1000 );
    }
    return fake_backend_active_since_flush() == n;
}

int hs_stays_at( int n, int settle_ms )
{
    for( int t = 0; t < settle_ms; t++ ) {
        if( fake_backend_active_since_flush() != n ) return 0;
        usleep( 1000 );
    }
    return fake_backend_active_since_flush() == n;
}
