# Copyright 2026 Courville Software
# SPDX-License-Identifier: Apache-2.0
"""Compile the production resolver/session and exercise real-model lifecycle and inference."""
import os
import subprocess
from pathlib import Path

import pytest

AVOS = Path(__file__).resolve().parents[3]
BUILDER = AVOS.parent / "fastText-android-builder"
PREBUILT = AVOS.parent / "prebuilt/fastText"


def test_language_session(tmp_path):
    library = Path(os.environ.get("FASTTEXT_HOST_LIBRARY", str(BUILDER / "build-host/libfasttext.a")))
    if not library.is_file():
        pytest.skip("Build the fastText builder's host static library first")
    binary = tmp_path / "language"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++11", "-pthread", "-Wall", "-Wextra", "-Werror",
        "-DCONFIG_FASTTEXT", "-I" + str(AVOS / "Include"), "-isystem", str(PREBUILT / "include"),
        str(AVOS / "Source/sub_language.cpp"), str(Path(__file__).with_name("language.cpp")),
        str(library), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), str(PREBUILT / "share/fastText/lid.176.ftz")], check=True, timeout=30)


def test_without_fasttext(tmp_path):
    binary = tmp_path / "no_model"
    main = tmp_path / "main.cpp"
    main.write_text('#include "sub_language.h"\n#include <cassert>\n'
                    'int main() { char language[32]; assert(!sub_language_create()); '
                    'assert(sub_language_resolve("und", "French (SDH)", language, sizeof(language)) '
                    '== SUB_LANG_DESCRIPTION); }\n')
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++11", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(AVOS / "Include"), str(AVOS / "Source/sub_language.cpp"), str(main),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_persistent_language_cache(tmp_path):
    binary = tmp_path / "cache_test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++11", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(AVOS / "Include"), str(AVOS / "Source/sub_language_cache.cpp"),
                    str(Path(__file__).with_name("language_cache.cpp")), "-o", str(binary)], check=True)
    # Separate processes prove persistence independently of any in-memory state.
    subprocess.run([str(binary), str(tmp_path / "cache"), "write"], check=True)
    subprocess.run([str(binary), str(tmp_path / "cache"), "read"], check=True)


def test_independent_preflight_and_persistent_results(tmp_path):
    """Opt-in production demux test; supply the unlabelled Ride.or.Die MKV locally."""
    sample = os.environ.get("AVOS_LANGUAGE_SAMPLE")
    if not sample:
        pytest.skip("Set AVOS_LANGUAGE_SAMPLE to the Ride.or.Die MKV after make FASTTEXT=ON")
    import shlex
    plan = subprocess.check_output(["make", "-n", "-W", "Source/main.c", "FASTTEXT=ON", "VERBOSE=1"],
                                   cwd=AVOS, text=True)
    commands = [shlex.split(line) for line in plan.splitlines()
                if "Source/main.c" in line or ("build/avos" in line and "build/main.o" in line)]
    compile_command = next(c for c in commands if c[0] != "echo" and "Source/main.c" in c and "-c" in c)
    link_command = next(c for c in commands if c[0] != "echo" and "-o" in c and "build/avos" in c and "build/main.o" in c)
    binary = tmp_path / "preflight"
    replacements = {"Source/main.c": str(Path(__file__).with_name("preflight.c")),
                    "build/main.o": str(tmp_path / "preflight.o"), "build/avos": str(binary)}
    for command in (compile_command, link_command):
        subprocess.run([replacements.get(arg, arg) for arg in command], cwd=AVOS, check=True)
    args = [str(binary), str(Path(sample).resolve()), str(PREBUILT / "share/fastText/lid.176.ftz"),
            str(tmp_path / "cache")]
    cold = subprocess.run(args, capture_output=True, text=True, check=True, timeout=15)
    warm = subprocess.run(args, capture_output=True, text=True, check=True, timeout=15)
    cold_tracks = [line for line in cold.stderr.splitlines() if line.startswith("TRACK ")]
    warm_tracks = [line for line in warm.stderr.splitlines() if line.startswith("TRACK ")]
    assert len(cold_tracks) == 38
    assert warm_tracks == cold_tracks
    assert cold_tracks[12].startswith("TRACK 13 fr 3 ")
    assert cold_tracks[13].startswith("TRACK 14 fr 3 ")
    cancelled = subprocess.run(args + ["cancel"], capture_output=True, text=True, timeout=5)
    assert cancelled.returncode == 1
    assert "RESULT status=1" in cancelled.stderr
