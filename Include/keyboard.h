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

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "awchar.h"
#include "types.h"


enum Modifiers {
	None  = 0,
	Shift = 1,
	Caps  = 2,
	Alt   = 4,
	AltGr = 8,
	Trema = 16,
	Circumflex = 32
};

enum KeyboardSource {
	KBD_DEVICE,
	KBD_IR_REMOTE,
	KBD_REMOTE_FM,
	KBD_HID,
	KBD_MEDIA,
};

typedef void (*kbd_modifiers_cb)(void *listener, int modifiers);

BOOL Keyboard_mapKey(wchar *key);
int  Keyboard_getModifiers(void);
void Keyboard_setModifiers(int modifiers);
void Keyboard_setModifiersListener(void *listener, kbd_modifiers_cb on_modifiers_changed);
int  Keyboard_getSource(void);

wchar Keyboard_applyShift(wchar key);
wchar Keyboard_applyTrema(wchar key);
wchar Keyboard_applyCircumflex(wchar key);


#endif // KEYBOARD_H
