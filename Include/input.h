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

#ifndef _AINPUT_H
#define _AINPUT_H

#include "dataevent.h"
#include "types.h"

//#define HIDE_BUTTONS_IN_INPUT_KEYBOARD_MODE

#define MAX_INPUT_DATA_EVENT 2

typedef enum {
	INPUT_KEYBOARD    = 0,
	INPUT_MOUSE       = 1,
	INPUT_DEVICES_MAX = 2
} InputMode;

struct input_device {
	data_event_t data_event[MAX_INPUT_DATA_EVENT];;
	void (*close)(struct input_device *dev);
	void (*flush)(struct input_device *dev);
	void *priv;
};

struct input_devices {
	int num_devices;
	struct input_device device[INPUT_DEVICES_MAX];
};

// Get the input device struct pointer. prevent global variables

struct input_devices *input_devices_get(void);

int  input_devices_init(struct input_devices *input_devices);
int  input_devices_open(struct input_devices *input_devices);
void input_devices_close(struct input_devices *input_devices);
void input_devices_flush(struct input_devices *input_devices);
void input_devices_read_nonblocking(struct input_devices *input_devices );

InputMode input_get_mode(void);
void input_set_mode(InputMode mode);

#endif // _AINPUT_H
