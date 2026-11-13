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

# AVOS test suite entry point. `make test` in the repository root runs this directory's default
# target (`check`); `make -C test help` lists the targets.
#
#   FFMPEG_PREFIX   install prefix of FFmpeg >= 7.1; only needed when FFmpeg is not installed
#                   system-wide (CI uses conda-forge ffmpeg 9). Exported to pkg-config and the
#                   dynamic loader for every recipe below.
#   CC              compiler for the replay/integration tests (default cc)
#   PYTEST          how to start pytest (default `python3 -m pytest`; pytest must be installed)
#
# Layout:  replay/  integration/  tooling/  device/  support/  tools/
# test/device/ needs an Android device and is not run by any target here.

HERE   := $(abspath .)
FFMPEG_PREFIX ?= /
PYTEST ?= python3 -m pytest
export FFMPEG_PREFIX

# The shell-driven checks (colorconv, subtitles) find FFmpeg through pkg-config; pytest gets the same
# from conftest.py.
ifneq ($(FFMPEG_PREFIX),/)
export PKG_CONFIG_PATH := $(FFMPEG_PREFIX)/lib/pkgconfig$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH))
export LD_LIBRARY_PATH := $(FFMPEG_PREFIX)/lib$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH))
export DYLD_LIBRARY_PATH := $(FFMPEG_PREFIX)/lib$(if $(DYLD_LIBRARY_PATH),:$(DYLD_LIBRARY_PATH))
endif

.PHONY: all help check replay integration tooling subtitles colorconv legacy clean
all: check

help:
	@echo "check       every automated test: all pytest suites, the subtitle host harness and the color-conversion check"
	@echo "replay      pytest: regression tests that recompile functions extracted from production C"
	@echo "integration pytest integration tests + the subtitle host harness + the color-conversion check"
	@echo "tooling     pytest: tests of the stress analyzers and campaign scripts"
	@echo "subtitles   native subtitle host harness alone (SANITIZERS=thread|address|undefined|none)"
	@echo "colorconv   FFmpeg color-conversion check alone (SANITIZERS=none to disable UBSan)"
	@echo "legacy      build the manual utilities in tools/legacy"
	@echo "(device/ needs an Android device and is not run here)"

# Every part always runs; the exit status is non-zero if any part failed.
check:
	@rc=0; \
	$(PYTEST) $(HERE) --ignore=$(HERE)/integration/subtitles || rc=1; \
	$(MAKE) subtitles || rc=1; \
	$(MAKE) colorconv || rc=1; \
	exit $$rc

replay tooling:
	$(PYTEST) $(HERE) -m $@ --ignore=$(HERE)/integration/subtitles

# "integration" is everything under integration/: the pytest part, the subtitle harness (its own runner,
# so pytest skips it) and the color-conversion check (a shell script that builds and runs a C program).
integration:
	@rc=0; \
	$(PYTEST) $(HERE) -m integration --ignore=$(HERE)/integration/subtitles || rc=1; \
	$(MAKE) subtitles || rc=1; \
	$(MAKE) colorconv || rc=1; \
	exit $$rc

subtitles:
	python3 $(HERE)/integration/subtitles/test_host_native.py

colorconv:
	bash $(HERE)/integration/ffmpeg/run-color-conversion.sh

legacy:
	$(MAKE) -C tools/legacy

clean:
	$(MAKE) -C tools/legacy clean
	rm -rf __pycache__ .pytest_cache
