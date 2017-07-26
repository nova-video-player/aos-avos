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

#ifndef _EPG_DVB_H
#define _EPG_DVB_H

#ifdef CONFIG_ATV_DVBT
#ifdef CONFIG_EPG

#include "epg.h"

extern const EPG epg_DVBT;

int epg_dvb_init( void *parser_priv );

#endif // CONFIG_EPG
#endif // CONFIG_ATV_DVBT

#endif // _EPG_DVB_H
