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

#ifndef _REDIRECT_LIST_PARSER_H
#define _REDIRECT_LIST_PARSER_H

typedef struct {
	char *p;
	size_t read; // reading position of the buffer; starts at 0 and incremented after each read
	size_t write; // writing position of the buffer; starts at 0 and incremented after each write
	size_t size; // total size allocat'ed for the buffer
} RedirectListBuf;

typedef struct RedirectListEntry {
	char *url;
	char *mime;
	int type;
	int etype;
	RedirectListBuf buf;
	struct RedirectListEntry *next;
} RedirectListEntry;

typedef int (*RedirectListCallback) ( void *ctx, RedirectListEntry *entry );
typedef int (*RedirectListAbortCallback) ( void *ctx );

typedef struct {
	int num_entries;
	RedirectListEntry *head;
	RedirectListEntry *tail;
	
	RedirectListCallback cb;
	RedirectListAbortCallback abort_cb;
	
	void *cb_ctx;
} RedirectList;

int RedirectListParser_read(RedirectList *list, const char *path);
int RedirectListParser_read_with_cb(RedirectList *list, const char *path, RedirectListCallback cb, RedirectListAbortCallback abort_cb, void *cb_ctx);
void RedirectListParser_cleanup(RedirectList *list);

#endif
