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
#include "util.h"
#include "debug.h"
#include "tty.h"
#include "app_key.h"
#include "key.h"
#include "timers.h"
#include "message.h"

#include <ctype.h>

#ifdef DEBUG_MSG

//#define TERMINAL_WITHOUT_ESC_SEQUENCES
#define MAX_LINE		512
#define PROMPT ">"
#define CR '\r'
#define LF '\n'

static char     current_cmd[MAX_LINE];
static unsigned char cmd[16][MAX_LINE];
static int 	len[16];
static int 	stack = 0;

static int 	cmd_row = 0;
static int 	cmd_column = 0;
static int 	seq;
static int 	stackcnt;
static int 	newline = 1;

static void debug_putc( char c)
{
	char s[2] = { c, '\0' };
	TTY_write( s );
}

static inline void clear_line(int len)
{
	int z;
	debug_putc(CR);
	for(z = 0; z < len; ++z)  
		debug_putc(' ');
}

#if defined( CONFIG_GUI ) || defined( CONFIG_CLI )
static int is_parameter_mode( void )
{
	int i;
	int state = 0;
	unsigned char c;
	
	if( cmd[cmd_row][0] == '!' )
		return 1;
		
	for (i = 0; i < len[cmd_row]; i++)
	{
		c = cmd[cmd_row][i];
		
		if (state == 0 && (!isspace(c)))
			state = 1;
		else if (state == 1 && isspace(c))
			return 1;	
	}
	
	return 0;
}

typedef struct SPECIAL_KEY {
	char	c;
	int	key;
} SPECIAL_KEY;

static const SPECIAL_KEY special_key[] =
{
	'5',	KEY_OK,
	'%',	KEY_OK_L,
	'4',	KEY_LEFT,
	'$',	KEY_LEFT_L,
	'6',	KEY_RIGHT,
	'&',	KEY_RIGHT_L,
	'8',	KEY_UP,
	'(',	KEY_UP_L,
	'2',	KEY_DOWN,
	'"',	KEY_DOWN_L,
	'0',	KEY_MENU,
	'=',	KEY_MENU_L,
	'3',	KEY_TAB,
	0247,	KEY_TAB_L,
	'9',	KEY_STOP,
	')',	KEY_STOP_L,
	'7',	KEY_ON,
	'/',	KEY_ON_L,
	'+',	KEY_PGUP,
	'-',	KEY_PGDOWN,
	'*',	KEY_TVLCD,
	'<',	KEY_VOLUP,
	'>',	KEY_VOLDOWN,
	',',	KEY_HOME,
};

static int is_special( char c )
{
	int i;
	for ( i = 0; i < sizeof( special_key ) / sizeof( SPECIAL_KEY ); i++ )
		if( special_key[i].c == c ) {
			return special_key[i].key;
		}
	
	return -1;
}

static int simulate_long_presses_on_sim = 0;

static void _simulate_long_presses_on_sim( void )
{
	simulate_long_presses_on_sim = 1 - simulate_long_presses_on_sim;
serprintf("long presses are %s\r\n", simulate_long_presses_on_sim ? "ON" : "OFF");
}

DECLARE_DEBUG_COMMAND_VOID( "slp", _simulate_long_presses_on_sim );

__attribute__((unused))
static void _simulate_release( size_t key )
{
	handle_key( key, 0, NULL, 1, KBD_DEVICE );
}
#endif

void cli_handle_key( int key ) ;

void CONSOLE_handle_char( unsigned char byte )
{
	int do_cmd = 1;

	if (newline) {
		cmd_column = 0;
		len[cmd_row] = 0;
		seq = 0;
		stackcnt = 0;
		cmd[cmd_row][cmd_column] = 0;
		newline = 0;
	}

	if (!is_parameter_mode()) {
		size_t key = is_special( byte );
		if( key != -1) {
#ifdef CONFIG_GUI
			if( simulate_long_presses_on_sim ){
				static int last_key = -1;
				static int simulate_timer_id = -1;
				// This kindof simulate key release coming from the terminal.
				// If no key is pressed during 100ms, then we assume it was a press and release
				// It works but introduces a 100ms in a lot of places :-(
				if( key == last_key ){
					Timers_Remove( &gui_timers, &simulate_timer_id );
				}
				last_key = key;

				handle_key( key, 1, NULL, 1, KBD_DEVICE );
				simulate_timer_id = Timers_AddWithParam( &gui_timers, (timer_cb)_simulate_release, (void*)key, 500, Single );
			} else {
				KEY_MESSAGE( key, MS_KEY_PRESSED, KBD_DEVICE );
				KEY_MESSAGE( key, MS_KEY_RELEASED, KBD_DEVICE );
			}
#endif
#ifdef CONFIG_CLI
				cli_handle_key( key );
#endif
			return;
		}
	}
//serprintf("[%02X]", byte );
	switch (byte) {
	    case 10: // ESC 
	   	byte = 13;
		break;
		
	    case 27: // ESC 
		if (seq != 0) {
		    seq = 0;
		    break;
		}
		seq++;
		return;
	
	    case 91: //  [ 
		if (seq != 1) {    
		    seq = 0;
		    break;
		}
		seq++;
		return;

	    case 65: {//  arrow up 
		int i_old;
		if (seq != 2) {
		    seq = 0;
		    break;
		}
    
		seq = 0;
		
		if (stackcnt == stack)
		    return;
		
		i_old = cmd_row;
		cmd_row--;
		cmd_row &= 0xf;
		cmd_column = len[cmd_row];
		if(len[i_old] > len[cmd_row])
			clear_line(len[i_old]+1);
		serprintf("%c%s%s", CR, PROMPT, &cmd[cmd_row][0]);
		
		stackcnt++;    
		return;
	    }
	    
	    case 66: { // arrow down 
		int i_old;
		if (seq != 2) {
		    seq = 0;
		    break;
		}
    
		seq = 0;
		
		if (stackcnt == 0)
		    return;
		
		i_old = cmd_row;
		cmd_row++;
		cmd_row &= 0xf;
		cmd_column = len[cmd_row];
		if(len[i_old] > len[cmd_row])
			clear_line(len[i_old]+1);
		serprintf("%c%s%s", CR, PROMPT, &cmd[cmd_row][0]);
		
		stackcnt--;    
		return;
	    }
	    
	    case 67: // arrow right 
		if (seq != 2) {
		    seq = 0;
		    break;
		}
    
		seq = 0;
		
		if (len[cmd_row] == cmd_column)
		    return;

		cmd_column++;
#ifndef TERMINAL_WITHOUT_ESC_SEQUENCES		    
		debug_putc(27);
		debug_putc(91);
		debug_putc(67);
#else
		serprintf("%c%s", CR, PROMPT); {
			int z;
			for (z=0; z<cmd_column; z++)
				debug_putc(cmd[cmd_row][z]);
		}
#endif
		return;

	    case 68: // arrow left 
		if (seq != 2) {
		    seq = 0;
		    break;
		}
		
		seq=0;

		if (cmd_column == 0)
		    return;
		
		cmd_column--;    
#ifndef TERMINAL_WITHOUT_ESC_SEQUENCES
		debug_putc(27);
		debug_putc(91);
		debug_putc(68);		
#else
		serprintf("%c%s", CR, PROMPT);
		{
			int z;
			for (z=0; z<cmd_column; z++)
				debug_putc(cmd[cmd_row][z]);
		}
#endif
		return;

    	    case 51:
		if (seq != 2) {
		    seq = 0;
		    break;
		}

		seq = 0;
		debug_putc(7);
		debug_putc(51);
		return;

	    case 8: // backspace 
	    case 0x7F: // DEL 
		if (cmd_column == 0)
		    return;
	
		if (len[cmd_row] == cmd_column) {
		    --cmd_column;
		    --len[cmd_row];
		    debug_putc(8);
		    debug_putc(' ');
		    debug_putc(8);
		    cmd[cmd_row][cmd_column] = 0;
		} else {
		    int z;
		    --cmd_column;
		    memmove(&cmd[cmd_row][cmd_column], &cmd[cmd_row][cmd_column+1], len[cmd_row]-cmd_column);    
		    serprintf("%c%s  %c", 8, &cmd[cmd_row][cmd_column], CR);
#ifndef TERMINAL_WITHOUT_ESC_SEQUENCES
		    for (z=0; z<cmd_column+1; ++z)
		    {
			debug_putc(27);
			debug_putc(91);
			debug_putc(67);
		    }
#else
		    serprintf("%c%s", CR, PROMPT);
		    			
		    for (z=0; z<cmd_column; z++)
			debug_putc(cmd[cmd_row][z]);
		   
#endif
		 
		    --len[cmd_row];
		}
		return;
					
	    default: 
		seq = 0;
	    	break;
	    }

	    if (byte == 13) { // return 
		debug_putc(CR);
		debug_putc(LF);
		
		/* same command as last */
		int prev_cmd_row = (cmd_row == 0)?15:cmd_row-1;
		if (stack > 0 && len[cmd_row] == len[prev_cmd_row]
		     && (memcmp(&cmd[cmd_row][0], &cmd[prev_cmd_row][0], len[cmd_row]) == 0)) {
		    cmd_column = 0;
		    goto do_command;
		}
		
		if (stackcnt == 1) {
		    cmd_row++;
		    cmd_row &= 0xf;
		    cmd_column=0;
		    len[cmd_row] = 0;
		    goto do_command;
		}
		
		if (stackcnt > 1) {
		    int i_old = cmd_row;
		    cmd_row += stackcnt;
		    cmd_row &= 0xf;
		    	    
		    memcpy(&cmd[cmd_row][0], &cmd[i_old][0], len[i_old]+1);
		    len[cmd_row] = len[i_old];
		}    
		
		if (cmd[cmd_row][0] == 0) {
			do_cmd = 0;
			goto do_command;
		}
		
		cmd_row++;		
		cmd_row &= 0xf;
		
		if (stack < 16)
		    stack++;
		cmd_column=0;
		
		len[cmd_row] = 0;
		
do_command:
		if (do_cmd) {
			int prev_cmd_row = (cmd_row == 0)?15:cmd_row-1;
			strcpy ( current_cmd, &cmd[prev_cmd_row][0] );
			debug_do_cmd(&cmd[prev_cmd_row][0]);
		}
		serprintf(PROMPT);
		newline = 1;
		return;
	    }
    
	    if (len[cmd_row] == cmd_column) {
		cmd[cmd_row][cmd_column] = byte;	    
		debug_putc(cmd[cmd_row][cmd_column]);
		cmd_column++;
		len[cmd_row]++;
		cmd[cmd_row][cmd_column] = 0;
	    } else {
		int z;
		memmove(&cmd[cmd_row][cmd_column+1], &cmd[cmd_row][cmd_column], len[cmd_row]-cmd_column+1);    
		cmd[cmd_row][cmd_column] = byte;
		serprintf("%c%s%s  %c", CR, PROMPT, &cmd[cmd_row][0], CR);
		cmd_column++;
		len[cmd_row]++;
#ifndef TERMINAL_WITHOUT_ESC_SEQUENCES
		for (z=0; z<cmd_column+1; ++z) {
			debug_putc(27);
			debug_putc(91);
			debug_putc(67);
		}
#else
		serprintf("%c%s", CR, PROMPT);
		    			
		for (z=0; z<cmd_column; z++)
			debug_putc(cmd[cmd_row][z]);		   
#endif
	    }
}

#else
void APP_SerialPort( unsigned char byte ){}
#endif
