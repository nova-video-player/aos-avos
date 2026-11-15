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
