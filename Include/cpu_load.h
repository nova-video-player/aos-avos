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

#ifndef _CPU_LOAD_H
#define _CPU_LOAD_H

extern unsigned short cpu_load_get_amount_of_cpu_used( void );

extern unsigned short cpu_load_get_amount_of_memory_used( void );
		
extern void cpu_load_start_top( void );

extern void cpu_load_stop_top( void );

#endif
