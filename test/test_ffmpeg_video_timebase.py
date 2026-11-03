"""Video timestamps must use their packet's stream, including after seeking."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from native_replay import extract, source


class FfmpegVideoTimebaseTest(unittest.TestCase):
    def test_per_stream_clock(self):
        parser = source('stream_parser_ffmpeg.c')
        code = r'''
#include <assert.h>
#include <stdlib.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
typedef int64_t INT64;
typedef struct { AVFormatContext *fmt; int start_time; } PRIVATE;
typedef struct { PRIVATE *priv; } STREAM;
#define ff_p (s->priv)
static int use_pts = 1;
static double inv_speed = 1.0;
#define RST_TO_TS_TIME(t, type) ((type)((t) * inv_speed))
''' + extract(parser, 'static INT64 _get_video_timestamp(') + '\n' + extract(parser, 'static int _get_video_media_time(') + '\n' + extract(
            parser, 'static int _get_video_time(') + r'''
static void real_file(const char *path) {
    AVFormatContext *fmt = NULL;
    assert(avformat_open_input(&fmt, path, NULL, NULL) == 0);
    assert(avformat_find_stream_info(fmt, NULL) >= 0);
    int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    assert(video >= 0);
    PRIVATE priv = {.fmt = fmt};
    STREAM stream = {.priv = &priv};
    AVPacket *pkt = av_packet_alloc();
    assert(pkt);
    for (int pass = 0; pass < 2; pass++) {
        if (pass) assert(av_seek_frame(fmt, -1, 600 * AV_TIME_BASE, AVSEEK_FLAG_BACKWARD) >= 0);
        int count = 0;
        while (count < 100 && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == video && pkt->pts != AV_NOPTS_VALUE) {
                int expected = (int)(pkt->pts * av_q2d(fmt->streams[video]->time_base) * 1000);
                assert(abs(_get_video_media_time(&stream, pkt) - expected) <= 1);
                count++;
            }
            av_packet_unref(pkt);
        }
        assert(count == 100);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
}
int main(void) {
    AVFormatContext *fmt = avformat_alloc_context();
    assert(fmt);
    AVStream *main_video = avformat_new_stream(fmt, NULL);
    AVStream *thumbnail = avformat_new_stream(fmt, NULL);
    assert(main_video && thumbnail);
    main_video->time_base = (AVRational){1, 90000};
    thumbnail->time_base = (AVRational){1, 1000};
    PRIVATE priv = {.fmt = fmt};
    STREAM stream = {.priv = &priv};
    AVPacket packet = {.stream_index = 0, .pts = 3753, .dts = 0};
    assert(_get_video_time(&stream, &packet) == 41);
    packet.pts = 7507;
    assert(_get_video_time(&stream, &packet) == 83);
    packet.stream_index = 1;
    assert(_get_video_time(&stream, &packet) == 7507);
    packet.stream_index = 0;
    packet.pts = 90000;
    priv.start_time = 250;
    assert(_get_video_time(&stream, &packet) == 750);
    inv_speed = 0.5;
    assert(_get_video_media_time(&stream, &packet) == 750);
    assert(_get_video_time(&stream, &packet) == 375);
    inv_speed = 1.0;
    priv.start_time = 0;
    packet.pts = AV_NOPTS_VALUE;
    packet.dts = -7507;
    assert(_get_video_time(&stream, &packet) == -83);
    packet.dts = AV_NOPTS_VALUE;
    assert(_get_video_time(&stream, &packet) == -1);
    use_pts = 0;
    packet.pts = 90000;
    packet.dts = 45000;
    assert(_get_video_time(&stream, &packet) == 500);
    use_pts = 1;
    // A non-unit numerator still scales correctly; stream ordering is irrelevant.
    main_video->time_base = (AVRational){1001, 30000};
    packet.pts = 30;
    assert(_get_video_time(&stream, &packet) == 1001);
    avformat_free_context(fmt);
    const char *path = getenv("AVOS_VIDEO_TIMEBASE_SAMPLE");
    if (path) real_file(path);
    return 0;
}
'''
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'libavformat', 'libavcodec', 'libavutil'],
            text=True))
        with tempfile.TemporaryDirectory(prefix='avos-video-timebase-') as directory:
            path = Path(directory) / 'replay.c'
            binary = path.with_suffix('')
            path.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                            '-Werror', str(path), '-o', str(binary), *flags], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
