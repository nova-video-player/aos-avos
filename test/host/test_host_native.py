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

#!/usr/bin/env python3
"""Host-side tests for the native subtitle code (libass geometry, SSA style cache, engine).

Each test compiles a C test program together with the REAL engine sources it exercises, links
it on the host, and runs it. No NDK, no device, no Makefile changes. Android-only headers are
replaced by host_stubs.h; the handful of symbols from the wider tree are weak stand-ins in
stubs.c; fake_backend.c replaces the SSA/SRT/GFX decoders where a test needs to control them.

Run (from anywhere):

    python3 test/host/test_host_native.py               # all
    python3 test/host/test_host_native.py -v -k race    # one test, every check listed
    SANITIZERS=thread python3 test/host/test_host_native.py -k race -k registry -k switch
    SANITIZERS=address,undefined python3 test/host/test_host_native.py -k jni
    HOST_TEST_OUTPUT=1 python3 test/host/test_host_native.py   # raw program output of every test
    NO_COLOR=1 python3 test/host/test_host_native.py           # plain text (also automatic when piped)

Environment:
    CC            compiler (default "cc"; may contain arguments, e.g. "ccache gcc")
    SANITIZERS    comma-separated -fsanitize= list; default "undefined"; "none" disables
    PKG_CONFIG_PATH  to select a host libass
    HOST_TEST_OUTPUT  set to print each test's raw stdout/stderr even when it passes
    NO_COLOR      disable colour (colour is only used on a terminal)

Output: one row per test (PASS / FAIL / SKIP, duration, number of checks). A failing test lists
its failing checks; -v lists every check; compiler warnings from the code under test are
collected into one de-duplicated section at the end instead of interleaving with the rows.

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
                 or detached thread, and 200 create/announce/destroy races with no leak. Run 4:
                 nativeDestroy() while a stream still holds the engine defers the destroy; the
                 last registry release frees the engine and the listener ref.
registry_lifetime  sub_engine_registry.c on its own: an engine is destroyed exactly once, never
                 while held, immediately when unheld and otherwise by the LAST release; retract
                 never blocks; a newer publish does not lose older references; the destroy
                 callback runs outside the registry lock; stray releases are ignored; 6 threads
                 acquire/release while 400 engines are published and retracted.
ssa_late_script  An external ASS script fed AFTER the first render: Custom (forced) settings must
                 apply without the user touching a setting. Compared against the same script fed
                 before the first render, with an unforced run proving the comparison can fail.
track_switch     Rapid switches between two large SRT/VTT feeds (and close_track) with generation
                 tokens: stale cues are dropped, none reach another track's backend, the last
                 track loads completely, nothing leaks.
seek_pause_feed  Pause / resume / seek racing the initial bulk feed, modelling the
                 flush-then-raise-refeed-flag protocol: no deadlock, pause state exact, one full
                 copy of the list afterwards.
close_open       Close a video and immediately open another while its parse worker is active, with
                 the Java side rebuilding the engine: every engine destroyed exactly once and never
                 while held, no cross-video cues, no leaked backend. Orderly and hostile close
                 orders.
style_resize_paused  Style changes and surface resizes while paused: each renders once at the frozen
                 clock, geometry reaches the backend as a coalesced pair ending on the last
                 request, style + resize storms on two threads, pause state untouched.

Scope: the four loading/seek/close/paused scenarios run against the real engine and the real
generation-token and registry code, with the STREAM-level workers modelled by small threads in the
tests (stream_sub_ext.c needs the whole STREAM to link). They prove the engine-side contract those
workers rely on; they do not execute stream_sub_ext.c itself.

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
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
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
REGISTRY = ('Source/sub_engine_registry.c',)      # opaque SUB_ENGINE* keys only: no engine needed
ENGINE = ('Source/sub_engine.c', 'Source/sub_render_gl.c', 'Source/sub_style.c',
          'Source/debug.c', HOST + 'stubs.c', HOST + 'fake_backend.c')
JNI = ENGINE + ('Source/jni_sub_engine.c', 'Source/sub_engine_registry.c')
LIFECYCLE = ENGINE + ('Source/sub_engine_registry.c',)   # real engine + real registry, fake backends

# Everything the engine links is JNI- or vtable-shaped (JNIEnv*/jobject on nearly every native
# entry point, SUB_FORMAT_BACKEND's fixed callback signatures, the mock JNINativeInterface_), so
# unused parameters are by design there, not mistakes; the warning class means nothing.
ENGINE_FLAGS = ('-Wno-pointer-sign', '-Wno-unused-parameter')

_BUILD = None   # temp dir, per test run
_OBJS = {}      # (source, cflags) -> object file; sources shared by several tests build once
_INFO = {}      # test method name -> RunInfo, filled by build_and_run(), read by the result printer
_WARNINGS = []  # compiler output from the files under test, shown once in the summary


class RunInfo:
    """What one test's C program printed and how it ran."""
    def __init__(self):
        self.output = ''
        self.seconds = 0.0
        self.checks_ok = 0
        self.checks_failed = 0
        self.failed_lines = []
        self.all_lines = []


_CHECK_LINE = re.compile(r'^\s*(ok|FAIL)\b')


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
                _WARNINGS.append(out)           # warnings from the files under test
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

        info = _INFO[self._testMethodName] = RunInfo()
        started = time.monotonic()
        try:
            run = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=RUN_TIMEOUT_S)
        except subprocess.TimeoutExpired as e:
            hung = e
        else:
            hung = None
        info.seconds = time.monotonic() - started
        if hung:
            info.output = _text(hung.stdout) + _text(hung.stderr)
            self.fail('%s timed out after %ds (deadlock?)' % (main, RUN_TIMEOUT_S))
        output = run.stdout + run.stderr
        info.output = output
        for line in output.splitlines():
            m = _CHECK_LINE.match(line)
            if not m:
                continue
            info.all_lines.append(line.rstrip())
            if m.group(1) == 'ok':
                info.checks_ok += 1
            else:
                info.checks_failed += 1
                info.failed_lines.append(line.rstrip())
        if run.returncode:
            how = ('killed by signal %d' % -run.returncode) if run.returncode < 0 else 'exit status %d' % run.returncode
            self.fail('%s: %s' % (main, how))


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

    def test_registry_lifetime(self):
        """Engine destroyed once, never while held, deferred to the last release."""
        self.build_and_run('registry_lifetime_test.c', REGISTRY, flags=ENGINE_FLAGS)

    def test_ssa_late_script(self):
        """External ASS script arriving after the first render still gets Custom settings."""
        self.build_and_run('ssa_late_script_test.c', SSA_BACKEND, pkgs=['libass'],
                           flags=('-Wno-unused-parameter',))

    def test_track_switch_loading(self):
        """Rapid switches between two large tracks while loading: no stale or foreign cues."""
        self.build_and_run('track_switch_loading_test.c', LIFECYCLE, flags=ENGINE_FLAGS)

    def test_seek_pause_feed(self):
        """Pause/resume/seek during the initial feed: no deadlock, one full copy afterwards."""
        self.build_and_run('seek_pause_feed_test.c', LIFECYCLE, flags=ENGINE_FLAGS)

    def test_close_open_parsing(self):
        """Close + immediately open a video while parsing, with engine rebuilds underneath."""
        self.build_and_run('close_open_parsing_test.c', LIFECYCLE, flags=ENGINE_FLAGS)

    def test_style_resize_paused(self):
        """Style change and surface resize while paused."""
        self.build_and_run('style_resize_paused_test.c', LIFECYCLE, flags=ENGINE_FLAGS)


# ----------------------------------------------------------------------------------------------
# Output formatting
# ----------------------------------------------------------------------------------------------
class _Style:
    def __init__(self):
        self.on = sys.stdout.isatty() and not os.environ.get('NO_COLOR')

    def _c(self, code, s):
        return '\033[%sm%s\033[0m' % (code, s) if self.on else s

    def green(self, s): return self._c('32', s)
    def red(self, s): return self._c('31;1', s)
    def yellow(self, s): return self._c('33', s)
    def dim(self, s): return self._c('2', s)
    def bold(self, s): return self._c('1', s)


def _tool_version(cmd):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=10).stdout.strip()
        return out.splitlines()[0] if out else None
    except (OSError, subprocess.SubprocessError):
        return None


def _environment_line():
    cc = shlex.split(os.environ.get('CC') or 'cc')
    parts = ['compiler: %s' % (_tool_version([*cc, '--version']) or ' '.join(cc) + ' (version unknown)'),
             'libass: %s' % (_tool_version(['pkg-config', '--modversion', 'libass']) or 'not found'),
             'sanitizers: %s' % (os.environ.get('SANITIZERS') or 'undefined')]
    return parts


class FormattedResult(unittest.TestResult):
    NAME_W = 22

    def __init__(self, verbosity, style):
        super().__init__()
        self.verbosity = verbosity
        self.s = style
        self.rows = []          # (name, status) in run order
        self._t0 = 0.0

    @staticmethod
    def _short(test):
        return test._testMethodName.replace('test_', '', 1)

    def startTest(self, test):
        super().startTest(test)
        self._t0 = time.monotonic()

    def _row(self, test, status, detail=''):
        s = self.s
        name = self._short(test)
        info = _INFO.get(test._testMethodName)
        secs = info.seconds if info and info.seconds else time.monotonic() - self._t0
        tag = {'PASS': s.green('PASS'), 'FAIL': s.red('FAIL'), 'SKIP': s.yellow('SKIP')}[status]
        counts = ''
        if info and (info.checks_ok or info.checks_failed):
            counts = '%d checks' % (info.checks_ok + info.checks_failed)
            if info.checks_failed:
                counts = s.red('%d of %s failed' % (info.checks_failed, counts))
        elif detail:
            counts = detail
        print('  %s  %-*s %6.2fs   %s' % (tag, self.NAME_W, name, secs, counts))
        doc = (test.shortDescription() or '').strip()
        if doc and status == 'FAIL':
            print(s.dim('        ' + doc))
        self.rows.append((name, status))
        return info

    def addSuccess(self, test):
        super().addSuccess(test)
        info = self._row(test, 'PASS')
        if info and (self.verbosity > 1 or os.environ.get('HOST_TEST_OUTPUT')):
            self._print_lines(info, everything=True)

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self._row(test, 'SKIP')
        first = (reason.splitlines() or [''])[0]
        print(self.s.dim('        ' + (first if len(first) <= 110 else first[:107] + '...')))

    def _fail(self, test, err, kind):
        info = self._row(test, 'FAIL')
        msg = str(err[1]).strip()
        if info and info.failed_lines:
            self._print_lines(info, everything=self.verbosity > 1)
        else:
            lines = msg.splitlines()
            print(self.s.red('        ' + (lines[0] if lines else kind)))
            for line in lines[1:40]:
                print('        ' + line)
            if len(lines) > 40:
                print(self.s.dim('        ... %d more lines (HOST_TEST_OUTPUT=1 for everything)' % (len(lines) - 40)))
        if msg and info and info.failed_lines and not msg.endswith('exit status 1'):
            print(self.s.red('        ' + msg.splitlines()[0]))      # e.g. "killed by signal 11" / "timed out"
        if info and os.environ.get('HOST_TEST_OUTPUT'):
            print(self.s.dim('        --- raw output ---'))
            for line in info.output.splitlines():
                print('        ' + line)

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._fail(test, err, 'failed')

    def addError(self, test, err):
        super().addError(test, err)
        self._fail(test, err, 'error')

    def _print_lines(self, info, everything):
        for line in (info.all_lines if everything else info.failed_lines):
            text = line.strip()
            if _CHECK_LINE.match(text) and text.startswith('FAIL'):
                print(self.s.red('        x ' + re.sub(r'^FAIL:?\s*', '', text)))
            else:
                print(self.s.dim('        . ' + re.sub(r'^ok:?\s*', '', text)))


class FormattedRunner:
    """unittest runner that prints the rows above plus a header and a summary."""
    def __init__(self, verbosity=1, **kwargs):
        self.verbosity = verbosity

    def run(self, suite):
        s = _Style()
        print(s.bold('AVOS native subtitle host tests'))
        for part in _environment_line():
            print(s.dim('  ' + part))
        print()
        result = FormattedResult(self.verbosity, s)
        started = time.monotonic()
        suite(result)
        elapsed = time.monotonic() - started

        warns = []
        for text in _WARNINGS:
            for line in text.splitlines():
                if 'warning:' in line and line not in warns:
                    warns.append(line)
        if warns:
            print()
            print(s.yellow('  Compiler warnings in the code under test (%d):' % len(warns)))
            for line in warns[:20]:
                print('    ' + line.replace(str(REPO) + '/', ''))
            if len(warns) > 20:
                print(s.dim('    ... %d more' % (len(warns) - 20)))

        passed = sum(1 for _, st in result.rows if st == 'PASS')
        failed = sum(1 for _, st in result.rows if st == 'FAIL')
        skipped = sum(1 for _, st in result.rows if st == 'SKIP')
        print()
        parts = ['%d passed' % passed, '%d failed' % failed, '%d skipped' % skipped]
        line = '  %s in %.1fs' % (', '.join(parts), elapsed)
        print(s.red(line) if failed else s.green(line))
        if failed:
            print(s.red('  failed: ') + ', '.join(n for n, st in result.rows if st == 'FAIL'))
        return result


if __name__ == '__main__':
    unittest.main(testRunner=FormattedRunner)
