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

#ifndef _DRM_H
#define _DRM_H

// ***************************************************************************
// (MICROSOFT) DRM identifiers
// ***************************************************************************
typedef enum {
	DRM_NONE	= 0,
	DRM_ANY     	= 1,
	MS_DRM_PD   	= 2,
	MS_DRM_V1   	= 4,
	MS_DRM_V2   	= 8,
	MS_DRM_ND   	= 16
} DRM_TYPE;

enum { 
	OPL_AnalogVideo_Any = 0, 
	OPL_AnalogVideo_CGSMA = 151,
	OPL_AnalogVideo_Off = 201,
};
 
typedef struct {
	UINT		valid;
	UINT		CompressedDigitalVideo;
	UINT		UncompressedDigitalVideo;
	UINT		AnalogVideo; 	//   0..100 any video output
					// 101..150 should turn on CGSM-A ("copy never")
					// 151..200 must turn on CGSM-A ("copy never")
					// 201..    no output
	UINT		CompressedDigitalAudio;
	UINT		UncompressedDigitalAudio;
} DRM_OPL;

typedef struct {
	int		drm;

	int		napster;
	int		subscription;
	int		bound;
	int		noVideoOut;
	int		noDigitalAudioOut;
	
	unsigned char* 	V1_key;			// Content Encryption Object data
	UINT32 		V1_key_sz;		// Content Encryption Object size
	
	unsigned char* 	V1_secret;		// Content Encryption Object data
	UINT32 		V1_secret_sz;		// Content Encryption Object size
	
	unsigned char* 	V1_prottype;		// Content Encryption Object data
	UINT32 		V1_prottype_sz;		// Content Encryption Object size

	unsigned char* 	V1_url;			// Content Encryption Object data
	UINT32 		V1_url_sz;		// Content Encryption Object size
	
	unsigned char* 	V2_data;		// Extended Content Encryption Object data
	UINT32 		V2_size;		// Extended Content Encryption Object size

	DRM_OPL		opl;
	UINT32		macrovision;		// need macrovision ?
} DRM_PROPERTIES;

typedef int (*DRM_SETUP)( void *ctx, void *param );
typedef int (*DRM_OPEN) ( void *ctx );
typedef int (*DRM_CRYPT)( void *ctx, UINT64 sample_ID, UCHAR *data, int size  );
typedef int (*DRM_CLOSE)( void *ctx );

typedef struct DRM_CTX {
	int		type;
	void 		*handle;	
	DRM_SETUP 	setup;		
	DRM_OPEN  	open;
	DRM_CRYPT 	crypt;		
	DRM_OPEN  	close;
} DRM_CTX;


#endif
