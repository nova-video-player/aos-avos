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

#ifndef H264_SPS_H
#define H264_SPS_H

typedef struct H264_SPS {
	int valid;
	
	int profile_idc;
	int level_idc;
	int width;
	int height;
	int frame_mbs_only_flag;
	int mb_aff;
	int sar_num;
	int sar_den;
	
	int timing_info_present;
	int num_units_in_tick;
	int time_scale;
	int fixed_frame_rate;
	int log2_max_frame_num;
	int num_ref_frames;
	int num_reorder_frames;
	
	// this is actually from PPS:
	int entropy_coding_mode_flag;
} H264_SPS;

#endif
