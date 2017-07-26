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

// ***********************************************************************************************
//
// Global Definitions for AVUX Source 
//
// ************************************************************************************************

#ifndef _GLOBAL_H
#define _GLOBAL_H

#define MAX_NAME_LEN 255 

#ifdef ARM_HAS_NEON
	#define CONFIG_NEON
#endif

// ************************************************************************************************
// SPECIAL Compilation options to be set or not
//
#ifndef CONFIG_RELEASE
	#define DEBUG_MSG			// Features used for debugging
	#undef  DEBUG_MSG_SYNC			// make sure dbg messages are sent and flushed before serprintf returns
#else
	#undef DEBUG_MSG			// Features used for debugging
#endif

// ----------------------------
// TV OUT
// ----------------------------
#define CONFIG_HDMI
#define CONFIG_HDMI_720P

#define CONFIG_I18N				// support for internationalization (asian fonts etc.) 
#define CONFIG_CP1250				// support for Eastern Europe 
#define CONFIG_CP1251				// support for Cyrillic Russian  
#define CONFIG_CP1253				// support for Cyrillic Greek 
#define CONFIG_CP1254				// support for Turkish 
#define CONFIG_CP1255				// support for Hebrew  
#define CONFIG_CP1256				// support for Arabic 
#define CONFIG_CP1257				// support for Baltic
#define CONFIG_CP1258				// support for Vietnamese
#define CONFIG_CP874				// support for Thai
#define CONFIG_CP936				// support for Simplified  Chinese 
#define CONFIG_CP950				// support for Traditional Chinese 
#define CONFIG_CP932				// support for Japanese 
#define CONFIG_CP949				// support for Korean 

// other "global" features
// ------------------------
#define MAX_NAME_LEN 	255	// maximum name of a dir/track title

// HDD_ROOT: path to the FAT32 partition (content)
// if no hdd root yet, define one!
#ifndef HDD_ROOT
	#ifdef CONFIG_ANDROID
		#define HDD_ROOT "/mnt/sdcard/"
	#else
		#define HDD_ROOT "/mnt/data/"
	#endif
#endif
#define FD_ROOT "fd://"

// HDD_SYSTEM: path to the EXT3 (or else) hidden (TBC) partition (system)
#ifndef HDD_SYSTEM
	#define HDD_SYSTEM "/mnt/system/"
#endif

#ifdef CONFIG_ANDROID
	#define CARD_ROOT "/mnt/sdcard/"
#else
	#define CARD_ROOT "/mnt/card/"
#endif

#ifndef NETWORK_ROOT
	#define NETWORK_ROOT "/mnt/network/"
#endif

#define UPNP_ROOT "/mnt/network/upnp/"

//#define GFI_ASYNC

#endif	// _GLOBAL_H
