# Frame-identity tests

Four tests for the subtitle-engine frame-identity/3D-push work, in the same style as
`run-ssa-geometry.sh` / `run-ssa-style-cache.sh` / `run-color-conversion.sh`: a standalone
`run-*.sh` script per test that finds this repo, links the real engine sources, builds to a
temp dir, and runs the result. No NDK, no device, no Makefile changes.

```
./run-race-harness.sh
./run-fill-contract.sh
./run-push-policy.sh
./run-jni-listener.sh
```

`SANITIZERS` (default `undefined`, or `none` to disable) and `CC` work exactly as in the
other `run-*.sh` scripts, e.g. `SANITIZERS=thread ./run-race-harness.sh` or
`SANITIZERS=address,undefined ./run-jni-listener.sh`.

The four scripts share `frame-identity-lib.sh` (sourced, not a new convention -- see its own
header for why this family of tests factors that part out where the three pre-existing
scripts don't). It follows `run-ssa-style-cache.sh`'s own real-vs-stub preference: real
`Source/sub_style.c` and `Source/debug.c` whenever present (override with `SUB_STYLE_C`/
`DEBUG_C`/`LOG_C`), `test_stubs.c`'s stand-ins otherwise. `test_stubs.c` gained one addition
for this: a `SSA_TEST_STUB_SKIP_FRAME_UNREF` guard, needed because these tests link
`sub_engine.c` itself, which defines `sub_frame_unref()` -- see the comment on that guard.

`android_stub/` gained five headers (`android/native_window_jni.h`, `android/bitmap.h`,
`android/log.h`, `EGL/egl.h`, `GLES2/gl2.h`, `jni.h`) plus their link-time bodies
(`android_stub.c`), alongside the existing `android/native_window.h`. Each header says what
it stands in for and, importantly, what it does *not* exercise -- most of the render thread's
real GL blend path is link-only here, since it never gets a real EGL surface in these tests
(same as production 3D mode with no window attached).

`fake_backend.c`/`.h` stand in for `sub_format_ssa/srt/gfx_create()` with a backend a test
can drive on demand (emit a frame now / stay slow), which the real decoders don't offer
control over. See `fake_backend.h`'s header for why that's the right trade-off here.

## What each test covers

- **race_harness_test** — hammers open/close against a deliberately slow backend and checks
  that nothing is ever visible once `close_track()` has returned. This is the concrete bug:
  before poll+publish became one engine-lock hold, a frame from a just-closed track could
  still land on screen. Reverting that fix turns `stale-frame-after-close=0` into thousands
  of hits.
- **fill_contract_test** — the renderer's UNCHANGED / CLEAR / FRAME / ERROR contract and
  exactly when `frame_generation` moves.
- **push_policy_test** — when the native render thread announces "content changed" for the
  3D path: only while paused, only in 3D, once per generation, and never with a lock held.
- **jni_listener_lifecycle_test** — the same policy through the real JNI file, against a mock
  JavaVM: one global ref per engine, attach-once/detach-at-exit, no call on a freed ref or a
  detached thread, and 200 create/announce/destroy races with nothing leaked.

## What this does *not* prove

The mock JavaVM in `jni_listener_lifecycle_test.c` validates our use of the JNI contract, not
a real ART, and can't stand in for a device run. Please still smoke-test on-device: pause in
3D, switch (or disable) the subtitle track, confirm the old cue clears and the new one
appears without resuming.
