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
#include "subtitle_format.h"
#include "util.h"
#include "astdlib.h"
#include "i18n.h"

#include <ctype.h>

#define DBG if(Debug[DBG_SUB])

/*******************
 SMI line
 Gets the sync start time, return the pointer to char after sync

******************/
#define SMI_ARRAY_SIZE 10
static sub_coding_style *chop_line( char * );

static int detect_SMI( FILE * file )
{
	//make sure that read starts at the beginning of file
	fseek( file, 0, SEEK_SET );
	
	char _tmp[ LINE_LEN + 1 ];
	char* tmp = _tmp;
	if ( !fgets( tmp, LINE_LEN, file ) ) {
		goto ErrorExit;
	}
	if ( strstrNC( tmp, "<SAMI>" ) ) {
		fseek( file, 0, SEEK_SET );
DBG serprintf( "SMI: found!\n" );
		return 0;
	}

ErrorExit:
DBG serprintf( "SMI: not SAMI\n" );
	return 1;
}

static uni_sub *store_title(uni_sub* sub_rec, sub_line* title)
{
	if(!title){
		return sub_rec;
	}
	if(title->top == 0 && title->bottom == 0){
		afree(title);
		return sub_rec;
	}
	if(!sub_rec->first){
		sub_rec->first = title;
		sub_rec->last  = title;
	} else {
		title->prev = sub_rec->last;
		sub_rec->last->next = title;
		sub_rec->last = title;	
	}
	return sub_rec;
}
//calculates end times for titles that have none
static void compress_times( uni_sub * sub_rec )
{
	sub_line *title = sub_rec->first;
	while ( title ) {
		if ( title->end == 0 ) {
			if ( title->next ) {
				title->end = title->next->start -1;
			} else {
				title->end = title->start + 10000;
				// this is the last title. Should not be end should not be zero
				return;
			}
		}
		if(title->next == title){
			return;
		}
		title = title->next;
	}
	return;
}

char* compressCoding(char* dst, char** lines, int pos, int max)
{
	int ipos = 0;
	while(pos < max && !strstr(lines[pos], "</style>") ){
		if((strlen(dst) + strlen(lines[pos])) >= LINE_LEN){
			return 0;
		}
		int i = 0;
		while(i < (strlen(lines[pos])-1)){
			if(!isspace(lines[pos][i])){
				dst[ipos++] = lines[pos][i];
				if(dst[ipos-1] == '}'){
					if(dst[0] != '.'){
						return 0;
					}
					return dst;
				}
			}
			++i;
		}
		pos++;
	}
	if(dst[0] != '.'){
		return 0;
	}

	return dst;
}
/************
 * 
 * function to parse complex SMI headers
 * 
 * ************/
static sub_coding_style **info_SMI( FILE * file, int *cnt, uint32_t *palette, int *has_palette )
{
	char _header[LINE_LEN+1];
	char* header = _header;
	int init_header_len = 30;
	int i = 0;
	char **hd = acalloc( init_header_len, sizeof(char*) );
	char *tmp;
	int styles = 0;
	sub_coding_style **style_arr = NULL;
	sub_coding_style *ifstyle = NULL;
	//make sure that reading start from the begining of file
	fseek( file, 0, SEEK_SET );
	
	//store header to read it easier.
	header = fgets( header, LINE_LEN, file );
	while ( !strstrNC( header, "</Head>" ) ) {
		//if the header is long
		if ( i == init_header_len ) {
			hd = arealloc( hd, ( init_header_len + 10 ) * sizeof(char*) );
			init_header_len += 10;
		}
		header = ( fgets( header, LINE_LEN, file ) );
		if ( feof( file ) ) {
			DBG serprintf( "SMI: <subtitle_parse_samiheader()>: Unexpected end of file\n" );
			goto ERROR_CLEAN;
		}
		hd[i] = acalloc( LINE_LEN + 1, 1 );
		strcpy( hd[i], header );
		++i;
	}
	init_header_len = i;
	//find '-->' which come right after .XXX definitions
	for ( i = 0; i < init_header_len - 1 ; ++i ) {
		tmp = hd[i];
		if(!tmp){
			continue;
		}
		tmp = strchr(tmp, '.');
		if(!tmp){
			continue;
		}
		if(strchr(tmp, '{') && strchr(tmp,'}')){
			ifstyle = chop_line(tmp);
		} else {
			char toComp[LINE_LEN];
			memset(toComp,0,LINE_LEN);
			if(compressCoding(toComp, hd, i, init_header_len)){
				ifstyle = chop_line(toComp);
			}
		}
		   
		if(ifstyle){
			style_arr = arealloc(style_arr,(styles + 1) * sizeof(sub_coding_style*));
			style_arr[styles++] = ifstyle;
			ifstyle = 0;
		}
	}
	*cnt = styles;
	for ( i = 0; i < init_header_len; ++i ) {
		if ( hd[i] ) {
			afree( hd[i] );
		}
	}
	afree( hd );
	return style_arr;
ERROR_CLEAN:
	if(hd){
		for ( i = 0; i < init_header_len; ++i ) {
			if ( hd[i] ) {
				afree( hd[i] );
			}
		}
		afree( hd );
	}
	if(style_arr){
		for(i = 0; i < styles;++i){
			if(style_arr[i]->id)
				afree(style_arr[i]->id);
			if(style_arr[i]->name)
				afree(style_arr[i]->name);
			if(style_arr[i]->lang)
				afree(style_arr[i]->lang);
			afree(style_arr[i]);
		}
		afree(style_arr);
	}
	*cnt = 0;
	return 0;
}

/**********
 * SMI header parsing function
 * 
 * **************/
static sub_coding_style *chop_line( char *line )
{
	if(!line){
		return 0;
	}
	char *tmp = strchr( line, '{' );
	if(!tmp){
		return 0;
	}
	sub_coding_style *codex = acalloc(1, sizeof( sub_coding_style ) );
	codex->id = acalloc( tmp - line, 1);
	*tmp = '\0';
	tmp++;
	//copy the language identifier but skip the starting '.'
	line++;
	int i = 0;
	while(tmp > line){
		if(!isspace(*line))
			codex->id[i++] = *line;
		line++;
	
	}
	if(!tmp){
		return 0;
	}

	line = strchr( tmp, ':' );
	if(!line){
		return 0;
	}
	tmp = strchr( line, ';' );
	if ( !line || !tmp ) {
		DBG serprintf( "Parse error in SAMI header .XXX section\n" );
		afree(codex->id);
		afree(codex);
		return 0;
	}
	
	codex->name = acalloc( tmp - line, 1 );
	*tmp = '\0';
	//copy language name
	strcpy( codex->name, line + 1 );
	tmp++;
	line = strchr( tmp, ':' );
	tmp = strchr( line, ';' );
	if ( !line || !tmp ) {

		DBG serprintf( "Parse error in SAMI header .XXX section\n" );
		afree(codex->id);
		afree(codex->name);
		afree(codex);
		return 0;
	}
	codex->lang = acalloc( tmp - line, 1 );
	codex->type = 0;	//future use
	*tmp = '\0';
	strcpy( codex->lang, line + 1 );
	DBG serprintf( "data: '%s' '%s' '%s'\n", codex->id, codex->name, codex->lang );
	return codex;
}

/****************
 * set_SMI_code 
 * input: subt_orig* which contains data gathered this far. will be gathere if not found from struct
 * output: char* which contains the valid identifier  or NULL if not found
 * 
 * In the SMI file header .XYZ line defines languages and codepages.
 * This XYZ is later used to define different language subtitles. There may be multiple .XYZ definitions
 * 
 * Function sets the default XYZ identifies to subs->default_language. If it already exists
 * the function checks that it is valid
 * 
 * *****************/
static char *set_SMI_code( subt_orig * subs, FILE * file )
{
	int i;
	//Find out which ID codes to use. SMI format has at least one language defined, but
	//may have many. The required data can be acquired from header.
	if ( !subs->default_language ) {
		if ( !subs->title_langs ) {
			subs->title_langs = info_SMI( file, &subs->lan_count, NULL, NULL );
			if ( !subs->title_langs ) {

				DBG serprintf( "Subtitle: parseSMI: Invalid language select\n" );
				return 0;
			}
		}
		subs->default_language = astrdup( subs->title_langs[0]->id );
	}
	if ( !subs->title_langs ) {
		subs->title_langs = info_SMI( file, &subs->lan_count, NULL, NULL );
		if ( !subs->title_langs ) {
			DBG serprintf( "subtitle: parseSMI: SMI header reading error\n" );
			return 0;
		}
	}
	//verify that selected language is really found. Otherwise we wont get anything. Fall to first one
	//if the requested ID is not found
	for ( i = 0; i < subs->lan_count; ++i ) {
		if ( !strncmp( subs->default_language, subs->title_langs[i]->id, strlen( subs->default_language ) ) ) {
			//found match. Use it
	//		subs->default_language = astrdup( subs->title_langs[i]->id );
			return subs->default_language;
		}
	}
	//Nope. The requested ID does not exist. Copy it from first location.
	afree( subs->default_language );
	subs->default_language = astrdup( subs->title_langs[0]->id );

	return subs->default_language;
}

//stores the line to sub_line,
//determines where to copy the new line
static inline void store_text_line( sub_line *new_line, char *line, int clean_tags, int utf8 )
{
#ifdef CONFIG_I18N
	char utf[ LINE_LEN * 2 + 1 ] = { 0 };
	if( !utf8 ) {
//serprintf("cp: %s\r\n", line );
		wchar unicode[ LINE_LEN + 1 ] = { 0 };
		wchar *uc = unicode;
		char *c   = line;
		while(*c){
			c+= I18N_codepage_to_unicode(c, uc);
			uc++;
		}
		*uc = 0;
	
		utf16_to_utf8( utf, unicode, LINE_LEN);
		line = utf;
//serprintf("uc: %s\r\n", line );
	}
#endif
	char *tmp_line = subtitle_clean_formatter(line, clean_tags);
	char *end = strstr(line, "&nbsp");
	if(end == line || end == line + 1){
		new_line->top = astrdup(" ");
		goto SMI_TXT_CLEAN;
	}
	if ( !new_line->top ) {
		new_line->top = astrdup( tmp_line );
		goto SMI_TXT_CLEAN;
	}
	if ( !new_line->bottom ) {
		new_line->bottom = astrdup( tmp_line );
		goto SMI_TXT_CLEAN; 
	}
	//both lines are alread filled. do some rearring
	if ( strlen( tmp_line ) < ( strlen( new_line->bottom ) + strlen( new_line->top ) ) ) {
		new_line->bottom = arealloc( new_line->bottom, ( strlen( new_line->bottom ) + strlen( tmp_line ) + 3 ) );
		strcat( new_line->bottom, " " );
		strcat( new_line->bottom, tmp_line );
		goto SMI_TXT_CLEAN; 		
	} else {		// move the bottom line to end of upperline and tmpline to bottomline
		new_line->top = arealloc( new_line->top, ( strlen( new_line->top ) + strlen( new_line->bottom ) + 3 ) );
		strcat( new_line->top, " " );
		strcat( new_line->top, new_line->bottom );
		afree( new_line->bottom );
		new_line->bottom = astrdup( tmp_line );
		goto SMI_TXT_CLEAN; 		
	}
SMI_TXT_CLEAN:
	afree(tmp_line);
}

//input dataline contains <br> somewhere. if it is at the begin, copy everything to bottomline else, strcat head
//to topline and rest to bottom line
static inline void handle_linebreak( char *data, sub_line *title, int clean_tags )
{
	char *data_low = 0;
	char* dataline = subtitle_clean_formatter(data, clean_tags);
	if ( !strncmp( dataline, "<br", 3 ) ) {
		data_low = strchr(dataline, '>');
		data_low++;
		*dataline = '\0';
	} else {
		//split the string to two C strings
		data_low = strstr( dataline, "<br" );
		*data_low = '\0';
		data_low += 4;
		if(*data_low == '>')
			data_low++;
	}
	if ( dataline ) {
		if ( !title->top ) {
			title->top = astrdup( dataline );
		} else {
			title->top = arealloc( title->top, strlen( title->top ) + strlen( dataline ) + 1 );
			strcat( title->top, dataline );
		}
	}

	if ( !title->bottom ) {
		title->bottom = astrdup( data_low );
	} else {
		title->bottom = arealloc( title->bottom, strlen( title->bottom ) + strlen( data_low ) + 1 );
		strcat( title->bottom, data_low );
	}
	afree(dataline);
}
static char *clear_empty( char *tmp )
{
	while ( isspace( *tmp ) ) {
		tmp++;
	}
	if ( ( *tmp ) == MS_CURSOR_BEGIN || ( *tmp ) == NEW_LINE_CH ) {
		return 0;
	}
	return tmp;
}

BOOL smi_match_coding( char *coding, char *line )
{
	if ( !line || !coding )
		return FALSE;
	if ( *line == '"' )
		line++;
	if ( !strncmp( coding, line, strlen( coding ) ) ) {
		return TRUE;
	}
	if ( !strncmp( coding, line + 1, strlen( coding ) ) && ( *line == '.' ) ) {
		return TRUE;
	}
	return FALSE;
}

static int smi_get_time( char *line )
{
	if ( !line )
		return -1;
	line = strchr( line, '=' );
	if ( !line || ( strlen( line ) < 2 ) )
		return -1;
	line++;
	if ( *line == '"' )
		line++;
	return atoi( line );
}

static char *next_line( char **data, int *line )
{
	(*line)++;
	if( *line < SMI_ARRAY_SIZE ) {
		return data[*line];
	}
	return NULL;
}

/*******************
 * Input: 
 * <Sync Start=XXXX>
 * <P Class=ZXY> Text
 * <P Class=ZXY> More text
 * <P Class=ABC> Text in other language
 * <Sync Start=ZZZZ><P Class=ZXY>&nbsp (this line is not required)
 * </SYNC> (optional)
 *******************/
static sub_line *extract_smi_line( char **data, char *coding, int clean_tags, int utf8 )
{
#if 0
	int i;
	for ( i = 0; i < SMI_ARRAY_SIZE && data[i]; i++ ) {
		serprintf( "[%d] '%s'\n", i, data[i] );
	}
	serprintf("lines: %d\n", i);
#endif

	int line = 0;
	//extract start_time. First line always contains <Sync Start=xy> or there is something wrong
	sub_line *new_line = acalloc(1, sizeof( sub_line ) );
	new_line->start = smi_get_time(data[line]);
	
	if ( new_line->start == -1 ) {
		DBG serprintf( "Subtitle: SMI: extract_smi_line: CODING ERROR in line %s\n", data[0] );
		goto ERROR_CLEAN;
	}
	char *tmp = strchr( data[0], '>' );
	if ( !tmp ) {
		DBG serprintf( "Subtitle: SMI: extract_smi_line: CODING ERROR in line %s\n", data[0] );
		goto ERROR_CLEAN;
	}
        tmp++;
        //ignore empty spaces
        char *tmp_line = clear_empty(tmp);
        //loop through every line and acquire lines that have <P Class=coding>
        if(!tmp_line)     {
		tmp_line = data[++line];
	}
        if(!tmp_line)     {
		goto ERROR_CLEAN;
	}
	if ( strlen( tmp_line ) < 5 ) {
		tmp_line = data[++line];
	}
	
	int get_line = 0;
	while ( line < SMI_ARRAY_SIZE && tmp_line && strncmpNC( tmp_line, "<Sync Start=", 12 )) {
		//when next sync start comes or
		//if not syncstart then it should be EITHER <P Class > or data for previous lines P Class
		if ( !strncmpNC( tmp_line, "<P Class=", 8 ) ) {
			if (smi_match_coding(coding,tmp_line + 9)){
				tmp_line = strchr( tmp_line, '>' );
				if ( !tmp_line ) {
					DBG serprintf( "Subtitle: SMI: extract_smi_line: Syntax error in %s\n", data[line] );
					goto ERROR_CLEAN;
				}
				tmp_line++;
				if ( strstr( tmp_line, "<br" ) ){
					handle_linebreak( tmp_line, new_line, clean_tags );
				} else{
					store_text_line( new_line, tmp_line, clean_tags, utf8 );
				}
				get_line = 1;	//get next line if it has no P Class definition
				tmp_line = next_line( data, &line );
				continue;
			} else {
				//some other coding. Iqnore
				get_line = 0;	//if next line has no P Class definition, ignore it
				tmp_line = next_line( data, &line );
				continue;
			}
		} else {
			//some SMI files use /SYNC to end text fields. Handle here
			if(strstrNC(tmp_line,"</SYNC>")){
				tmp_line = 0;
				break;
			}
			//runaway from previous line
			if ( !get_line ) {	//previous P Class was wrong coding. Skip this one
				tmp_line = next_line( data, &line );
				continue;
			}
			//Previous line was right coding.
			//check if there is <br>
			tmp = strstr( tmp_line, "<br" );
			if ( tmp ) {
				handle_linebreak( tmp_line, new_line, clean_tags );
			} else {
				store_text_line( new_line, tmp_line, clean_tags, utf8 );
			}
		}
		tmp_line = next_line( data, &line );
	}

	//the title line ends here. Extract the end time if its there
	//no end time for this frame. Fill it later
	if ( !tmp_line ) {
		return new_line;
	}
	//end time is here. Use it
	tmp = strchr( tmp_line, '=' );
	if ( !tmp ) {
		DBG serprintf( "Subtitle: SMI: extract_smi_line: Syntax error in terminating sync:  %s\n", data[line] );
		goto ERROR_CLEAN;
	}
	if(*tmp == '"')
		tmp++;
	new_line->end = atoi( tmp + 1 );
	return new_line;

ERROR_CLEAN:

	DBG serprintf( "Subtitle:SMI:extract_smi_line: Errors encountered. Bailing out\n" );

	if ( new_line->top ) {
		afree( new_line->top );
	}
	if ( new_line->bottom ) {
		afree( new_line->bottom );
	}
	afree( new_line );
	return 0;
}

static inline void clean_smi_array( char** data)
{
	int i = 0;
	for ( i = 0; i < SMI_ARRAY_SIZE; ++i ) {
		afree(data[i]);
		data[i] = 0;
	}
}

//cleans new lines and cursor begin characters from line
static inline char *strip_smi_line( char **line )
{
	char *tmp;
	tmp = strchr( *line, MS_CURSOR_BEGIN );
	if ( tmp ) {
		*tmp = '\0';
	}
	tmp = strchr( *line, NEW_LINE_CH );
	if ( tmp ) {
		*tmp = '\0';
	}
	tmp = *line;
	while(isspace(*tmp)) {
		tmp++;
		if(!tmp)
			return 0;
	}
	return tmp;
}

char *getNextLine(char* start, int len, FILE* fd)
{
	char *ret = fgets(start, len, fd);
	if(strlen(ret) >= LINE_LEN - 1){
		if(strchr(ret,'\n')){
			return ret;
		} else{
			//line did not fit into inputbuffer. Its propably
			//filled with crap. Read until newline so it wont
			//mess the next reads
			char* tmp = NULL;
			do {
				tmp = fgets(start,len,fd);
			} while(!strchr(tmp,'\n'));
			ret = start;
			start = strchr(start,'>');
			if(start) {
				*(start + 1) = ' ';
				*(start + 2) = '\0';
			}
		}
	}
	return ret;
}

/***************
 * Parses SMI file format
 * subs->filename must exists
 * Returns uni_sub* which contains all the subtitles
 * in linked list
 * 
 * **********/
static uni_sub *parse_SMI( subt_orig *subs, int clean_tags )
{
	FILE *file;
	char _line_read[LINE_LEN + 1];
	char* line_read = _line_read;
	char *line_read_start = line_read;
	sub_line *new_line = 0;
	int line = 0;
	int cmd = 0;
	char** cmd_lines = 0;
	int i;	
	uni_sub *sub_record = 0;
	if ( !subs->filename ) {
		DBG serprintf( "Subtitle: parseSMI: <NULL> filename>\n" );
		return 0;
	}
	file = fopen( subs->filename, "r" );
	if ( !file ) {
		DBG serprintf( "subtitle: parseSMI: could not open file %s\n", subs->filename );
		return 0;
	}
	//make sure ID is there and valid. SMI header is based on it.
	subs->default_language = set_SMI_code( subs, file );
	if ( !subs->default_language ) {
		DBG serprintf( "Subtitle: parseSMI: Could not get/quess the language selection\n" );
		return 0;
	}
	
	sub_record = acalloc(1, sizeof( uni_sub ) );
	
	fseek( file, 0, SEEK_SET );
	line_read = getNextLine( line_read_start, LINE_LEN, file );
	//skip the header
	while ( !feof( file ) ) {

		//<BODY> statement starts the actual data. Header is already read
		if ( strstrNC( line_read, "<BODY>" ) ) {
			break;
		}
		line_read = getNextLine( line_read_start, LINE_LEN, file );

	}
	line_read = getNextLine( line_read_start, LINE_LEN, file );
	if(!line_read){
		goto CLEAN_ERROR;
	}
	cmd_lines = acalloc( SMI_ARRAY_SIZE, sizeof( char * ) );
	if(!cmd_lines)
		goto CLEAN_ERROR;

	clean_smi_array(cmd_lines);
	while ( !feof( file ) ) {
		line++;
		//Check for end of BODY
		if ( strstrNC( line_read, "</BODY>" ) ) {
			break;
		}
		if ( strstrNC( line_read, "Sync Start" ) ) {
			if ( cmd == 0 ) {
				line_read = strip_smi_line( &line_read );
				if(*line_read != 0){
					cmd_lines[0] = astrdup(line_read);
					cmd++;
				}
				line_read = getNextLine( line_read_start, LINE_LEN, file );
				continue;
			}
				
			//current data entry ends to this line. Here ending time is indicated by sync start of &nbsp line
			if ( strstrNC( line_read, "&nbsp" ) || strstrNC(line_read,"</SYNC>")) {
				line_read = strip_smi_line( &line_read );
				cmd_lines[cmd] = astrdup( line_read);
				new_line = extract_smi_line( cmd_lines, subs->default_language, clean_tags, subs->utf8 );
				//store gathered data to sub_record
				if ( new_line ) {
					if ( !new_line->bottom && !new_line->top ) {
						//all lines were for different language. Skip this
						afree( new_line );
						new_line = 0;
						clean_smi_array( cmd_lines );
						cmd = 0;
						line_read = getNextLine( line_read_start, LINE_LEN, file );
						continue;
					}
					sub_record = store_title(sub_record,new_line);
					new_line = 0;
				}
				clean_smi_array( cmd_lines );
				cmd = 0;
				line_read = getNextLine( line_read_start, LINE_LEN, file );
				continue;
			}
			//new data entry begins. Store what came this far. If here then the previous frame
			//has no ending time. Add it there
			if ( cmd != 0 ) {
				if(cmd == 1){
					//there was no data to store. propably syntax error in file
					//or two consecutive SYNC Start's
					clean_smi_array( cmd_lines );
					cmd = 1;
					line_read = strip_smi_line( &line_read );					
					cmd_lines[0] = astrdup( line_read );
					
					line_read = getNextLine( line_read_start, LINE_LEN, file );					
					continue;
				}
					      
				new_line = extract_smi_line( cmd_lines, subs->default_language, clean_tags, subs->utf8 );
				
				//store gathered data to sub_record
				if ( new_line ) {
					sub_record = store_title(sub_record,new_line);
					new_line = 0;
				} else {
					// do not stop here, could be a comment, just go on
					//goto CLEAN_ERROR;
				}
				cmd = 1;
				clean_smi_array( cmd_lines );
				//Store array here
				line_read = strip_smi_line( &line_read );				
				cmd_lines[0] = astrdup(line_read);
				line_read = getNextLine( line_read_start, LINE_LEN, file );
				continue;
			}
		}
		
		line_read = strip_smi_line( &line_read );
		if(*line_read != 0){
			cmd_lines[cmd++] = astrdup(line_read);
		}
		line_read = getNextLine( line_read_start, LINE_LEN, file );
	}
	if(cmd != 0){
		new_line = extract_smi_line(cmd_lines, subs->default_language, clean_tags, subs->utf8 );
		if(new_line){
			store_title(sub_record, new_line);
		}
	}
	for(i = 0; i < SMI_ARRAY_SIZE; ++i){
		afree(cmd_lines[i]);
	}
	afree(cmd_lines);
	fclose( file );
	if ( !sub_record ) {
		return 0;
	}
	if ( !sub_record->first ) {
		return 0;
	}
	compress_times( sub_record );
	if ( !sub_record ) {
DBG serprintf( "SMI parsing failed. Null record\n" );
		return 0;
	}
	if ( !sub_record->first ) {
DBG serprintf( "SMI parsing failed. No records\n" );
		return 0;
	}

	sub_record->frame_multiplier = 0;
	return sub_record;
CLEAN_ERROR:
	afree(sub_record);
	if(cmd_lines != 0){
		for(i = 0; i < SMI_ARRAY_SIZE;++i){
			afree(cmd_lines[i]);
		}
		afree(cmd_lines);
	}
	subtitle_clean_error(sub_record);
	return 0;
}

static struct SUBTITLE_FORMAT SMI = {
	"SAMI",
	detect_SMI,
	info_SMI,
	parse_SMI,
};

SUBTITLE_REGISTER_FORMAT( SMI );
