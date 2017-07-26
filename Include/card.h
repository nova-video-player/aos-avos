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

#ifndef _CARD_H_
#define _CARD_H_

#include "types.h"
#include "object.h"

#define CARD_DEV_PATH		"/dev/block/mmcblk0"
#define CARD_DEV		"/sys/block/mmcblk0"
#define CARD_P1_DEV_PATH	"/dev/block/mmcblk0p1"
#define CARD_CID_PATH		CARD_DEV"/device/cid"
/*
	SHARED card stuff
	a card reader can have several owners:
	1. avos:
		card is mounted and registered to MTPLIB
		insertion/removal triggers gui stuff or mtp events
	2. msc:
		card is unmounted
		insertion/removal triggers lun attach removal
*/
typedef void (*card_cb)(void *arg);

enum {
	CARD_AVOS,
	CARD_MSC,
};

enum {
	CARD_ADD,
	CARD_REMOVE,
};

typedef enum {
	CARD_UNKNOWN = 0,
	CARD_INDEXED,
	CARD_NOT_INDEXED,
} CardIndexedState;

int  Card_IsInsertedAsync(  void );
void Card_Open(             void );
void Card_Close(            void );
void Card_DefaultParams(    void );
void Card_CheckParams(      void );
int  Card_GetNo(            void );
void Card_ResetPartnership( void );

void Card_SetOwner(                   int new_owner );
void Card_GetBounds(       int *min_no, int *max_no );
void Card_SetIndexedState( CardIndexedState indexed );
void Card_handleEvent(int action);

CardIndexedState Card_GetIndexedState( void );
int              Card_GetCid(  UINT128 *cid );

//
// AVOS card stuf
//
void CardAvos_FormatInteractive( void );
void CardAvos_FormatFromMTP(     void );
int  CardAvos_IsAttached(        void );
void CardAvos_SafelyRemove(      void );

/** 
   *WARNING* *WARNING* *WARNING* *WARNING* *WARNING* *WARNING* *WARNING*
   You should not change anything there or else you will have the params crash !
   If you *really* want to do so, comment PARAM_DSCR_CARD_PARTNERSHIPS and 
   define a new one
**/

#define MAX_PARTNERSHIPS	5

typedef struct {
	UINT128		cid;
	UINT32		indexed;
	UINT32		last_access;
	UINT32		reserved1;
	UINT32		reserved2;
	UINT32		reserved3;
	UINT32		reserved4;
	UINT32		reserved5;
} CardPartnerShip;

typedef struct {
	UINT32		count;
	UINT32		archos_cid;
	UINT32		ticks;
	UINT32		reserved1;
	UINT32		reserved2;
	UINT32		reserved3;
	UINT32		reserved4;
	UINT32		reserved5;
	CardPartnerShip	partnership[MAX_PARTNERSHIPS];
} CardPartnerShipArray;

extern CardPartnerShipArray card_array;

DECLARE_CLASS( CardDetachScreen );

CardDetachScreen *CardDetachScreen_create( void );

#endif	// _CARD_H_
