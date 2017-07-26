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

#include "object.h"
#include "astdlib.h"
#include "debug.h"


/**
 @struct Object
 @brief Base class for classes supporting the dynamic type system. 

 The struct contains a pointer to a global class variable, which 
 contains information for the dynamic type system. This information 
 consists of the class name in plaintext and a pointer to the class 
 variable of the superclass.
 See https://subversion/trac/avx/wiki/objectOrientedC for an overwiew.
*/
Class Object_class = {
	NULL, 
	"Object"
};

/**
 @def OBJ_CREATE
 @brief Allocates an object on the heap.

 Allocates memory for the object, initializes it to zero and sets the pointer to the class variable.
 @returns The allocated object or NULL in an out of memory condition.
*/

/**
 @def OBJ_IS
 @brief Returns whether the object is of the given type.
 @returns True if the object implements the given class or inherits it.
*/

/**
 @def OBJ_AS
 @brief Casts the object to the given type and asserts that the cast is valid.

 OBJ_AS or OBJ_DCAST \em must be used for all downcasts.
 Since OBJ_AS is based on an assert, using it has no performance penalty in release builds.
*/

/**
 @def OBJ_DCAST
 @brief Dynamically casts the object to the given type.
 @returns The object, cast to the given type, or NULL if the cast is invalid.
*/

/**
 @brief Dumps the name and address of the object, using serprintf.

 Safe to use on a NULL pointer.
*/
void Object_dump(const Object *obj)
{
	if(obj != NULL) {
		serprintf("(%s %p)\n", obj->cls->name, obj);
	} else {
		serprintf("(NULL)\n");
	}
}

// internal helper function for OBJ_CREATE.
Object* Object_create(const Class *cls, int size)
{
	Object *obj = acalloc_named(1, size, cls->name);
	if(obj != NULL) {
		obj->cls = cls;
		assert(Object_inherits(obj, &Object_class));
	} else {
		serprintf("out of memory!");
		Trace();
		
	}
	return obj;
}

// internal helper function for OBJ_IS, OBJ_AS and OBJ_DCAST.
BOOL Object_inherits(const Object *obj, const Class *cls)
{
	const Class *tmp = obj->cls;

	while(tmp != NULL) {
		if(tmp == cls)
			return TRUE;
		tmp = tmp->super;
	}
	return FALSE;
}





//----------------------------------------------------//
//	test code
//----------------------------------------------------//

//#define TEST_OBJECT

#ifdef TEST_OBJECT

Class A_class = {
	&Object_class, 
	"A"
};

typedef struct {
	Object s_object;
} A;

//----------------------------------------------------//

Class B_class = {
	&A_class, 
	"B"
};

typedef struct {
	A s_a;
} B;

//----------------------------------------------------//

void Object_test(void)
{
	A *a = OBJ_CREATE(A);
	serprintf("a inherits Object: %i\n",      OBJ_IS(Object, a));	// => TRUE
	serprintf("a inherits A: %i\n",           OBJ_IS(A,      a));	// => TRUE
	serprintf("a inherits B: %i\n\n",         OBJ_IS(B,      a));	// => TRUE

	B *b = OBJ_CREATE(B);
	serprintf("b inherits Object: %i\n",      OBJ_IS(Object, b));	// => TRUE
	serprintf("b inherits A: %i\n",           OBJ_IS(A,      b));	// => TRUE
	serprintf("b inherits B: %i\n\n",         OBJ_IS(B,      b));	// => FALSE

	A *b_as_a = (A*) b;
	serprintf("b_as_a inherits Object: %i\n", OBJ_IS(Object, b_as_a));	// => TRUE
	serprintf("b_as_a inherits A: %i\n",      OBJ_IS(A,      b_as_a));	// => TRUE
	serprintf("b_as_a inherits B: %i\n\n",    OBJ_IS(B,      b_as_a));	// => TRUE

	Object *to = OBJ_DCAST(Object, a); serprintf("a dcast Object: %p\n", to);	// => obj
	A *ta      = OBJ_DCAST(A, a);      serprintf("a dcast A: %p\n",      ta);	// => obj
	B *tb      = OBJ_DCAST(B, a);      serprintf("a dcast B: %p\n\n",    tb);	// => NULL

	to = OBJ_AS(Object, a); serprintf("a as Object: %p\n", to);	// => obj
	ta = OBJ_AS(A, a);      serprintf("a as A: %p\n",      ta);	// => obj
//	tb = OBJ_AS(B, a);      serprintf("a as B: %p\n",      tb);	// would cause the assert to fail

	Object_dump(OBJ_AS(Object, a));
	Object_dump(OBJ_AS(Object, b));
}

#endif
