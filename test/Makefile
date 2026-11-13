# AVOS test suite entry point (`make test` in the repository root runs this).   make -C test help
#
#   FFMPEG_PREFIX   install prefix of FFmpeg >= 7.1 (CI uses conda-forge ffmpeg 9)
#   CC              compiler for the replay/integration tests (default cc)
#
# Layout:  replay/  integration/  tooling/  device/  support/  tools/

HERE   := $(abspath .)
FFMPEG_PREFIX ?= /
PYTEST ?= python3 -m pytest
export FFMPEG_PREFIX

.PHONY: all help check replay integration tooling subtitles legacy clean
all: check

help:
	@echo "check       run every automated test"
	@echo "replay|integration|tooling   one test type"
	@echo "subtitles   native subtitle host harness (SANITIZERS=thread|address|undefined)"
	@echo "legacy      build the manual utilities in tools/legacy"

# Both halves always run; the exit status is non-zero if either one failed.
check:
	@rc=0; \
	$(PYTEST) $(HERE) --ignore=$(HERE)/integration/subtitles || rc=1; \
	$(MAKE) subtitles || rc=1; \
	exit $$rc

replay integration tooling:
	$(PYTEST) $(HERE) -m $@ --ignore=$(HERE)/integration/subtitles

subtitles:
	python3 $(HERE)/integration/subtitles/test_host_native.py

legacy:
	$(MAKE) -C tools/legacy

clean:
	$(MAKE) -C tools/legacy clean
	rm -rf __pycache__ .pytest_cache
