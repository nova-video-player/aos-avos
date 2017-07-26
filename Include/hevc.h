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

#ifndef HEVC_H
#define HEVC_H

int HEVC_convert_nal_units(UCHAR *data, int data_size, 
			   UCHAR *out,  int out_size, 
			   int *_sps_pps_size, int *_nal_size);

int HEVC_convert_extradata( VIDEO_PROPERTIES *video );

int HEVC_parse_NAL( UCHAR *d, int size, CBE *cbe, int *out_size, int nal_unit_size );

#endif	// HEVC_H
