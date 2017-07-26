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

#ifndef _BROWSER_H_
#define _BROWSER_H_

#include "atf.h"
#include "dv/dv_source.h"

DVSource* FirstEntriesSource_create( int redirection, DVFilterCb filter, int mode_select );

enum {
	REDIRECTION_PHOTO,
	REDIRECTION_VIDEO,
	REDIRECTION_AUDIO,
	REDIRECTION_DOUBLEBROWSER,
};

DVExtraData 	*createBrowserExtraDataAt(const DVSource *obj, int index, extradata_abort_cb abort, void *abort_ctx);

#define DATA_THUMB_WIDTH	200
#define DATA_THUMB_HEIGHT	200

typedef struct DVThumb_str DVThumb;

struct DVThumb_str {
	DVExtraData	s;

	int		error:2;
	int		drm:1;
	int		protect:1;
	IMAGE		*thumb;
	ATF_INFOS	atfi;
	void		*dir_infos;
	char		*parent_location;

	int		pos;
	int		dirty:1;
};

#endif // _BROWSER_H_


