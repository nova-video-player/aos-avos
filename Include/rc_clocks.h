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

#ifndef _RC_CLOCKS_H
#define	_RC_CLOCKS_H

typedef enum {
	RC_SYSCLK_1000			= 1008000000,
	RC_SYSCLK_800			=  800000000,
	RC_SYSCLK_600			=  600000000,
	RC_SYSCLK_300			=  300000000,
	RC_SYSCLK_MAX			= RC_SYSCLK_1000,
	RC_SYSCLK_MIN			= RC_SYSCLK_300,
	RC_SYSCLK_NORMAL		= RC_SYSCLK_600,
	RC_SYSCLK_VIDDEC_MAX		= RC_SYSCLK_1000,
	RC_SYSCLK_VIDDEC_DEFAULT	= RC_SYSCLK_600, 	     
	RC_SYSCLK_VIDDEC_HIGH		= RC_SYSCLK_800, 	     
	RC_SYSCLK_VIDDEC_VERY_HIGH	= RC_SYSCLK_1000, 	     
	RC_SYSCLK_VIDDEC_OVERDRIVE	= RC_SYSCLK_1000, 	     
	RC_SYSCLK_AUDDEC_DEFAULT	= RC_SYSCLK_300, 	     
	RC_SYSCLK_AUDDEC_HIGH		= RC_SYSCLK_300, 	     
	RC_SYSCLK_AUDDEC_A2DP		= RC_SYSCLK_300,

} RcSystemClockFreqs;

#endif /* _RC_CLOCKS_H */
