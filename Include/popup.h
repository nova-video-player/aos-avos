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

#ifndef POPUP_H
#define POPUP_H

//-----------------------------------------------------------//
//	DEPRECATED
//
//	For new screens, set a menu builder for PopupMenuitemWidgets, 
//	using PopupMenuitemWidget_setSubmenuBuilder.
//-----------------------------------------------------------//

#include "types.h"


// Maximum number of items in a popup
#define POPUP_MAX_ITEMS		12

#define POPUP_MODE_NONE		-1
#define POPUP_MODE_TOGGLE	0


typedef void (*fp_popup)( void *param );

typedef struct popup_item_str {
	int		shortcut_icon;			// Index of the corresponding shortcut icon or SYM_NO_BITMAP if none
	const char	*label;				// Name displayed in the popup menu
	fp_popup	action;				// Action to do when this item is selected
	void		*param;				// Parameter passed to the action function
} PopupItem;


// Description of a popup
struct popup_str {
	// General parameters, used by all types of popups
	int		num_items;		// Number of items in the current popup menu
	int		mode;			// Popup mode (vertical, toggle or slider)
	int 		pos;			// Index of the current item or -1 if popup is closed
	
	// Slider popup specific parameters
	int		min;			// Minimum value for a POPUP_SLIDER
	int		max;			// Maximum value for a POPUP_SLIDER
	int		value;			// Current value for a POPUP_SLIDER

	// Table of items
	PopupItem	item[POPUP_MAX_ITEMS];	// Table of items
};


// Variable containing the current popup data
extern struct popup_str *popup;

// Popup management functions
void Popup_Clear( struct popup_str *obj );
void Popup_AddItem( struct popup_str *obj, const struct popup_item_str *item );
int  Popup_CountEntries( const struct popup_str *obj );

void AC_PopupToggleOpen( int starting_pos );

const PopupItem* Popup_GetItem( const struct popup_str *obj, int index );


#endif	// POPUP_H
