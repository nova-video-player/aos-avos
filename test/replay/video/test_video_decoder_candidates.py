#
# Copyright 2026 Courville Software
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

"""Exercise production hardware decoder retry cleanup."""
import unittest
from native_replay import run_c, source

class VideoDecoderCandidatesTest(unittest.TestCase):
    def test_candidate_failures_cleanup_and_stop_after_sixteen(self):
        text = source('codec_sfdec2.c')
        retry = text[text.index('next_decoder_candidate:'):text.index('err:', text.index('next_decoder_candidate:'))]
        run_c(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define afree free
#define CLOG(...) ((void)0)
static int deleted, queries, available;
static void sfdec_delete(void *codec) { assert(codec); deleted++; }
static const char *acodecs_get_video_decoder(const char *mime, int profile,
        int width, int height, double rate, int index) {
    assert(!strcmp(mime,"video/dolby-vision") && profile==1024);
    assert(width==3840 && height==2160 && rate==24);
    queries++;
    return index < available ? strdup("next.decoder") : NULL;
}
static int replay(int named, int success_at) {
    struct { void *sfdec; } state={0}, *p=&state;
    struct { int width,height; } v={3840,2160}, *video=&v;
    const char *decoder_name=named ? strdup("first.decoder") : NULL;
    const char *selection_mime="video/dolby-vision";
    int selection_profile=1024, candidate_index=0, attempts=0;
    double selection_rate=24;
retry_decoder_candidate:
    attempts++;
    p->sfdec=(void *)1;
    if (attempts == success_at) {
        free((void *)decoder_name);
        return attempts;
    }
    goto next_decoder_candidate;
''' + retry + r'''
    free((void *)decoder_name);
    assert(!p->sfdec);
    return attempts;
}
int main(void) {
    available=3;
    assert(replay(1,2)==2 && deleted==1 && queries==1);
    deleted=queries=0;
    assert(replay(1,0)==3 && deleted==3 && queries==3);
    deleted=queries=0; available=100;
    assert(replay(1,0)==16 && deleted==16 && queries==15);
    deleted=queries=0;
    assert(replay(0,0)==1 && deleted==1 && queries==0);
    return 0;
}
''')
