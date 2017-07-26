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


#ifndef _HARDWARE_H
#define _HARDWARE_H

#include "types.h"
#include "global.h"

extern void HDW_Init( void );
extern void HDW_Deinit( void );
extern int  HDW_DCDetect( int force );
extern int  HDW_DetectModule( void );
extern void HDW_LCDStart( void );
extern void HDW_LCDStop( void );
extern void HDW_VideoOutEnable( void );
extern void HDW_VideoOutDisable( void );
extern BOOL HDW_IsVideoOutEnabled( void );
extern int  HDW_IsBattOK( void );
extern void HDW_SetVcom( long Level );


#define DEFAULT_LCD_VCOM 		31	// actual value is x2
#define LCD_VCOM_MIN			0	// actual value is x2
#define LCD_VCOM_MAX			63	// actual value is x2

#define LCD_CONTRAST_MIN		0
#define LCD_CONTRAST_MAX		63
#define DEFAULT_LCD_CONTRAST		32

#define LCD_BRIGHTNESS_MIN		0
#define LCD_BRIGHTNESS_MAX		63
#define DEFAULT_LCD_BRIGHTNESS		32

#define LCD_GAMMA_MIN			0
#define LCD_GAMMA_MAX			3
#define DEFAULT_LCD_GAMMA		2

#define PROGRAMMED_CURRENT_USB		0.550

#define VENC_WIDESCREEN 0x07
#define VENC_ATR_INSERTION 0x80
#define VENC_ATR_CLEAR 0x00

// charge handling
extern float HDW_GetProgrammedCurrent_A( void );
extern int HDW_GetBatteryVoltage_mV( void );
extern void HDW_SetBatteryChargeLed( int on, int mode );

// IR receiver
extern void HDW_EnableIrrIO( int en );
extern int HDW_IsIRROnBoard( void );

// USB device
enum { USB_EXTERNAL_PORT=0, USB_INTERNAL_PORT };
int HDW_GetUsbDetected( int usb_port );


#endif // _HARDWARE_H



