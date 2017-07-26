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

#ifdef CONFIG_SUBTITLES

#define DBG  if(Debug[DBG_SUB])
#define DBG2 if(Debug[DBG_SUB] > 1)

#include "subtitle.h"
#include "subtitle_files.h"
#include "subtitle_graphic.h"
#include "videoplayer_screen.h"

static int lang_index;
int subtitles_font_index;
int subtitle_font_size = 0;
int subtitles_time_offset = 0;
static RECT area;

int subtitles_init( void )
{
DBG serprintf("subtitles_init\r\n");
	subtitles_set_font_index( subtitles_font_index );
	lang_index = 0;
	subtitles_set_lang( lang_index );
	
	return 0;
}

int subtitles_add( AV_PROPERTIES *av )
{
DBG serprintf("subtitles_add\r\n" );
	if( !av )
		return 1;

	subtitles_graphic_open( av );

	int count = subtitles_graphic_get_count();

	if( !lang_index && count ) {
		lang_index = 1;
		subtitles_set_lang( lang_index );
	}
	return (count == 0);
}

void subtitles_close( void )
{
DBG serprintf("subtitles_close\r\n" );
	subtitles_graphic_close();
}

void subtitles_set_lang( int lang )
{
	int count = subtitles_graphic_get_count();

DBG serprintf("subtitles_set_lang( %d )\r\n", lang );
	lang_index = lang;
	if( !lang ) {
		subtitles_clear();
	} else if ( count ) {
		subtitles_graphic_set_lang( lang - 1 );
	}
}

int subtitles_get_lang( int *_text )
{
	int text = 0;
	if( lang_index ) {
		subtitles_graphic_get_item( lang_index - 1, &text );
	}
DBG serprintf("subtitles_get_lang() = %d  text %d\r\n", lang_index, text );
	if( _text )
		*_text = text;
	return lang_index;
}

int subtitles_get_count( void )
{
	int count = subtitles_graphic_get_count();

	if( count > 0 ) {
		count ++;
	}
DBG serprintf("subtitles_get_count() = %d\r\n", count );
	return count;
}

const char *subtitles_get_name( int lang, int *_text )
{
	int count = subtitles_graphic_get_count();
	
	if( count > 0 ) {
		count ++;
	}

	int text  = 0;
	const char *name;
	if( lang == 0 || lang >= count ) {
		name = "Off";
	} else {
		name = subtitles_graphic_get_item( lang - 1, &text );
	} 	
	if( _text ) {
		*_text = text;
	}
DBG serprintf("subtitles_get_name( %2d ) [%s]\r\n", lang, name );
	return name;
}

void subtitles_set_font_index( int index )
{
DBG serprintf("subtitles_set_font_index( %d )\r\n", index );
	subtitles_font_index = index;

	switch( subtitles_font_index ) {
		case 1: subtitle_font_size = FONT_SMALL; break;
		case 0:	subtitle_font_size = FONT_LARGE; break;
	}
	VideoPlayerScreen_setSubtitleFontSize( subtitle_font_size );
}

int subtitles_get_font_index( void )
{	
DBG serprintf("subtitles_get_font_index() = %d\r\n", subtitles_font_index );
	return subtitles_font_index;
}

void subtitles_display( int s_time )
{
	if( !lang_index ) {
		return;
	}
	
	subtitles_graphic_display( s_time );
}

void subtitles_redraw( void )
{
	if( !lang_index ) {
		return;
	}
	
	//subtitles_graphic_redraw();
}

void subtitles_show( void )
{
	int text;
	subtitles_get_lang( &text );
	VideoPlayerScreen *screen = OBJ_AS( VideoPlayerScreen, Screen_find( VIDEOPLAYER_SCREEN_NAME ) );
	if( text ) {
DBG serprintf("show text subtitles\r\n");
		VideoPlayerScreen_enableSubtitle( screen, TRUE );
		VideoPlayerScreen_enableGraphicSubtitle( screen, FALSE );
	} else {
DBG serprintf("show gfx subtitles\r\n");
		VideoPlayerScreen_enableSubtitle( screen, FALSE );
		VideoPlayerScreen_enableGraphicSubtitle( screen, TRUE );
	}
}

static void _update_area( void )
{
	VideoPlayerScreen *screen = OBJ_AS( VideoPlayerScreen, Screen_find( VIDEOPLAYER_SCREEN_NAME ) );
	VideoPlayerScreen_getSubtitleArea( screen, &area );
DBG2 Rect_Dump( &area, "new");
}

Rect subtitles_get_area( void )
{
	if( !lang_index ) {
		return (Rect){0, 0, 0, 0};
	}
	return area;
}

void subtitles_text_update( const char *top, const char *bottom )
{
DBG2 serprintf("subtitles_text_update [%s][%s]\r\n", top, bottom);
	VideoPlayerScreen_updateSubtitle( top, bottom );
	_update_area();
}

void subtitles_clear( void )
{
DBG serprintf("subtitles_clear\r\n");
	VideoPlayerScreen_updateSubtitle( "", "" );
	_update_area();
}

void subtitles_graphic_update( const IMAGE *img )
{	
	VideoPlayerScreen_updateGraphicSubtitle( img );
	_update_area();
}
#endif
