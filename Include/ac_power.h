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
 * C Interface: ac_power
 * 
 *  Description: 
 *	Device power down and standby management
 *
 * Author: Matthias Welwarsky <welwarsky@archos.com>, (C) 2007
 *
 *
 */

#ifndef _AC_POWER_H
#define _AC_POWER_H

#include "app_state.h"

extern const KEYB_STATE STANDBY_actions;

#define REBOOT_DELAY	15 //7

extern void AC_PowerOff( void );
extern void AC_PowerBatteryLow( void );
extern void AC_PowerEmergencyOff( void );
extern void AC_PowerStandBy( void );
extern int  Standby_isActive( void );
extern void Power_auto_standby( void );
extern void Set_Standbyatstart( void );
extern void Clear_Standbyatstart( void );
extern int  Is_Standbyatstart( void );
extern void Postponed_Standby( void );
extern int  Is_StandbyVeto( void );
extern int  Is_StandbyRequested( void) ;
extern void Exit_Standby_from_event( void );
extern void Clear_Standby_Req( void );
extern void Set_Standby_Req( void );

// For debug only:
extern unsigned long get_heap_size(void);

#endif	// _AC_POWER_H
