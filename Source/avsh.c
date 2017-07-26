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

#include "avos_lifetime.h"
#include "global.h"
#include "debug.h"
#include "dataevent.h"

#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef CONFIG_MEDIACENTER
#define SERVER  "/data/data/com.archos.mediacenter.videoti/files/avsh.sock"
#else
#define SERVER  "/tmp/avshsocket"
#endif

#define MAXMSG  512

static data_event_t socket_data_event;

int make_named_socket( const char *filename )
{
	struct sockaddr_un name;
	int sock;
	size_t size;

	/* Create the socket. */
	sock = socket( PF_LOCAL, SOCK_DGRAM, 0 );
	if ( sock < 0 ) {
		perror( "socket" );
		return -1;
	}

	/* Bind a name to the socket. */
	name.sun_family = AF_LOCAL;
	strncpy( name.sun_path, filename, sizeof( name.sun_path ) );
	name.sun_path[sizeof( name.sun_path ) - 1] = '\0';

	/* The size of the address is
	   the offset of the start of the filename,
	   plus its length,
	   plus one for the terminating null byte.
	   Alternatively you can just do:
	   size = SUN_LEN (&name);
	 */
	size = ( offsetof( struct sockaddr_un, sun_path )
		 + strlen( name.sun_path ) + 1 );

	if ( bind( sock, ( struct sockaddr * ) &name, size ) < 0 ) {
		perror( "bind" );
		return -1;
	}

	return sock;
}

static void socket_callback(void *p)
{
	data_event_t *socket_data_event = (data_event_t*)p;

	struct sockaddr_un name;
	socklen_t size = sizeof( name );
	char message[MAXMSG];
	
	int nbytes = recvfrom( socket_data_event->fd, message, MAXMSG, 0, ( struct sockaddr * ) &name, &size );
	if ( nbytes < 0 ) {
		serprintf( "avsh: error recfrom\r\n" );
	} else {
serprintf( "avsh: %s\r\n", message );
#ifdef DEBUG_MSG
		debug_do_cmd( message );
#endif
	}
}

void avsh_init( void )
{
	/* Remove the filename first, it's ok if the call fails */
	unlink( SERVER );

	/* Make the socket, then loop endlessly. */
	int sock_fd = make_named_socket( SERVER );
	if( sock_fd == -1 ) {
		serprintf("avsh: failed to make the socket!\r\n");
		return;
	}
		
	// we wan't to be informed when somebody connects to our comm-socket
	memset(&socket_data_event, 0, sizeof(socket_data_event));
	socket_data_event.fd        = sock_fd;
	socket_data_event.read_func = socket_callback;
	register_data_event(&mainloop_events, &socket_data_event, &socket_data_event);

serprintf( "avsh: socket open: %d\r\n", sock_fd);
}
AVOS_REGISTER_INIT_RUNLEVEL(avsh_init, AVOS_RUNLEVEL_APP);
AVOS_REGISTER_CLEAN_FILE(avsh, SERVER);
