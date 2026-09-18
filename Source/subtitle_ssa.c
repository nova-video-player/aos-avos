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
#include "i18n.h"
#include "util.h"
#include "astdlib.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <limits.h>

#define DBG if(Debug[DBG_SUB])

#define SS_TO_MS(x) ((x)*1000)
#define MM_TO_MS(x) ((SS_TO_MS(x))*60)
#define HH_TO_MS(x) ((MM_TO_MS(x))*60)

/************************
 * Function: subtitle_get_sub_time
 * input: timeline of sub-formatted subtitle
 * output: array of start and endtime in int
 * 
 * Converts the given string to start end endtime.
 * The string must be in "{XX}{YY}ZTY" where XX and YY are int
 * *********************/
#define USED_ID 4
#define SKIP_SPACE(x) while(isspace(*x)){x++;}
#define Xfgets(str,len,fd) do { \
	do { str = fgets(str, len, fd); } \
	while (str && (*str == '\r' || *str == '\n' || !*str)); \
} while (0);


char *subtitle_get_next_line( char *start, int len, FILE *fd );

static const char* rel_str[USED_ID] ={
	"Start","End", "Text", NULL
};
		
static void ssa_clean_line(char* line,int rep){
	char* tmp = strchr(line,'\r');
	if(tmp){
		*tmp = rep;
	}
}

static int detect_SSA( FILE *file )
{
	//make sure that read starts at the beginning of file
	fseek( file, 0, SEEK_SET );
	char _line[ LINE_LEN + 1 ];
	char* line = _line;
	line = fgets( line, LINE_LEN, file );
	while( line && (*line == '\r' || *line == '\n') ){//skip possible empty lines
		line = fgets( line, 83, file );
	}
	if(line && strstrNC(line,"[Script Info]")){
DBG serprintf( "SSA: found!\n" );
		return 0;
	}
DBG serprintf( "SSA: not SSA\n" );
	return 1;

}

static int *ssa_pick_relevant(char *line)
{
	if (!line) return NULL;
	char *start = strchr(line, ':');
	if (!start) return NULL;
	int *used = amalloc(USED_ID * sizeof(*used));
	if (!used) return NULL;
	for (int i = 0; i < USED_ID; ++i) used[i] = -1;
	int column = 0;
	for (++start; *start; ++column) {
		while (isspace((unsigned char)*start)) ++start;
		char *end = strchr(start, ',');
		char *next = end ? end + 1 : start + strlen(start);
		if (!end) end = next;
		while (end > start && isspace((unsigned char)end[-1])) --end;
		for (int i = 0; i < USED_ID - 1; ++i) {
			if ((size_t)(end - start) == strlen(rel_str[i]) &&
			    !strncmp(start, rel_str[i], end - start)) {
				if (used[i] >= 0) { afree(used); return NULL; }
				used[i] = column;
			}
		}
		start = next;
	}
	// The line reader consumes these fields in Start, End, Text order.
	if (used[0] < 0 || used[1] <= used[0] || used[2] <= used[1]) {
		afree(used);
		return NULL;
	}
	return used;
}

static void ssa_handle_text(sub_line *sb, char *line, int utf8)
{
	char *tmp;
// ***********
// Remove styling and convert to utf8 here!
// **********		
#ifdef CONFIG_I18N
	if( !utf8 ) {
		wchar unicode[ LINE_LEN + 1 ];
		memset(unicode, 0, LINE_LEN);
		wchar *uc = unicode;
		char* c= line;
		while(*c){
			c+= I18N_codepage_to_unicode(c, uc);
			uc++;
		}
		utf16_to_utf8( line, unicode, LINE_LEN);
	}
#endif //CONFIG_I18N
	ssa_clean_line(line,'\0');
	tmp = strstr(line,"\\N");
	if(!tmp) {

		sb->top = astrdup(line);
	}
	else{
		tmp += 2;
		sb->top = acalloc(tmp-line + 1, 1);
		strncpy(sb->top,line,tmp-line-2);
		sb->bottom = astrdup(tmp);
	}
	return;
}

static int calc_time(char *line)
{
	unsigned hh, mm, ss, cs;
	if (sscanf(line, "%u:%u:%u.%u", &hh, &mm, &ss, &cs) != 4 ||
	    mm >= 60 || ss >= 60 || cs >= 100) return -1;
	int64_t time = ((int64_t)hh * 3600 + mm * 60 + ss) * 1000 + cs * 10;
	return time <= INT_MAX ? (int)time : -1;
}

static sub_line *ssa_handle_line(char *line, int *fields, int utf8)
{
	while (isspace((unsigned char)*line)) ++line;
	if (strncasecmp(line, "Dialogue:", 9)) return NULL;
	char *text = line + 9;
	int start = -1, end = -1;
	for (int field = 0; field < fields[2]; ++field) {
		char *comma = strchr(text, ',');
		if (!comma) return NULL;
		*comma = 0;
		if (field == fields[0]) start = calc_time(text);
		if (field == fields[1]) end = calc_time(text);
		text = comma + 1;
	}
	if (start < 0 || end <= start) return NULL;
	sub_line *cue = acalloc(1, sizeof(*cue));
	if (!cue) return NULL;
	cue->start = start;
	cue->end = end;
	ssa_handle_text(cue, text, utf8);
	return cue;
}

static uni_sub *parse_SSA( subt_orig *spex, int clean_tags )
{
	if(!spex){
		return 0;
	}
	if(!spex->filename){
		return 0;
	}
	FILE *fd = fopen(spex->filename,"r");
	if(!fd){
		DBG serprintf("Subtitle: parse_SSA, could not open file %s\n",spex->filename);
		return 0;
	}
	char _line[LINE_LEN];
	char *line = _line;
	//go to begin on events
	while(line){
		line = subtitle_get_next_line(line, LINE_LEN,fd);
		if(line && strstrNC(line,"[Events]")){
			memset(line,0,LINE_LEN);
			Xfgets(line, LINE_LEN,fd)			

			break;
		}
	}
	int *relevant = ssa_pick_relevant(line);
	if(!relevant){
		fclose(fd);
		DBG serprintf("Subtitle: parse_SSA, could not obtain format data\n");
		return 0;
	}
	uni_sub *sub_record = acalloc(1,sizeof(uni_sub));

	line = subtitle_get_next_line(_line, LINE_LEN,fd);
	while(line){
		sub_line *new_line = ssa_handle_line(line,relevant, spex->utf8);
		if(new_line == 0){
			Xfgets(line, LINE_LEN,fd)
			continue;
		}
		if(!sub_record->first){
			sub_record->first = new_line;
			sub_record->last  = new_line;
		} else {
			if( new_line->start >= sub_record->last->start ) {
				// add at end
				sub_record->last->next = new_line;
				new_line->prev         = sub_record->last;
				sub_record->last       = new_line;
			} else {
				// insertion in list
				sub_line *l = sub_record->last->prev;
				while( l && new_line->start < l->start ) {
					l = l->prev;
				}

				if( !l ) {
					// insert at the start
					sub_record->first->prev = new_line;
					new_line->next          = sub_record->first;
					sub_record->first       = new_line;
				} else {
					// insert at l
					new_line->next  = l->next;
					l->next->prev   = new_line;
					new_line->prev  = l;
					l->next         = new_line;
				}
			}
		}
		memset(line,0,LINE_LEN);
		Xfgets(line, LINE_LEN,fd)
	}
	fclose(fd);
	if(!sub_record->first){ //nothing could be acquired
		afree(sub_record);
		afree(relevant);
		return 0;
	}
	afree(relevant);
	return sub_record;
}

static struct SUBTITLE_FORMAT SSA = {
	"SubStation",
	detect_SSA,
	NULL,		// no info
	parse_SSA,
};

SUBTITLE_REGISTER_FORMAT( SSA );
