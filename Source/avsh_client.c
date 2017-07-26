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

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>

#ifdef CONFIG_MEDIACENTER
#define SERVER  "/data/data/com.archos.mediacenter.videoti/files/avsh.sock"
#define CLIENT  "/data/data/com.archos.mediacenter.videoti/files/avshclient.sock"
#else
#define SERVER  "/tmp/avshsocket"
#define CLIENT  "/tmp/mysocket"
#endif

#define MAXMSG  512
 
int make_named_socket( const char *filename )
{
	struct sockaddr_un name;
	int sock;
	size_t size;

	/* Create the socket. */
	sock = socket( PF_LOCAL, SOCK_DGRAM, 0 );
	if ( sock < 0 ) {
		perror( "socket" );
		exit( EXIT_FAILURE );
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
		fprintf(stderr, "error with %s\n", filename );
		perror( "bind" );
		exit( EXIT_FAILURE );
	}

	return sock;
}

static void handle_error()
{
	if (errno == ENOENT) {
#ifdef CONFIG_MEDIACENTER
		fprintf(stderr, "error: MediaCenter not running\n");
#else
		fprintf(stderr, "error: avos not running\n");
#endif
	} else {
		perror( "sendto (client)" );
	}
	exit( EXIT_FAILURE );
}

int main( int argc, char **argv )
{
	extern int make_named_socket( const char *name );

	/* Remove the filename first, it's ok if the call fails */
	unlink( CLIENT );

	/* Make the socket. */
	int sock = make_named_socket( CLIENT );

	/* Initialize the server socket address. */
	struct sockaddr_un name;
	name.sun_family = AF_LOCAL;
	strcpy( name.sun_path, SERVER );
	size_t size = strlen( name.sun_path ) + sizeof( name.sun_family );

#ifdef CONFIG_MEDIACENTER
	if ( sendto( sock, "ping", strlen("ping") +1 , 0, ( struct sockaddr * ) &name, size ) < 0 ) {
		printf("launching MediaCenter...\n");
		// start MediaCenter process and init libavos
		system("am broadcast -a com.archos.mediacenter.DEBUG");
	}
#endif

	if( argc > 1 ) {
		char line[1024] = "";
		char *l = line;
		int i;
		for( i = 1; i < argc; i ++ ) {
			if( i > 1 )
				strcat( l, " " );
			strcat( l, argv[i] );
		}
printf("Sending: [%s]\n", line );
		int nbytes = sendto( sock, line, strlen( line ) + 1, 0, ( struct sockaddr * ) &name, size );
		if ( nbytes < 0 ) {
			handle_error();
		}
	} else {
		while( 1 ) {
			char *msg = NULL;
		 	size_t len;
printf(">");		
#ifdef BIONIC
			const char *line = fgetln(stdin, &len);
			msg = realloc(msg, len);
			strncpy(msg, line, len);
			// replace \n by \0
			msg[len - 1] = '\0';
#else
			getline( &msg, &len, stdin);
			// remove the trailing \n
			msg[strlen(msg) - 1] = '\0';
#endif

			// exit on Q
			if( !strcmp( msg, "q" ) ) {
				// "q" exits
				break;
			}

			if( !strcmp( msg, "" ) ) {
				// just emit an empty line
printf("\n");
			} else {
printf("Sending: [%s]\n", msg);
				/* Send the datagram. */
				int nbytes = sendto( sock, msg, strlen( msg ) + 1, 0, ( struct sockaddr * ) &name, size );
				if ( nbytes < 0 ) {
					handle_error();
				}
			}
			free( msg );
		}
	}

	/* Clean up. */
	remove( CLIENT );
	close( sock );
	return 0;
}
