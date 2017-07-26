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

#ifndef LINKED_LIST_H
#define LINKED_LIST_H

// Simple implementation of a double linked list

#include "object.h"


DECLARE_CLASS(LinkedListNode);

void LinkedListNode_init(LinkedListNode *obj);

struct LinkedListNode_str {
	Object s_object;
	LinkedListNode *prev;
	LinkedListNode *next;
};


struct LinkedList_str;
typedef struct LinkedList_str LinkedList;

typedef void *(*node_duplication_function)(void *);

typedef void *(*node_is_wanted)(void *, void *);

void LinkedList_init(LinkedList *obj);
void LinkedList_freeEntries(LinkedList *obj);

int  LinkedList_count(   const LinkedList *obj);
BOOL LinkedList_contains(const LinkedList *obj, const LinkedListNode *entry);
int  LinkedList_indexOf( const LinkedList *obj, const LinkedListNode *entry);
LinkedListNode* LinkedList_entryAt(LinkedList *obj, int index);

void LinkedList_append(           LinkedList *obj, LinkedListNode *entry);
void LinkedList_prepend(          LinkedList *obj, LinkedListNode *entry);
void LinkedList_insertBefore(     LinkedList *obj, LinkedListNode *entry, LinkedListNode *next);
void LinkedList_insertAfter(      LinkedList *obj, LinkedListNode *entry, LinkedListNode *prev);
LinkedListNode* LinkedList_remove(LinkedList *obj, LinkedListNode *entry);

LinkedList *LinkedList_duplicate(LinkedList *list, node_duplication_function);
LinkedList *LinkedList_duplicate_if(LinkedList *list, node_duplication_function, node_is_wanted, void *);

struct LinkedList_str {
	LinkedListNode *first;
	LinkedListNode *last;
	int cnt;
};

#define TEST_LLIST

#ifdef TEST_LLIST

struct TestNode_str {
	LinkedListNode s;
	char value;
};
typedef struct TestNode_str TestNode;

void LinkedList_dumpTestNodes(const LinkedList *obj);
void LinkedList_dump(const LinkedList *obj);
BOOL LinkedList_invariantOk(const LinkedList *obj);
void LinkedList_test(void);

#endif

#endif // LINKED_LIST_H
