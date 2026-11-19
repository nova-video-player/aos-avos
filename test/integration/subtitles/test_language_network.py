# Copyright 2026 Courville Software
# SPDX-License-Identifier: Apache-2.0
"""Explicit analysis deadlines and real demuxing through a delayed range server."""
import os
import re
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest
from test_language import AVOS, PREBUILT, build_preflight


def production_function(source, name):
    match = re.search(r"static (?:int|void) " + name + r"\([^)]*\)\s*\{", source)
    assert match
    start = match.start()
    opening = source.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_explicit_retriever_does_not_inherit_metadata_deadline(tmp_path):
    source = (AVOS / "Source/avos_mr.c").read_text()
    main = tmp_path / "deadline.c"
    main.write_text(r'''
#include "sub_language.h"
#include <assert.h>
#include <stdlib.h>
#define CONFIG_FFMPEG_PARSER 1
#define AVOS_ERR_OK 0
#define AVOS_ERR -1
#define acalloc calloc
#define afree free
static int64_t clock_us;
static int64_t av_gettime_relative(void) { return clock_us; }
typedef struct { int unused; } STREAM_URL;
typedef struct { void *abort_opaque; } FILE_INFO;
typedef struct { int cancelled, info_valid; int64_t deadline_us; STREAM_URL src; FILE_INFO info; } avos_mr_t;
static int avos_mr_fillmetadata(avos_mr_t *mr) { (void)mr; return AVOS_ERR_OK; }
static int ffmpeg_guess_subtitle_languages(STREAM_URL *src, FILE_INFO *info, int (*abort)(void *)) {
    (void)src;
    clock_us = 40000000;
    assert(!abort(info->abort_opaque)); /* Regression: ordinary MR stopped here at 30 seconds. */
    clock_us = SUB_LANGUAGE_SCAN_TIMEOUT_US + 1;
    assert(abort(info->abort_opaque));
    return 0;
}
''' + "\n".join(production_function(source, name) for name in (
        "avos_mr_aborted", "avos_mr_begin", "avos_mr_guess_subtitle_languages")) + r'''
int main(void) {
    avos_mr_t mr = {0};
    assert(avos_mr_guess_subtitle_languages(&mr) == AVOS_ERR_OK);
    clock_us = 0;
    avos_mr_begin(&mr);
    clock_us = 30000001;
    assert(avos_mr_aborted(&mr)); /* Normal metadata keeps its original limit. */
    clock_us = 0;
    avos_mr_begin(&mr);
    mr.cancelled = 1;
    assert(avos_mr_aborted(&mr)); /* User cancellation still takes effect immediately. */
}
''')
    binary = tmp_path / "deadline"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(AVOS / "Include"),
                    str(main), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_explicit_analysis_avoids_remote_cues_and_identifies_text(tmp_path):
    sample = os.environ.get("AVOS_LANGUAGE_SAMPLE")
    if not sample:
        pytest.skip("Set AVOS_LANGUAGE_SAMPLE after building AVOS with FASTTEXT=ON")
    source = Path(sample)
    length = source.stat().st_size
    offsets = []

    class RangeServer(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
            start = int(match[1]) if match else 0
            end = min(int(match[2]), length - 1) if match and match[2] else length - 1
            offsets.append(start)
            # Loading distant Cues must not be necessary to sample initial dialogue.
            if start > length // 2:
                self.send_error(503)
                return
            time.sleep(0.02)
            self.send_response(206 if match else 200)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Type", "video/x-matroska")
            self.send_header("Content-Length", str(end - start + 1))
            if match:
                self.send_header("Content-Range", f"bytes {start}-{end}/{length}")
            self.end_headers()
            try:
                with source.open("rb") as stream:
                    stream.seek(start)
                    remaining = end - start + 1
                    while remaining > 0:
                        chunk = stream.read(min(65536, remaining))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        remaining -= len(chunk)
                        time.sleep(0.001)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), RangeServer)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        binary = build_preflight(tmp_path)
        result = subprocess.run([str(binary), f"http://127.0.0.1:{server.server_port}/sample.mkv",
                                 str(PREBUILT / "share/fastText/lid.176.ftz"), "unused"],
                                capture_output=True, text=True, timeout=65)
        assert result.returncode == 0, result.stderr
        tracks = [line for line in result.stderr.splitlines() if line.startswith("TRACK ")]
        assert len(tracks) == 38
        assert tracks[12].startswith("TRACK 13 fr 3 "), result.stderr
        assert tracks[13].startswith("TRACK 14 fr 3 "), result.stderr
        assert max(offsets) < length // 2, offsets
        assert len([line for line in tracks if re.search(r" [a-z]{2,3} 3 \d+$", line)]) >= 20
        print("\n".join(line for line in result.stderr.splitlines()
                        if line.startswith(("RESULT", "TRACK", "subtitle language scan:"))))
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
