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

"""Exercise Profile 9 AVC codec-config framing."""
import unittest
from native_replay import extract, run_c, source
from test_dovi_base_codec import PREAMBLE

class DoviAvcTest(unittest.TestCase):
    def test_profile9_prepares_avcc_without_changing_dv_selection(self):
        text = source('stream_video.c')
        start=text.index('        const int is_avc =')
        end=text.index(';',start)+1
        guard=text[start:end]+extract(text,'if( is_avc && s->video->extraDataSize')
        h264=source('h264.c')
        conversion=extract(h264,'static int _convert_extradata(')+extract(h264,'int H264_convert_extradata(')
        preamble=PREAMBLE.replace('int format,', 'struct { int width,height; } sps; int width,height,avcc,needs_header; int format,')
        run_c(preamble+r'''
#define DBGP if (0)
static const UCHAR sync_word[4]={0,0,0,1};
static int H264_parse_avcc(VIDEO_PROPERTIES *v, void *a, void *b, int *size) {
    (void)a; (void)b; *size=(v->extraData[4]&3)+1; return 0;
}
'''+conversion+r'''
static void prepare(VIDEO_PROPERTIES *video) {
    struct { VIDEO_PROPERTIES *video; } stream={video}, *s=&stream;
'''+guard+r'''
}
int main(void) {
    UCHAR avcc[]={1,100,0,40,255,225,0,4,0x67,0x64,0,40,1,0,2,0x68,0};
    UCHAR annexb[]={0,0,0,1,0x67,0x64,0,40,0,0,0,1,0x68,0};
    VIDEO_PROPERTIES v={.codec_id=AV_CODEC_ID_H264,
        .format=VIDEO_FORMAT_DOLBY_VISION,.fourcc=VIDEO_FOURCC_DOLBY_VISION};
    memcpy(v.extraData,avcc,sizeof(avcc)); v.extraDataSize=sizeof(avcc);
    prepare(&v);
    assert(v.avcc && v.needs_header && v.nal_unit_size==4);
    assert(v.format==VIDEO_FORMAT_DOLBY_VISION && v.fourcc==VIDEO_FOURCC_DOLBY_VISION);
    assert(v.extraDataSize==sizeof(annexb) && !memcmp(v.extraData,annexb,sizeof(annexb)));
    v.codec_id=AV_CODEC_ID_AV1; v.avcc=0;
    memcpy(v.extraData,avcc,sizeof(avcc)); v.extraDataSize=sizeof(avcc);
    prepare(&v);
    assert(!v.avcc && v.extraDataSize==sizeof(avcc));
    return 0;
}
''')
