#!/usr/bin/env python3
"""Host-side tests for the native subtitle code (libass geometry, SSA style cache, engine).

Each test compiles a C test program together with the REAL engine sources it exercises, links
it on the host, and runs it. No NDK, no device, no Makefile changes. Android-only headers are
replaced by host_stubs.h; the handful of symbols from the wider tree are weak stand-ins in
stubs.c; fake_backend.c replaces the SSA/SRT/GFX decoders where a test needs to control them.

Run (from anywhere):

    python3 test/host/test_host_native.py               # all
    python3 test/host/test_host_native.py -v -k race    # one, with names
    SANITIZERS=thread python3 test/host/test_host_native.py -k race
    SANITIZERS=address,undefined python3 test/host/test_host_native.py -k jni
    HOST_TEST_OUTPUT=1 python3 test/host/test_host_native.py   # also show passing output

Environment:
    CC            compiler (default "cc"; may contain arguments, e.g. "ccache gcc")
    SANITIZERS    comma-separated -fsanitize= list; default "undefined"; "none" disables
    PKG_CONFIG_PATH  to select a host libass
    HOST_TEST_OUTPUT  set to print each test's stdout/stderr even when it passes

Tests that need libass are SKIPPED (not failed) when pkg-config can't find it. Compiler
warnings are shown; sources that are borrowed rather than under test (see QUIET) are built
with -w. This directory is not a package, so the repo's top-level unittest discovery does not
pick it up -- it needs a host toolchain the other tests don't.

What each test covers
---------------------
ssa_geometry     ssa_geom_compute()/ssa_geom_apply() against a real libass: synthetic ASS
                 scripts rendered through ass_render_frame(), ink bounding boxes compared with
                 a box-only reference render (regular bottom-aligned text, \\pos text,
                 SBAS=no/outline). Portrait/landscape/pillarbox/tablet layouts plus degenerate
                 and transient inputs (zero-size canvas or box, orientation-mismatched coded
                 size): margins never go negative, and storage size is dropped, not misapplied.
ssa_style_cache  Real sub_format_ssa.c through its open()/resize()/set_video_box()/render_at()
                 vtable: forced FontSize must be recomputed when video geometry changes (e.g.
                 black bars removed or content height changed).
race_harness     open_track()/close_track() hammered against a deliberately slow fake backend;
                 nothing may be visible once close_track() has returned. This is the bug fixed
                 by doing poll+publish in one engine-lock hold; reverting that fix turns
                 stale-frame-after-close=0 into thousands of hits (checked by mutation).
fill_contract    The renderer's UNCHANGED / CLEAR / FRAME / ERROR contract and exactly when
                 frame_generation moves.
push_policy      The native "content changed" push for the 3D path: only while paused, only in
                 a 3D UI mode, once per generation, with no engine or renderer lock held.
jni_listener     The same policy through the real jni_sub_engine.c against a mock JavaVM: one
                 global ref per engine, attach once / detach at exit, no call on a freed ref
                 or detached thread, and 200 create/announce/destroy races with no leak.

What this does NOT prove
------------------------
The mock JavaVM validates our use of the JNI contract, not a real ART. The render thread never
gets a real EGL surface (same as production 3D mode with no window attached), so the GPU blend
path is link-only here. Please still smoke-test on a device: pause in 3D, switch (or disable)
the subtitle track, and confirm the old cue clears and the new one appears without resuming.
"""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent        # <repo>/test/host
REPO = HERE.parents[1]                        # <repo>
HOST = 'test/host/'                           # repo-relative prefix for this directory
RUN_TIMEOUT_S = 120                           # a hung test is a deadlock, not a slow test

# <header> paths the engine includes; each becomes a one-line forwarder to host_stubs.h in the
# temp build dir, so no Source/ file has to change.
STUB_HEADERS = ('jni.h', 'android/native_window.h', 'android/native_window_jni.h',
                'android/bitmap.h', 'android/log.h', 'EGL/egl.h', 'GLES2/gl2.h')

# Borrowed for realism, not under test: built with -w so their pre-existing warnings don't
# bury the ones from the files that are. (log.c is deliberately absent: see stubs.c.)
QUIET = {'Source/debug.c', HOST + 'stubs.c'}

# Link sets. Repo-relative; the test's own .c is passed separately.
SSA_BACKEND = ('Source/sub_format_ssa.c', 'Source/sub_style.c', 'Source/debug.c', HOST + 'stubs.c')
ENGINE = ('Source/sub_engine.c', 'Source/sub_render_gl.c', 'Source/sub_style.c',
          'Source/debug.c', HOST + 'stubs.c', HOST + 'fake_backend.c')
JNI = ENGINE + ('Source/jni_sub_engine.c', 'Source/sub_engine_registry.c')

# Everything the engine links is JNI- or vtable-shaped (JNIEnv*/jobject on nearly every native
# entry point, SUB_FORMAT_BACKEND's fixed callback signatures, the mock JNINativeInterface_), so
# unused parameters are by design there, not mistakes; the warning class means nothing.
ENGINE_FLAGS = ('-Wno-pointer-sign', '-Wno-unused-parameter')

_BUILD = None   # temp dir, per test run
_OBJS = {}      # (source, cflags) -> object file; sources shared by several tests build once


def setUpModule():
    global _BUILD
    _BUILD = Path(tempfile.mkdtemp(prefix='avos-host.'))
    for rel in STUB_HEADERS:
        fwd = _BUILD / 'inc' / rel
        fwd.parent.mkdir(parents=True, exist_ok=True)
        fwd.write_text('#include "%s"\n' % (HERE / 'host_stubs.h'))


def tearDownModule():
    shutil.rmtree(_BUILD, ignore_errors=True)


def _sanitize_flags():
    sanitizers = os.environ.get('SANITIZERS') or 'undefined'
    return [] if sanitizers == 'none' else ['-fsanitize=' + sanitizers, '-fno-omit-frame-pointer']


def _pkg_config(flag, pkgs):
    if not pkgs:
        return []
    try:
        out = subprocess.run(['pkg-config', flag, *pkgs], capture_output=True, text=True, check=True).stdout
    except FileNotFoundError:
        raise unittest.SkipTest('pkg-config is not installed (needed for %s)' % ' '.join(pkgs)) from None
    except subprocess.CalledProcessError as e:
        raise unittest.SkipTest('pkg-config cannot find %s (try PKG_CONFIG_PATH): %s'
                                % (' '.join(pkgs), e.stderr.strip().splitlines()[0])) from None
    return shlex.split(out)


def _text(data):
    return data.decode(errors='replace') if isinstance(data, bytes) else (data or '')


def _build_objects(cc, jobs):
    """Compile (source, cflags) jobs in parallel, reusing objects. Returns (objects, errors)."""
    objs, todo = [], []
    for src, cflags in jobs:
        key = (src, cflags)
        if key not in _OBJS:
            _OBJS[key] = _BUILD / ('%d-%s.o' % (len(_OBJS), src.stem))
            todo.append((key, _OBJS[key]))
        objs.append(_OBJS[key])

    def compile_one(item):
        (src, cflags), obj = item
        p = subprocess.run([*cc, *cflags, '-c', str(src), '-o', str(obj)], capture_output=True, text=True)
        return item, p.returncode, p.stdout + p.stderr

    errors = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for (key, _), rc, out in pool.map(compile_one, todo):
            if rc:
                del _OBJS[key]                  # don't let a later test reuse a missing object
                errors.append('%s:\n%s' % (key[0], out))
            elif out.strip():
                sys.stderr.write(out)           # warnings from the files under test
    return objs, errors


class HostTestCase(unittest.TestCase):
    def build_and_run(self, main, sources=(), *, pkgs=(), flags=(), args=()):
        """Compile `main` (a file in this directory) with `sources` (repo-relative), link, run.

        Compiles run in parallel; the test itself runs alone because several of them are
        timing-sensitive.
        """
        cc = shlex.split(os.environ.get('CC') or 'cc')
        pkg_cflags, pkg_libs = _pkg_config('--cflags', pkgs), _pkg_config('--libs', pkgs)
        sanitize = _sanitize_flags()
        base = ['-std=gnu11', '-g', '-O1']
        includes = ['-I', str(REPO / 'Include'), '-I', str(_BUILD / 'inc')]
        checked = tuple([*base, '-Wall', '-Wextra', '-Wno-deprecated-declarations',
                         *sanitize, *flags, *includes, *pkg_cflags])
        quiet = tuple([*base, '-w', *includes])

        jobs = [(HERE / main, checked)]
        for rel in sources:
            jobs.append((REPO / rel, quiet if rel in QUIET else checked))
        for src, _ in jobs:
            if not src.is_file():
                self.fail('missing source file: %s' % src)

        objs, errors = _build_objects(cc, jobs)
        if errors:
            self.fail('compile failed:\n' + '\n'.join(errors))

        exe = _BUILD / (Path(main).stem + '.bin')
        link = subprocess.run([*cc, *sanitize, *map(str, objs), *pkg_libs, '-lpthread', '-o', str(exe)],
                              capture_output=True, text=True)
        if link.returncode:
            self.fail('link failed:\n' + link.stdout + link.stderr)

        try:
            run = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=RUN_TIMEOUT_S)
        except subprocess.TimeoutExpired as e:
            hung = e
        else:
            hung = None
        if hung:
            self.fail('%s timed out after %ds (deadlock?)\n%s%s'
                      % (main, RUN_TIMEOUT_S, _text(hung.stdout), _text(hung.stderr)))
        output = run.stdout + run.stderr
        if os.environ.get('HOST_TEST_OUTPUT'):
            sys.stderr.write(output)
        if run.returncode:
            how = ('killed by signal %d' % -run.returncode) if run.returncode < 0 else 'exit status %d' % run.returncode
            self.fail('%s: %s\n%s' % (main, how, output))


class HostNativeTests(HostTestCase):
    def test_ssa_geometry(self):
        """ssa_geom_compute()/ssa_geom_apply() vs a box-only libass reference render."""
        self.build_and_run('ssa_geometry_test.c', pkgs=['libass'])

    def test_ssa_style_cache(self):
        """Forced FontSize is recomputed when video geometry changes."""
        self.build_and_run('ssa_style_cache_test.c', SSA_BACKEND, pkgs=['libass'],
                           flags=('-Wno-unused-parameter',))

    def test_race_harness(self):
        """Nothing is visible once close_track() has returned."""
        self.build_and_run('race_harness_test.c', ENGINE, flags=ENGINE_FLAGS)

    def test_fill_contract(self):
        """UNCHANGED / CLEAR / FRAME / ERROR and when frame_generation moves."""
        self.build_and_run('fill_contract_test.c', ENGINE, flags=ENGINE_FLAGS)

    def test_push_policy(self):
        """3D 'content changed' push: paused only, 3D only, once per generation, no locks held."""
        self.build_and_run('push_policy_test.c', ENGINE, flags=ENGINE_FLAGS)

    def test_jni_listener(self):
        """Listener ref lifetime and thread attach/detach, against a mock JavaVM."""
        self.build_and_run('jni_listener_test.c', JNI, flags=ENGINE_FLAGS)


if __name__ == '__main__':
    unittest.main()
