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

#ifndef _PTS_REORDER_H_
#define _PTS_REORDER_H_

void *pts_ro_alloc( int count );
void  pts_ro_init ( void *c );
void  pts_ro_run  ( void *c );
void  pts_ro_stop ( void *c );
void  pts_ro_free ( void *c );
void  pts_ro_put  ( void *c, int ts );
int   pts_ro_get  ( void *c);

extern int pts_force_reorder;

#endif
