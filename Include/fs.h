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

/* 

 Structures for handles and directory entries for the underlying filesystem

*/
#ifndef _FS_H
#define _FS_H

#define			HD_OK		0
#define			HD_NOREAD	(1 << 0)
#define			HD_NOPART	(1 << 1)
#define			HD_NOFAT32	(1 << 2)
#define			HD_LOCKED	(1 << 3)
#define			HD_FATCORRUPT	(1 << 4)
#define			HD_ACCESS_ERROR	0xFF

// device types
typedef enum {
	DEV_UNAVAIL = -1,	// filesystem not mounted / not available
	DEV_HDD = 0,		// local hard drive - user partition
	DEV_PTP,		// PTP camera through usb host
	DEV_ARCLIB,		// ARCLIBRARY
	DEV_URL,		// URL location
	DEV_NET,		// network shares

	DEV_UPNP,		// uPnP shares
	
} DEV_TYPE;


#define FS_ERROR_ANY		1
#define FS_ERROR_DISKFULL	2
#define FS_ERROR_PERMISSION	3

#endif
