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
    SANITIZERS=thread python3 test/host/test_host_native.py -k race -k registry -k ext_
    SANITIZERS=address,undefined python3 test/host/test_host_native.py -k jni
    HOST_TEST_OUTPUT=1 python3 test/host/test_host_native.py   # raw program output of every test
    NO_COLOR=1 python3 test/host/test_host_native.py           # plain text (also automatic when piped)

Environment:
    CC            compiler (default "cc"; may contain arguments, e.g. "ccache gcc")
    SANITIZERS    comma-separated -fsanitize= list; default "undefined"; "none" disables
    PKG_CONFIG_PATH  to select a host libass
    HOST_TEST_OUTPUT  set to print each test's raw stdout/stderr even when it passes
    HOST_TEST_LOG     set to 1 to print the app's own log lines (serprintf) from the ext_* tests, in the same
                      format as a device log: "stream_set_subtitle_stream( 1 )", "sub_ext_feed_engine: ..."
    HOST_BASELINE_REV  a git revision to build the ext_* tests' CODE UNDER TEST from: every Source/ file of
                      the real stack and the whole Include/ come from that one revision (only the test
                      files and the host stand-ins stay current), so the stack is consistent -- one file
                      from an old revision on top of today's others is not a baseline, because fixes live in
                      more than one file (the forced refeed in stream_subtitle.c, for one). To prove the
                      tests FAIL on a buggy revision, use the parent of the first #2004 fix:
                          HOST_BASELINE_REV=c4cfcb8~1 python3 test/host/test_host_native.py -k ext_
                      (needs a git checkout that has the revision)
    HOST_TEST_FONT, HOST_TEST_FONT_FAMILY   use this font file (and its family name) for the libass tests
    NO_COLOR      disable colour (colour is only used on a terminal)

Output: one row per test (PASS / FAIL / SKIP, duration, number of checks). A failing test lists
its failing checks; -v lists every check; compiler warnings from the code under test are
collected into one de-duplicated section at the end instead of interleaving with the rows.

Tests that need libass are SKIPPED (not failed) when pkg-config can't find it. They also need one real
TrueType/OpenType font file; none is bundled. The harness uses, in order: HOST_TEST_FONT (+
HOST_TEST_FONT_FAMILY), a well-known system font (Arial on macOS; DejaVu Sans / Liberation Sans / Noto
Sans on Linux), or fontconfig's sans-serif match. The file is registered with libass directly, so
fontconfig itself is not required. Every check compares text rendered with the SAME font against
itself, so which font is used does not matter -- only that glyphs render. Compiler
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
ext_reselect     (SRT and VTT) Issue #2004 through the real stack: select 02, 01, 02, 01 ... each selection
                 must leave exactly that file's cues in the engine; double open; reopen while the track's
                 own feed is running; a storm of back-to-back selections; selecting while the engine is
                 paused; subtitles switched off mid-feed with stream_disable_subtitles() (nothing visible, no late cue, no reopen) and back on; the
                 finished track renders. A track that is accepted but never fed is reported as a SILENT
                 FAILURE (the report's n_events=0).
ext_interrupt    (SRT and VTT) A pause/seek/rescan (idling the subtitle thread) landing late in a long feed
                 must not leave the track empty or cache a truncated cue list (reselect replays the cache);
                 a storm of engine pause / resume / seek / subtitle-thread pauses racing one feed (pause
                 state exact, one full copy, renders); the flush + refeed-flag protocol on a loaded track.
ext_close_open   (SRT and VTT) Close a video and immediately open another while its real parse worker is
                 mid-feed, with the Java side rebuilding the engine (registry publish/retract), in
                 avos_mp_video_close()'s order: every engine destroyed exactly once and never while held, no
                 cross-video cues, no leaked backend, no hang while the pool is torn down mid-feed.
gen_token        ENGINE-LEVEL (plain feeder threads, not stream_sub_ext.c): the track-generation token
                 contract -- a stale token's feed/flush never reaches the new track, every cue a feeder saw
                 accepted reached exactly one backend (none lost, none doubled) across 300 switches and
                 closes, nothing visible after close_track(), the last track loads and renders.
ext_finish_race  (SRT and VTT) A reopen landing exactly as a feed job finishes. The few-instruction window is
                 opened on purpose (a hooked pthread_mutex_unlock sleeps on the worker), so the test is
                 deterministic; it also checks the window really was opened every round.
style_resize_paused  Style changes and surface resizes while paused: each renders once at the frozen
                 clock, geometry reaches the backend as a coalesced pair ending on the last
                 request, style + resize storms on two threads, pause state untouched.

Scope: the ext_* tests run the REAL external-subtitle stack -- stream_sub_ext.c (parse-worker pool, cue
cache, SUBT_PARSE_* state machine, generation stamping), stream_subtitle.c (the subtitle decode thread,
_get_next_ext_sub(), stream_set_subtitle_stream()), subtitle_formats.c, the SRT/VTT parsers and the real
engine -- over real .srt/.vtt files, on a fake STREAM (stream_host.c). Only the engine's backend is fake.
They are the regression tests for aos-AVP issue #2004 (an external track is selected but renders nothing).

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
import platform
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

# The real external-subtitle stack, driven through a fake STREAM (stream_host.c). stream_sub_ext.c and
# stream_subtitle.c are the code under test; the rest is the real code they call, borrowed (-w).
STREAM_EXT_SRC = 'Source/stream_sub_ext.c'
STREAM_EXT = LIFECYCLE + (STREAM_EXT_SRC, 'Source/stream_subtitle.c', 'Source/subtitle_formats.c',
                          'Source/subtitle_srt.c', 'Source/subtitle_vtt.c', 'Source/athread.c', 'Source/util.c',
                          'Source/browse.c', 'Source/file.c', 'Source/i18n.c', 'Source/iso639.c',
                          'Source/subtitle_idx.c', 'Source/subtitle_pgs.c', 'Source/stream_alloc.c',
                          'Source/atime.c', HOST + 'stream_host.c')
QUIET |= {'Source/subtitle_formats.c', 'Source/subtitle_srt.c', 'Source/subtitle_vtt.c', 'Source/athread.c',
          'Source/util.c', 'Source/browse.c', 'Source/file.c', 'Source/i18n.c', 'Source/iso639.c',
          'Source/subtitle_idx.c', 'Source/subtitle_pgs.c', 'Source/stream_alloc.c', 'Source/atime.c'}
STREAM_FLAGS = ('-Wno-pointer-sign', '-Wno-unused-parameter', '-Wno-sign-compare',
                '-D_GNU_SOURCE', '-DCONFIG_STREAM', '-DCONFIG_SUBTITLES')
# ext_finish_race opens a few-instruction race window by sleeping in a hooked pthread_mutex_unlock. The hook is a
# compile-time rename (every object of the test calls host_unlock_hook(), which the test defines and which calls the
# real function), not a linker --wrap: Apple's ld has no --wrap.
HOOK_UNLOCK = ('-Dpthread_mutex_unlock=host_unlock_hook',)

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


# (file, family as an ASS style must spell it). Only fonts that ship with the OS or with the
# near-universal font packages; the first one present wins.
FONT_CANDIDATES = (
    ('/System/Library/Fonts/Supplemental/Arial.ttf', 'Arial'),                       # macOS
    ('/Library/Fonts/Arial.ttf', 'Arial'),
    ('/System/Library/Fonts/Supplemental/Verdana.ttf', 'Verdana'),
    ('/System/Library/Fonts/Supplemental/Tahoma.ttf', 'Tahoma'),
    ('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 'DejaVu Sans'),              # Debian / Ubuntu
    ('/usr/share/fonts/TTF/DejaVuSans.ttf', 'DejaVu Sans'),                          # Arch
    ('/usr/share/fonts/dejavu/DejaVuSans.ttf', 'DejaVu Sans'),                       # Fedora / SUSE
    ('/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf', 'Liberation Sans'),
    ('/usr/share/fonts/liberation/LiberationSans-Regular.ttf', 'Liberation Sans'),
    ('/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf', 'Liberation Sans'),
    ('/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf', 'Noto Sans'),
    ('/usr/share/fonts/noto/NotoSans-Regular.ttf', 'Noto Sans'),
)
FONT_HELP = ('no usable font found for the libass tests. Install one (Linux: fonts-dejavu-core or '
             'fonts-liberation; macOS ships Arial) or point at any TrueType/OpenType file with '
             'HOST_TEST_FONT=/path/font.ttf HOST_TEST_FONT_FAMILY="Family Name".')


def find_test_font():
    """(file, family) of a real font on this machine. Raises RuntimeError with instructions if none."""
    path, family = os.environ.get('HOST_TEST_FONT'), os.environ.get('HOST_TEST_FONT_FAMILY')
    if path:
        if not family:
            raise RuntimeError('HOST_TEST_FONT is set but HOST_TEST_FONT_FAMILY is not: libass styles need '
                               'the family name (for example "Liberation Sans").')
        if not Path(path).is_file():
            raise RuntimeError('HOST_TEST_FONT=%s is not a file' % path)
        return path, family
    for cand, fam in FONT_CANDIDATES:
        if Path(cand).is_file():
            return cand, fam
    fc_match = shutil.which('fc-match')                  # any Linux with fontconfig, wherever it keeps fonts
    if fc_match:
        out = subprocess.run([fc_match, '-f', '%{file}|%{family[0]}', 'sans-serif'],
                             capture_output=True, text=True).stdout
        cand, _, fam = out.partition('|')
        if fam and cand.lower().endswith(('.ttf', '.otf')) and Path(cand).is_file():
            return cand, fam
    raise RuntimeError(FONT_HELP)


_FONT_ENV = None


def _font_env():
    """HOST_TEST_FONT / _FAMILY / _DIR for the libass tests; the DIR holds a copy of just that font,
    for the backend's fonts-folder path. Raises RuntimeError if no font can be found."""
    global _FONT_ENV
    if _FONT_ENV is None:
        path, family = find_test_font()
        fonts = _BUILD / 'font'
        fonts.mkdir(exist_ok=True)
        shutil.copyfile(path, fonts / Path(path).name)   # content only: copy2 copies macOS file flags and fails with EPERM
        _FONT_ENV = {'HOST_TEST_FONT': path, 'HOST_TEST_FONT_FAMILY': family, 'HOST_TEST_FONT_DIR': str(fonts)}
    return _FONT_ENV


def _run_env(libass):
    env = dict(os.environ)
    if libass:
        env.update(_font_env())
    return env


_CHECK_LINE = re.compile(r'^\s*(ok|FAIL)\b')


def setUpModule():
    global _BUILD
    _BUILD = Path(tempfile.mkdtemp(prefix='avos-host.'))


def tearDownModule():
    shutil.rmtree(_BUILD, ignore_errors=True)


_BASELINES = {}


def _baseline_tree(rev):
    """Extract Source/ and Include/ of git revision `rev` into the build dir (once); returns its root."""
    if rev in _BASELINES:
        return _BASELINES[rev]
    try:
        sha = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--verify', rev + '^{commit}'],
                             capture_output=True, text=True, check=True).stdout.strip()
        dest = _BUILD / ('baseline-' + sha[:12])
        dest.mkdir()
        tar = subprocess.run(['git', '-C', str(REPO), 'archive', '--format=tar', sha, 'Source', 'Include'],
                             capture_output=True, check=True).stdout
        subprocess.run(['tar', '-x', '-C', str(dest)], input=tar, check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        raise RuntimeError('HOST_BASELINE_REV=%s: cannot extract it from git (%s)' % (rev, getattr(e, 'stderr', e))) from e
    _BASELINES[rev] = dest
    return dest


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
        rev = os.environ.get('HOST_BASELINE_REV') if STREAM_EXT_SRC in sources else None
        try:
            root = _baseline_tree(rev) if rev else REPO
        except RuntimeError as e:
            self.fail(str(e))
        includes = ['-I', str(REPO / 'Include' / 'host_compat'), '-I', str(root / 'Include')]
        checked = tuple([*base, '-Wall', '-Wextra', '-Wno-deprecated-declarations',
                         *sanitize, *flags, *includes, *pkg_cflags])
        quiet = tuple([*base, '-w', *[f for f in flags if f.startswith('-D')], *includes])

        jobs = [(HERE / main, checked)]
        for rel in sources:
            src = (root if rel.startswith('Source/') else REPO) / rel      # HOST_BASELINE_REV: code under test from one revision
            jobs.append((src, quiet if rel in QUIET else checked))
        for src, _ in jobs:
            if not src.is_file():
                self.fail('missing source file: %s' % src)

        objs, errors = _build_objects(cc, jobs)
        if errors:
            self.fail('compile failed:\n' + '\n'.join(errors))

        exe = _BUILD / (Path(main).stem + '.bin')
        link = subprocess.run([*cc, *sanitize, *map(str, objs), *pkg_libs, '-lpthread', '-lm', '-o', str(exe)],
                              capture_output=True, text=True)
        if link.returncode:
            self.fail('link failed:\n' + link.stdout + link.stderr)

        try:
            run_env = _run_env('libass' in pkgs)
        except RuntimeError as e:
            self.fail(str(e))
        info = _INFO[self._testMethodName] = RunInfo()
        started = time.monotonic()
        try:
            run = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=RUN_TIMEOUT_S,
                                 env=run_env)
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

    def _ext(self, main, fmt, flags=()):
        self.build_and_run(main, STREAM_EXT, flags=(*ENGINE_FLAGS, *STREAM_FLAGS, *flags), args=(fmt,))

    def test_ext_reselect_srt(self):
        """#2004: every selection of an external SRT track ends with its cues in the engine (real stack)."""
        self._ext('ext_reselect_test.c', 'srt')

    def test_ext_reselect_vtt(self):
        """#2004: every selection of an external VTT track ends with its cues in the engine (real stack)."""
        self._ext('ext_reselect_test.c', 'vtt')

    def test_ext_interrupt_srt(self):
        """#2004: pause/seek mid-feed leaves neither an empty nor a truncated SRT track (real stack)."""
        self._ext('ext_interrupt_test.c', 'srt')

    def test_ext_interrupt_vtt(self):
        """#2004: pause/seek mid-feed leaves neither an empty nor a truncated VTT track (real stack)."""
        self._ext('ext_interrupt_test.c', 'vtt')

    def test_ext_close_open_srt(self):
        """Close + immediately open a video while its real SRT parse worker runs, engine rebuilt underneath."""
        self._ext('ext_close_open_test.c', 'srt')

    def test_ext_close_open_vtt(self):
        """Close + immediately open a video while its real VTT parse worker runs, engine rebuilt underneath."""
        self._ext('ext_close_open_test.c', 'vtt')

    def test_gen_token(self):
        """Engine track-generation token contract: stale cues dropped, accepted == delivered (engine-level)."""
        self.build_and_run('gen_token_test.c', LIFECYCLE, flags=ENGINE_FLAGS)

    def test_ext_finish_race_srt(self):
        """#2004: a reopen landing as the SRT feed job finishes still gets the track fed (real stack)."""
        self._ext('ext_finish_race_test.c', 'srt', flags=HOOK_UNLOCK)

    def test_ext_finish_race_vtt(self):
        """#2004: a reopen landing as the VTT feed job finishes still gets the track fed (real stack)."""
        self._ext('ext_finish_race_test.c', 'vtt', flags=HOOK_UNLOCK)

    def test_style_resize_paused(self):
        """Style change and surface resize while paused."""
        self.build_and_run('style_resize_paused_test.c', LIFECYCLE, flags=ENGINE_FLAGS)

    def test_smi_parse_error(self):
        """Empty SMI bodies close the file and free the record without touching it after free."""
        self.build_and_run('smi_parse_error_test.c', (HOST + 'stubs.c',),
                           flags=('-Wno-sign-compare', '-Wno-unused-parameter', '-Wno-pointer-sign',
                                  '-Wno-unused-but-set-variable', '-Wno-missing-field-initializers'))


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


def _font_description():
    try:
        path, family = find_test_font()
        return '%s (%s)' % (family, path)
    except RuntimeError:
        return 'NONE FOUND -- the libass tests will fail (see HOST_TEST_FONT)'


def _environment_line():
    cc = shlex.split(os.environ.get('CC') or 'cc')
    parts = ['platform: %s %s' % (platform.system(), platform.machine()),
             'compiler: %s' % (_tool_version([*cc, '--version']) or ' '.join(cc) + ' (version unknown)'),
             'libass: %s' % (_tool_version(['pkg-config', '--modversion', 'libass']) or 'not found'),
             'sanitizers: %s' % (os.environ.get('SANITIZERS') or 'undefined'),
             'font: %s' % _font_description()]
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
