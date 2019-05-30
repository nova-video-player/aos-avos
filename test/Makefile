# Copyright 2017 Archos SA
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#
# test stuff
#
CC = gcc -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -g -I.

ALL = ff comp

# targets
all:	$(ALL)

comp:	comp.c ../Source/pcm_autogain.c ../Source/stream_filter_audio_agc.c
	$(CC) -I../Include -g -o comp comp.c

ff:	ff.c  ../Source/vobsub.c
	$(CC) -I ../Include  -g -o ff -lavformat -lavcodec -lavutil -lavfilter  ff.c

clean:	
	rm -f *.o $(ALL) *.out *~ *.bak core

