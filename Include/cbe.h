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

#ifndef _CBE_H_
#define _CBE_H_

typedef struct CBE 
{
	unsigned char 		*data;
	unsigned int 		size;
	unsigned int 		overlap;
	unsigned int		dma;
	volatile unsigned int	write;
	volatile unsigned int	read;
} CBE;

// *********************************************************************
// create, destroy
// *********************************************************************
void cbe_flush( CBE *cbe );
CBE *cbe_new( int size, int overlap, int dma );
void cbe_delete( CBE **cbe );

// *********************************************************************
// stats of the circular buffer
// *********************************************************************
int cbe_get_used( CBE *cbe );
int cbe_get_free( CBE *cbe );
int cbe_get_size( CBE *cbe );

// *********************************************************************
// read operations to the cbe
// *********************************************************************
unsigned char *cbe_get_p( CBE *cbe );
unsigned char *cbe_get_tail_p ( CBE *cbe, int size );
unsigned char *cbe_get_write_p( CBE *cbe );
unsigned int cbe_get_rest_size( CBE *cbe );
void cbe_skip( CBE *cbe, int count );
void cbe_skip_all( CBE *cbe );

// *********************************************************************
// write operations to the CBE
// *********************************************************************
int cbe_write( CBE *cbe, const unsigned char *buffer, int count );
unsigned char *cbe_get_patch_p( CBE *cbe, int size, unsigned char **overlap_p, int *overlap_size );
int cbe_patch( CBE *cbe, unsigned char *patch_p, int size );

#endif
