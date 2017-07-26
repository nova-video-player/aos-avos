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

#ifndef _MAINLOOP_H
#define _MAINLOOP_H

void mainloop_init( void );
void mainloop_deinit( void );
void mainloop_wakeup( void );
void mainloop_flush_event( void );
void mainloop_exit( void ); 
void mainloop_enter( void ); 

#endif
