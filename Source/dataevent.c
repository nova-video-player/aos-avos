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

#include "dataevent.h"
#include "debug.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/select.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#ifdef CONFIG_DBUS
#include "dbus.h"
#endif

#define DBG	if (Debug[DBG_DE])
#define DBG2	if (Debug[DBG_DE] > 1)

static int event_list_add_head(data_event_t* lhead, data_event_t* de)
{
	de->next = lhead->next;
	de->prev = lhead;
	lhead->next->prev = de;
	lhead->next = de;
	return 0;
}

static int event_list_del(data_event_t* de)
{
	de->prev->next = de->next;
	de->next->prev = de->prev;

	return 0;
}

int __register_data_event(data_event_t* lhead, data_event_t* de, void* context, const char* from)
{
	if (de->fd < 0)
		return 1;
	
	if ( (!de->read_func) && (!de->write_func) && (!de->except_func) )
		return 1;

	lhead->tainted = 1;
	de->head = lhead;
	
	de->context = context;
DBG	serprintf("%s: fd %i, context %p entry: %p\r\n", __FUNCTION__, de->fd,
		context, de);

	// copy the identifier and make sure it's zero-terminated
	strncpy(de->ident, from, sizeof(de->ident));
	de->ident[sizeof(de->ident)-1] = 0;

	return event_list_add_head(lhead, de);
}

int unregister_data_event(data_event_t* de)
{
DBG	serprintf("%s: fd %i, entry: %p\r\n", __FUNCTION__, de->fd, de);
	de->head->tainted = 1;
	return event_list_del(de);
}

static void prepare_data_events( data_event_t *lhead, fd_set* read_fds, fd_set* write_fds, fd_set* except_fds, int* max_fd )
{
	data_event_t *entry = lhead;

	while ( (entry = entry->next) != lhead ) {
		if (entry->read_func && read_fds)
			FD_SET(entry->fd, read_fds);
		if (entry->write_func && write_fds)
			FD_SET(entry->fd, write_fds);
		if (entry->except_func && except_fds)
			FD_SET(entry->fd, except_fds);
		
		if (max_fd) {
			if (entry->fd > (*max_fd))
				(*max_fd) = entry->fd;
		}
	}
}

static void handle_data_events( data_event_t *lhead, fd_set* read_fds, fd_set* write_fds, fd_set* except_fds )
{
	data_event_t *head = lhead;
	data_event_t *entry, *next;

	for  ( entry = head->next, next = entry->next; entry != lhead;
		      entry = next, next = entry->next )
	{
DBG2		serprintf("%s: fd: %i entry: %p\r\n", __FUNCTION__, entry->fd, entry);
		data_event_t saved = *entry;

		if (read_fds && FD_ISSET(saved.fd, read_fds)) {
DBG			serprintf("%s: read event for fd %i ident %s\r\n", __FUNCTION__, saved.fd, saved.ident);
			if (saved.read_func) {
				saved.read_func(saved.context);
			}
			FD_CLR(saved.fd, read_fds);
			if( head->abort ) {
				head->abort = 0;
				break;
			}
		}
		if (write_fds && FD_ISSET(saved.fd, write_fds)) {
DBG			serprintf("%s: write event for fd %i ident %s\r\n", __FUNCTION__, saved.fd, saved.ident);
			if (saved.write_func) {
				saved.write_func(saved.context);
			}
			FD_CLR(saved.fd, write_fds);
			if( head->abort ) {
				head->abort = 0;
				break;
			}
		}
		if (except_fds && FD_ISSET(saved.fd, except_fds)) {
DBG			serprintf("%s: write event for fd %i ident %s\r\n", __FUNCTION__, saved.fd, saved.ident);
			if (saved.except_func) {
				saved.except_func(saved.context);
			}
			FD_CLR(saved.fd, except_fds);
			if( head->abort ) {
				head->abort = 0;
				break;
			}
		}

		if ( head->tainted ) {
			/* NOTE
			 * The list traversal is safe only against deletion of 'entry' from inside,
			 * but if entry->next is zapped while entry is handled, 'next' is stale.
			 * To work around this limitation, we now maintain a 'tainted' flag for the
			 * list head and a pointer to the list head in each entry. Whenever
			 * the list is manipulated while we traverse it, we restart from the list head,
			 * which is safe against manipulation.
			 */
			next = head->next;
			head->tainted = 0;
		}
	}
}

void service_data_events( data_event_t *lhead, struct timeval* maxwait )
{
	int ret;
	int max_fd = -1;
	fd_set read_fds, write_fds, except_fds;

	// wait for something to happen
	FD_ZERO( &read_fds );
	FD_ZERO( &write_fds );
	FD_ZERO( &except_fds );

	prepare_data_events(lhead, &read_fds, &write_fds, &except_fds, &max_fd);
#ifdef CONFIG_DBUS
	dbus_prepare_data_events(&max_fd, &read_fds, &write_fds, &except_fds, maxwait, dbus_get_main_handle());
#endif
	// blocking wait for something to happen or timeout
	ret = select( max_fd + 1, &read_fds, &write_fds, &except_fds, maxwait );
	if (ret < 0)
	{
		if (errno == EINTR) {
			/* interupted by a signal */
			return;
		}
		serprintf("%s: select error(%d): %s\naborting...\n", __FUNCTION__, errno, strerror(errno));
		kill(getpid(), SIGTERM);
	} else if (ret > 0)
	{
		handle_data_events(lhead, &read_fds, &write_fds, &except_fds);
	}
#ifdef CONFIG_DBUS
	dbus_dispatch(ret, &read_fds, &write_fds, &except_fds, dbus_get_main_handle());
#endif
}

void data_events_dump( data_event_t *lhead )
{
	data_event_t *entry = lhead;

	while ( (entry = entry->next) != lhead ) {
		serprintf("data_event %p - fd %i, context %p, ident %s, %c%c%c\r\n",
			  entry, entry->fd, entry->context, entry->ident,
			  entry->read_func ? 'r' : ' ',
			  entry->write_func ? 'w' : ' ',
			  entry->except_func ? 'x' : ' ');
	}
}

#ifdef DEBUG_MSG
static void dump_mainloop_events(void)
{
	data_events_dump(&mainloop_events);
}
DECLARE_DEBUG_COMMAND_VOID("ded", dump_mainloop_events);
DECLARE_DEBUG_SWITCH("dbgde", DBG_DE);
#endif
