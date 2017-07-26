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

/**
 @file
 @brief Defines the Object class and related macros. 
 See https://subversion/trac/avx/wiki/objectOrientedC for an overwiew.
*/

#ifndef OBJECT_H
#define OBJECT_H

#include "types.h"
#include <assert.h>


// declares the struct and class variable of a class
#define DECLARE_CLASS(cname) \
 struct cname ## _str; \
 typedef struct cname ## _str cname; \
 extern Class cname ## _class;

// allocates an object and sets its class variable
#define OBJ_CREATE(cname) ((cname*) Object_create(& (cname ## _class), sizeof(cname)))

// returns whether obj is of type cname or inherits the type
#define OBJ_IS(cname, obj) ((Object_is_helper((Object*) (obj), & (cname ## _class))))

// casts obj to ctype, asserts that the cast is valid
#define OBJ_AS(cname, obj) ((cname *) Object_as_helper((Object*) (obj), & (cname ## _class)))

// casts obj to ctype, returns NULL if the cast is invalid
#define OBJ_DCAST(cname, obj) ((cname *) Object_dcast_helper((Object*) (obj), & (cname ## _class)))


struct Class_str {
	const struct Class_str *super;
	const char *name;
};
typedef struct Class_str Class;


struct Object_str {
	const Class *cls;
};
typedef struct Object_str Object;


Object* Object_create(const Class *cls, int size);

void Object_dump(const Object *obj);
BOOL Object_inherits(const Object *obj, const Class *cls);

extern Class Object_class;


static inline BOOL Object_is_helper(Object *obj, const Class *cls)
{
	return obj != NULL && Object_inherits(obj, cls);
}

static inline Object* Object_as_helper(Object *obj, const Class *cls)
{
	assert(obj == NULL || Object_inherits(obj, cls));
	return obj;
}

static inline Object* Object_dcast_helper(Object *obj, const Class *cls)
{
	return (obj != NULL && Object_inherits(obj, cls)) ? obj : NULL;
}


#endif // OBJECT_H
