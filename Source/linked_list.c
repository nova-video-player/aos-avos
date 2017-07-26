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

#include <stdlib.h>
#include <assert.h>
#include "linked_list.h"
#include "debug.h"
#include "astdlib.h"
#include "types.h"

// Note: A circular linked list would be more elegant, but I 
// want to link to NULL instead of a dummy object at the ends.


void LinkedListNode_init(LinkedListNode *obj)
{
	obj->prev = NULL;
	obj->next = NULL;
}

Class LinkedListNode_class = {
	&Object_class, 
	"LinkedListNode"
};

// return the previous element that inherits the given class or NULL
LinkedListNode* LinkedListNode_findPrev(LinkedListNode *obj, const Class *cls)
{
	obj = obj->prev;
	
	while(obj != NULL) {
		if(Object_inherits(OBJ_AS(Object, obj), cls))
			return obj;
		obj = obj->prev;
	}
	return NULL;
}

// return the next element that inherits the given class or NULL
LinkedListNode* LinkedListNode_findNext(LinkedListNode *obj, const Class *cls)
{
	obj = obj->next;
	
	while(obj != NULL) {
		if(Object_inherits(OBJ_AS(Object, obj), cls))
			return obj;
		obj = obj->next;
	}
	return NULL;
}

//----------------------------------------------------//

void LinkedList_dump(const LinkedList *obj);
BOOL LinkedList_invariantOk(const LinkedList *obj);

void LinkedList_init(LinkedList *obj)
{
	obj->first = NULL;
	obj->last  = NULL;
	obj->cnt = 0;
}

void LinkedList_freeEntries(LinkedList *obj)
{
	//DR: segv, don't use it
	//JT: your memory must have been corrupted before. please investigate.
	while(obj->first != NULL) {
		afree(LinkedList_remove(obj, obj->first));
	}
}

int LinkedList_count(const LinkedList *obj)
{
	return obj->cnt;
}

static inline BOOL LinkedList_isEnqueued(const LinkedList *obj, const LinkedListNode *entry)
{
	return entry->prev != NULL || entry->next != NULL || obj->first == entry;
}

BOOL LinkedList_contains(const LinkedList *obj, const LinkedListNode *entry)
{
	return entry != NULL && LinkedList_indexOf(obj, entry) != -1;
}

// Try to avoid this method. It has to iterate the list.
// Returns -1 if the list does not contain the entry.
int LinkedList_indexOf( const LinkedList *obj, const LinkedListNode *entry)
{
	if(!LinkedList_isEnqueued(obj, entry))
		return -1;

	int i = 0;
	LinkedListNode *act = obj->first;
	while(act != NULL) {
		if(act == entry)
			return i;
		act = act->next;
		i++;
	}
	return -1;
}

LinkedListNode* LinkedList_entryAt(LinkedList *obj, int index)
{
	int i = 0;
	LinkedListNode *act = obj->first;
	while(act != NULL) {
		if(i == index)
			return act;
		act = act->next;
		i++;
	}
	return NULL;
}

void LinkedList_append(LinkedList *obj, LinkedListNode *entry)
{
	assert(!LinkedList_isEnqueued(obj, entry));

	entry->prev = obj->last;
	if(obj->last != NULL)
		obj->last->next = entry;
	if(obj->first == NULL)
		obj->first = entry;
	obj->last = entry;
	obj->cnt++;
}

void LinkedList_prepend(LinkedList *obj, LinkedListNode *entry)
{
	assert(!LinkedList_isEnqueued(obj, entry));

	entry->next = obj->first;
	if(obj->first != NULL)
		obj->first->prev = entry;
	if(obj->last == NULL)
		obj->last = entry;
	obj->first = entry;
	obj->cnt++;
}

void LinkedList_insertBefore(LinkedList *obj, LinkedListNode *entry, LinkedListNode *next)
{
	assert(!LinkedList_isEnqueued(obj, entry));

	entry->next = next;
	entry->prev = next->prev;

	if(next->prev != NULL) {
		next->prev->next = entry;
	} else {
		obj->first = entry;
	}
	next->prev = entry;

	if(obj->last == NULL)
		obj->last = entry;

	obj->cnt++;
}

void LinkedList_insertAfter( LinkedList *obj, LinkedListNode *entry, LinkedListNode *prev)
{
	assert(!LinkedList_isEnqueued(obj, entry));

	entry->prev = prev;
	entry->next = prev->next;

	if(prev->next != NULL) {
		prev->next->prev = entry;
	} else {
		obj->last = entry;
	}
	prev->next = entry;

	if(obj->first == NULL)
		obj->first = entry;

	obj->cnt++;
}

LinkedListNode *LinkedList_remove(LinkedList *obj, LinkedListNode *entry)
{
	assert(LinkedList_isEnqueued(obj, entry));

	if(entry->prev != NULL)
		entry->prev->next = entry->next;
	if(entry->next != NULL)
		entry->next->prev = entry->prev;

	if(obj->first == entry)
		obj->first = entry->next;
	if(obj->last == entry)
		obj->last = entry->prev;

	entry->prev = NULL;
	entry->next = NULL;

	obj->cnt--;
	return entry;
}

static void *all_of_them(void *a, void *b) 
{
    return (void *) 1;
}

LinkedList *LinkedList_duplicate(LinkedList *list, node_duplication_function duplicate)
{
    return LinkedList_duplicate_if(list, duplicate, all_of_them, NULL);
}

LinkedList *LinkedList_duplicate_if(LinkedList *list, 
				    node_duplication_function duplicate, 
				    node_is_wanted is_wanted,
				    void *extra_data)
{
    LinkedList *new_list;
    LinkedListNode *current_node = list->first;
    LinkedListNode *new_node;

/*     serprintf("DR: list about to get duplicated :\n"); */
/*     LinkedList_dump(list); */

    new_list = amalloc(sizeof(LinkedList));
    assert(new_list); // TODO
    LinkedList_init(new_list);

    while (current_node)
    {
	if (is_wanted(current_node, extra_data))
	{
	    new_node = duplicate(current_node);
	    assert(new_node);
	    new_node->prev = NULL;
	    new_node->next = NULL;
	    LinkedList_append(new_list, new_node);
	}
	current_node = current_node->next;
    }

/*     serprintf("DR: list that got duplicated :\n"); */
/*     LinkedList_dump(new_list); */

    return new_list;
}



//----------------------------------------------------//
//	test code
//----------------------------------------------------//

#ifdef TEST_LLIST

void LinkedList_dumpTestNodes(const LinkedList *obj)
{
	serprintf("LinkedList %p {\n\tcnt = %i\n", obj, obj->cnt);

	LinkedListNode *act = obj->first;
	while(act != NULL) {
		char vact  = ((TestNode*) act)->value;
		char vprev = act->prev != NULL ? ((TestNode*) act->prev)->value : 'X';
		char vnext = act->next != NULL ? ((TestNode*) act->next)->value : 'X';

		serprintf("\t%c <- %c -> %c\n", vprev, vact, vnext);
		act = act->next;
	}
	serprintf("}\n");
}

void LinkedList_dump(const LinkedList *obj)
{
	serprintf("LinkedList %p {\n\tcnt = %i\n", obj, obj->cnt);

	LinkedListNode *act = obj->first;
	while(act != NULL) {
		serprintf("\t%p <- %p -> %p\n", act->prev, act, act->next);
		act = act->next;
	}
	serprintf("}\n");
}

BOOL LinkedList_invariantOk(const LinkedList *obj)
{
	int cnt = obj->cnt;
	LinkedListNode *act = obj->first;

	int i;
	for(i = 0; i < cnt; i++) {
		if(act == NULL)
			return FALSE;
		act = act->next;
	}
	return act == NULL;
}

void LinkedList_test(void)
{
	TestNode a; LinkedListNode_init((LinkedListNode*) &a); a.value = 'a';
	TestNode b; LinkedListNode_init((LinkedListNode*) &b); b.value = 'b';
	TestNode c; LinkedListNode_init((LinkedListNode*) &c); c.value = 'c';
	TestNode d; LinkedListNode_init((LinkedListNode*) &d); d.value = 'd';
	TestNode x; LinkedListNode_init((LinkedListNode*) &x); x.value = '*';

	LinkedList list;
	LinkedList_init(&list);
	LinkedList_dump(&list);

	LinkedList_append(&list, (LinkedListNode*) &c);
	LinkedList_dump(&list);

	LinkedList_prepend(&list, (LinkedListNode*) &a);
	LinkedList_dump(&list);

	LinkedList_insertBefore(&list, (LinkedListNode*) &b, (LinkedListNode*) &c);
	LinkedList_dump(&list);

	LinkedList_insertAfter( &list, (LinkedListNode*) &d, (LinkedListNode*) &c);
	LinkedList_dump(&list);

	LinkedList_remove(&list, (LinkedListNode*) &a);
	LinkedList_dump(&list);

	serprintf("contains a: %i\n", LinkedList_contains(&list, (LinkedListNode*) &a));
	serprintf("contains b: %i\n", LinkedList_contains(&list, (LinkedListNode*) &b));

	LinkedList_remove(&list, (LinkedListNode*) &c);
	LinkedList_dump(&list);

	LinkedList_remove(&list, (LinkedListNode*) &d);
	LinkedList_dump(&list);

	LinkedList_remove(&list, (LinkedListNode*) &b);
	LinkedList_dump(&list);
}

#endif
