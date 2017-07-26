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

#ifndef MEDIA_RETRIEVER_H
#define MEDIA_RETRIEVER_H

#include "media_server.h"

struct media_retriever_client {
	int			type;
	int			etype;
	const char *		mimetype;
	FILE_INFO		info;
	APIC			apic;
	int			info_valid;
	IMAGE			*thumbnail;
};

#endif
