"""Exercise cancelled-read recovery against real FFmpeg AVIO buffering and EOF."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from native_replay import extract, source


class FfmpegSeekCancelTest(unittest.TestCase):
    def test_cancel_seek_read_then_reach_eof_without_losing_real_errors(self):
        parser = source('stream_parser_ffmpeg.c')
        parse_once = parser[parser.index('static int _parse_once('):]
        handling = parse_once[parse_once.index('\tint demux_result = result;'):
                              parse_once.index('\n\tint stream = packet.stream_index;')]
        code = r'''
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
enum { FF_PARSE_ERROR = -1, FF_PARSE_PROGRESS = 0, FF_PARSE_END = 1, FF_PARSE_WAIT = 2 };
typedef struct { int parser_interrupt, abort, video_parse_end, audio_parse_end; } STREAM;
static int stream_abort(STREAM *s) { return s->abort; }
static int last_error;
static int parser_input_error(STREAM *s, int result, int demux_result) {
    (void)s; (void)demux_result; last_error = result; return FF_PARSE_ERROR;
}
''' + extract(parser, 'static void clear_seek_read_error(') + r'''
static int handle_result(STREAM *s, AVFormatContext *fmt, int result) {
    AVPacket packet = {0};
''' + handling + r'''
    return FF_PARSE_PROGRESS;
}
typedef struct { int pos, failure, partial; } INPUT;
static int read_input(void *opaque, uint8_t *buf, int size) {
    INPUT *input = opaque;
    if (input->failure && (!input->partial || input->pos == 128)) return input->failure;
    int count = FFMIN(size, 128 - input->pos);
    if (!count) return AVERROR_EOF;
    memset(buf, 0x55, count); input->pos += count; return count;
}
static int64_t seek_input(void *opaque, int64_t offset, int whence) {
    INPUT *input = opaque;
    if (whence == AVSEEK_SIZE) return 128;
    assert(whence == SEEK_SET && offset >= 0 && offset <= 128);
    input->pos = offset; return offset;
}
static void scenario(int failure, int interrupt, int abort, int partial) {
    INPUT input = {.failure = failure, .partial = partial};
    AVIOContext *pb = avio_alloc_context(av_malloc(32), 32, 0, &input,
                                        read_input, NULL, seek_input);
    assert(pb);
    AVFormatContext fmt = {.pb = pb};
    STREAM stream = {.parser_interrupt = interrupt, .abort = abort};
    uint8_t buf[256];
    int result = avio_read(pb, buf, sizeof(buf));
    assert(result == (partial ? 128 : failure) && pb->error == failure && pb->eof_reached);
    int handled = handle_result(&stream, &fmt, result);
    if (interrupt || abort) assert(handled == FF_PARSE_WAIT);
    else if (partial) assert(handled == FF_PARSE_PROGRESS);
    else assert(handled == FF_PARSE_ERROR && last_error == failure);
    int cleared = interrupt && !abort && failure == AVERROR_EXIT;
    assert(pb->error == (cleared ? 0 : failure));
    assert(pb->eof_reached == !cleared);

    // Successful seeks and packet reads must not resurrect a cancelled error
    // when the real end is reached. Genuine I/O failures remain observable.
    input.failure = 0;
    assert(avio_seek(pb, 0, SEEK_SET) == 0);
    assert(avio_read(pb, buf, sizeof(buf)) == 128);
    result = avio_read(pb, buf, sizeof(buf));
    stream.parser_interrupt = stream.abort = 0;
    if (cleared) {
        assert(result == AVERROR_EOF);
        assert(handle_result(&stream, &fmt, result) == FF_PARSE_END);
        assert(stream.video_parse_end && stream.audio_parse_end);
    } else {
        assert(result == failure);
        assert(handle_result(&stream, &fmt, AVERROR_EOF) == FF_PARSE_ERROR);
        assert(last_error == failure);
    }
    av_freep(&pb->buffer);
    avio_context_free(&pb);
}
int main(void) {
    for (int partial = 0; partial <= 1; partial++) {
        scenario(AVERROR_EXIT, 1, 0, partial);
        scenario(AVERROR(EIO), 1, 0, partial);
        scenario(AVERROR_EXIT, 0, 0, partial);
        scenario(AVERROR_EXIT, 1, 1, partial);
    }
    return 0;
}
'''
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'libavformat', 'libavcodec', 'libavutil'],
            text=True))
        with tempfile.TemporaryDirectory(prefix='avos-seek-cancel-') as directory:
            path = Path(directory) / 'replay.c'
            binary = path.with_suffix('')
            path.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                            '-Werror', str(path), '-o', str(binary), *flags], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
