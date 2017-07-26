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

// *****************************************************
//
// APP.H
//	application specific definitions
//
// *****************************************************

#ifndef _APP_H
#define _APP_H

#include "browse.h"
#include "rect.h"

#include <types.h>

#define INACTIVE	0
#define ACTIVE		(!INACTIVE)

// Set the default language and country
#if defined CONFIG_SFR
 #define PARAMS_DEFAULT_LANGUAGE	"FR"
 #define PARAMS_DEFAULT_COUNTRY		"FR"
#elif defined CONFIG_SWISSCOM
 #define PARAMS_DEFAULT_LANGUAGE	"DE"
 #define PARAMS_DEFAULT_COUNTRY		"CH"
#else
 #define PARAMS_DEFAULT_LANGUAGE	"EN"
 #define PARAMS_DEFAULT_COUNTRY		"US"
#endif

// Default language to use when the non-latin fonts are not loaded (=> must be a latin language!)
#define PARAMS_DEFAULT_LATIN_LANGUAGE	PARAMS_DEFAULT_LANGUAGE

extern int InStandByMode;

enum {
	LAUNCH_SCREEN_BROWSER = 0,
	LAUNCH_SCREEN_VIDEO_BROWSER,
	LAUNCH_SCREEN_INFO_BROWSER,
	LAUNCH_SCREEN_POS,
	LAUNCH_SCREEN_HOME
};

// ************************************************************
// MISC DISPLAY
// ************************************************************

#define	VIDEO_INT		0
#define VIDEO_EXT		1

#define VIDEO_FORMAT_NTSC	0
#define VIDEO_FORMAT_PAL	1

extern int		VIDEO_format;

#define TV_4_3			0
#define TV_16_9			1
#define TV_PLUS			2
#define DEFAULT_TV_FORMAT	TV_4_3

extern int		TV_format;

enum {
	EXTERNAL_DISPLAY_COMPOSITE = 0,
	EXTERNAL_DISPLAY_SVIDEO,
	EXTERNAL_DISPLAY_COMPONENT,
	EXTERNAL_DISPLAY_RGB,
#ifdef CONFIG_HDMI
	EXTERNAL_DISPLAY_HDMI,		// Must be after all the analog items
#endif
	NB_EXTERNAL_DISPLAY
};
#define DEFAULT_EXTERNAL_DISPLAY	EXTERNAL_DISPLAY_COMPOSITE
#ifdef CONFIG_HDMI
  #define NB_EXTERNAL_ANALOG_DISPLAY	EXTERNAL_DISPLAY_HDMI
#else
  #define NB_EXTERNAL_ANALOG_DISPLAY	NB_EXTERNAL_DISPLAY
#endif

extern int	VIDEO_external_display;

#define HDMI_MODE_OFFSET 5   /*MODE_HDMI_VGA in dm64xxfb.h*/
enum {
        HDMI_RESOLUTION_VGA = 0,
	HDMI_RESOLUTION_480P,
	HDMI_RESOLUTION_576P,
#ifdef CONFIG_HDMI_720P
#ifdef CONFIG_HDMI_720P_50HZ
	HDMI_RESOLUTION_720P50,
#endif
	HDMI_RESOLUTION_720P60,
#endif
#ifdef CONFIG_HDMI_LARGE
	HDMI_RESOLUTION_768,
	HDMI_RESOLUTION_1024,
	HDMI_RESOLUTION_1080P,
#endif
	NB_HDMI_RESOLUTION,
};

#ifdef CONFIG_HDMI_720P
#define DEFAULT_HDMI_RESOLUTION		HDMI_RESOLUTION_720P60
#else
#define DEFAULT_HDMI_RESOLUTION		HDMI_RESOLUTION_576P
#endif

extern int 	VIDEO_hdmi_resolution;

typedef struct {
	int external_display;
	int video_format;
	int tv_format;
#ifdef CONFIG_HDMI
	int hdmi_resolution;
#endif
	int sound_output;
} user_tvout_settings;

extern user_tvout_settings on_chip_tvout_settings;	// Mini-dock or battery-dock
extern user_tvout_settings ext_chip_tvout_settings;	// TV cradle

enum {
	STANDBY_TIMEOUT_INDEX_5MIN = 0,
	STANDBY_TIMEOUT_INDEX_30MIN,
	STANDBY_TIMEOUT_INDEX_2H,
	STANDBY_TIMEOUT_INDEX_12H,
	NB_STANDBY_TIMEOUT_VALUES
};

#define DEFAULT_STANDBY_TIMEOUT_INDEX	STANDBY_TIMEOUT_INDEX_2H	

extern int 	standby_timeout_index;

// Battery
#define BATT_LOAD_NUM_STEPS	2

// ************************************************************
// MESSAGEBOXES
// ************************************************************
#define MAX_PROGRESS		16

extern int power_off_requested;

// ************************************************************
// FUNCTION PROTOTYPES
// ************************************************************

void AC_FirstWelcome( void );
void check_mtp_arclib( void );

void AC_RestartFromStandBy( void );
void AC_RestartFromSuspend( void );

void AC_RecoverFromReset( void );

int  AC_GetClockTimeFormatForCountry( const char *country_iso_code );
void AC_SetClockTimeFormatForCountry( const char *country_iso_code );
void AC_SetClockMenu( void );
void AC_SetSaveParams( void );
void AC_SetClockModified( void );
void AC_SetClockTimeFormatModified( void );
void AC_ClockMenuExit( void );
void AC_TimeChanged( void );
void AC_DateChanged( void );
void AC_UpdateClockSettings( void );

void AC_DestroyAllApps( void );
void AC_DestroyAllApps_AV_stop( void );
void AC_SynchronousEscToBrowse( void );
void AC_SynchronousEscToBrowseByName( char *name );

void Device_lock( void );
void Device_unlock( void );
BOOL Device_isLocked( void );

void AC_RedrawOsd( void );
void AC_RedrawOsdAsync( void );
void AC_RedrawOsdArea( const rect *area );
void AC_RedrawOsdAreaAsync( const rect *area );
void AC_ScrollOsdArea( const rect *area, int dx, int dy );
void AC_ForceMergePaintEvents( void );
void AC_UnforceMergePaintEvents( void );

void APP_busy( void );
void APP_unbusy( void );
int  APP_is_busy( void );

void AC_Reboot( void );

void AC_DispClock( void );
int RTC_to_user_hour( int rtc_hour );

void APP_SerialPort( unsigned char byte );

void AC_MainLoop( int calibrate );

void OSD_Enable( void );
void OSD_Disable( void );
void OSD_FadeOut( void );
BOOL OSD_isEnabled( void );
BOOL OSD_isDisabled( void );
BOOL OSD_hasChanged( void );
void OSD_syncState( void );

void AC_MenuSetVideo( void );
void AC_MenuSetTVFormat( void );
void AC_SetExternalDisplay( void );
int  AC_GetVideoFormatForCountry( const char *country_iso_code );

#ifdef CONFIG_LCD_AJUSTABLE_SETTINGS
void AC_ChangeLCDContrast(void);
void AC_ChangeLCDBrightness(void);
void AC_ChangeLCDGamma(void);
void AC_ResetLCDSettings( void );
void AC_ChangeLCDVcom(void);
#endif

void SetTVformatString( void );

void VideoFormatToTvMode( int *tvmode, int *tvformat );

//AC_InitDevice return values
#define DEVICE_OK 		0
#define DEVICE_BAD_FAT 		1
#define DEVICE_NOT_SUPPORTED	2
#define DEVICE_REG_ERROR	3
#define DEVICE_RESET_ERROR	4
#define DEVICE_DETECTION_ERROR	5

#endif	// _APP_H
