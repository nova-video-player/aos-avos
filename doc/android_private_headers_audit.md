# Android private-header compatibility audit

Date: 2026-10-30. AVOS baseline: `1329243`, “Reset native-window rotation
explicitly and report transform errors”, in `nova-libass/native/avos`.
This is a source and compiler-layout audit, not device validation.

## Recommendation

Keep the six vendored headers unchanged. Raising the application's minimum API
from 21 to 23 does not require replacing this private-header snapshot. No change
was found in the layouts, callback signatures or constants that the current code
uses across the selected API 23–37 releases. There are concrete consumer-side
error-handling gaps to address separately; a bulk AOSP update would not fix them.

A newer header can change behavior even when its structures remain compatible:
`native_window_set_usage()` starts issuing a different operation in the API-27
snapshot. That is a reason to review changes individually, not synchronize the
private-header directory mechanically with `minSdkVersion`.

## Evidence and coverage

The separate [reference archive](../external/android/header-reference/README.md)
contains all headers within explicitly selected AOSP families, not just the six
retained headers. Its [index](../external/android/header-reference/snapshots/INDEX.json)
records the scope and missing project tags. Per-project manifests pin every
snapshot to its commit and checksum every captured file. Downloaded contents
and manifests remain local and ignored by Git. Only tools and written audit
documentation are versioned. A fresh checkout downloads the configured release
tags again; it does not preserve this audit's per-file checksums. Existing local
manifests support restoration from the original pinned commits. Links to
generated evidence below refer to local files, absent from a fresh checkout.

| API | Selected tag |
|---|---|
| 23 | android-6.0.0_r1 |
| 24 | android-7.0.0_r1 |
| 25 | android-7.1.0_r1 |
| 26 | android-8.0.0_r1 |
| 27 | android-8.1.0_r1 |
| 28 | android-9.0.0_r1 |
| 29 | android-10.0.0_r1 |
| 30 | android-11.0.0_r1 |
| 31 | android-12.0.0_r1 |
| 32 | android-12.1.0_r1 |
| 33 | android-13.0.0_r1 |
| 34 | android-14.0.0_r1 |
| 35 | android-15.0.0_r1 |
| 36 | android-16.0.0_r1 |
| 37 | android-17.0.0_r1 |

The detailed audit follows the two actual private-header consumers:
`external/android/libavos_android/android_buffer.c` and `android_window.c`.
The wider archive is retained for future audits; its other APIs have not received
an exhaustive semantic or security review. This is not a review of every release
between the selected tags, vendor forks, or Android's implementation history.

## ABI and constant results

The [layout report](../external/android/header-reference/layouts/layouts.json)
contains 64 successful compiler measurements: the vendored baseline and 15 AOSP
snapshots, each for armeabi-v7a, arm64-v8a, x86 and x86_64. NDK 30 clang compiles
extracted original C declarations for each target using the macOS NDK host
toolchain; the runner currently hardcodes that host path. Assertions also check the
callback signatures used by AVOS. The full platform headers are not substituted
into the production build or compiled as a complete AOSP sysroot.

No differences were found in the measured structure sizes and used field
offsets. Representative offsets, identical across the selected snapshots:

| Field | 32-bit | 64-bit |
|---|---:|---:|
| ANativeWindow.common | 0 | 0 |
| ANativeWindow.dequeueBuffer_DEPRECATED | 72 | 120 |
| ANativeWindow.query | 84 | 144 |
| ANativeWindow.perform | 88 | 152 |
| ANativeWindow.cancelBuffer_DEPRECATED | 92 | 160 |
| ANativeWindowBuffer.handle | 60 | 96 |
| gralloc_module_t.lock | 136 | 264 |
| gralloc_module_t.unlock | 140 | 272 |

The [constant report](../external/android/header-reference/constants.json)
compares 28 used values: window queries/operations, API/scaling selectors,
standard pixel formats, transforms and gralloc usage flags. All match the
vendored baseline. Vendor-specific formats in `Include/android_config.h` are
not given compatibility guarantees by this comparison.

| Retained header | Result |
|---|---|
| `hardware.h` | Used module prefix remains compatible; module version still must be checked by the consumer. |
| `gralloc.h` | gralloc0 lock/unlock signatures and offsets remain compatible; newer asynchronous callbacks use reserved space. This says nothing about treating a gralloc1 module as gralloc0. |
| `fb.h` | Transitive include only; AVOS does not call framebuffer HAL methods here. Byte-identical to the API-23 copy. |
| `window.h` | Used window fields/callbacks remain compatible; later definitions move between directories/headers. |
| `graphics.h` | Used standard format and transform values match; later versions split definitions into additional headers. |
| `native_handle.h` | Base layout remains compatible. AVOS passes buffer handles rather than implementing the later native-handle management APIs. |

Against API 23 specifically, `hardware.h`, `fb.h` and `native_handle.h` are
byte-identical to the vendored copies. `window.h`, `gralloc.h` and `graphics.h`
differ, but not in the measured interfaces AVOS uses. The original AVOS dump's
exact upstream tag is not established; `21/` is not proof that it matches an
unmodified `android-5.0.0_r1` tree.

## Meaningful evolution without a current layout break

### Usage setter semantics change

The [helper report](../external/android/header-reference/helpers.json) compares
all nine private inline helpers called by AVOS. Only `native_window_set_usage`
differs after whitespace/comments are removed:

- Vendored baseline through the API-26 snapshot: `int usage`, operation
  `NATIVE_WINDOW_SET_USAGE` (0).
- API-27 through API-37 snapshots: `uint64_t usage`, operation
  `NATIVE_WINDOW_SET_USAGE64` (30).

[API-37 Surface.cpp](../external/android/header-reference/evidence/api-37/libs/gui/Surface.cpp)
still handles both operations with their respective argument widths. The current
AVOS call therefore does not become invalid merely because the OS is newer.
Conversely, copying the new inline helper into a binary intended for API 23 would
make it send an operation absent from that release's interface. AVOS currently
uses the low-width flags listed in the constant report; supporting new 64-bit
usage flags would require an explicit implementation change.

### Native buffer evolution preserves the used slots

Newer `ANativeWindowBuffer` definitions rename the old usage field to
`usage_deprecated`, add layer count, and place 64-bit usage in reserved space.
The width/height/stride/format/handle offsets AVOS reads remain unchanged on all
four targets. AVOS does not read the old `anb->usage` field. A wholesale header
replacement is not needed to read the fields it actually consumes.

### Deprecated callbacks still exist, but are private dependencies

The selected API-37 Surface implementation still installs deprecated
queue/dequeue/cancel/lock hooks. Its deprecated dequeue wrapper performs the
fence wait; the lock compatibility hook is a no-op. The present code does not
simply omit fence waiting because it calls a deprecated entry point.

This is evidence about the inspected AOSP implementation, not a vendor guarantee.
Likewise, resolving `hw_get_module` varies with platform visibility and preload
behavior. A failed lookup already selects the public-window fallback. Neither
header age nor the OS API number alone establishes whether gralloc is reachable.

Android explicitly separates app-facing NDK stability from private platform
interfaces: [NDK/API guidelines](https://android.googlesource.com/platform/developers/docs/+/master/api-guidelines/ndk.md),
[native-library restrictions](https://developer.android.com/about/versions/nougat/android-7.0-changes#ndk).

## Concrete follow-ups found in the consumer code

These are pre-existing findings, not regressions introduced by the recent header
pruning. The original audit changed no production code; the P1 guard below
has since been implemented. The two P2 findings remain pending.

All three findings apply only when the private gralloc branch is selected; they
do not affect the public-window branch. Gralloc was observed on API 24/25
emulators, while the inspected API 26+ images used the public path. This is not
an API cutoff: native selection tests module availability, not Android version.
Physical API 23-25 devices are a validation priority, not the exclusive scope.

The planned gralloc removal would eliminate these paths, but remains gated on
compatibility and performance checks. If that migration is validated and is the
next change, defer the two P2 fixes. The P1 guard now provides interim protection.
If gralloc remains supported while migration is deferred, address both remaining P2s.
Do not remove the branch merely to close these findings.

### P1, conditional: validate gralloc kind before casting (implemented)

At the audit baseline, `android_surface_create()` accepted any successful
`hw_get_module("gralloc", ...)`
result and cast it to `gralloc_module_t`, without checking the module's major
version or required lock/unlock callbacks. `android_surface_check_gralloc()` then
selected a gralloc sink merely because this pointer was non-NULL.

The archived gralloc1 API has a distinct device/getFunction interface. AOSP's
[GrallocLoader](../external/android/header-reference/snapshots/api-37_android-17.0.0_r1/hardware/interfaces/graphics/mapper/2.0/utils/passthrough/include/mapper-passthrough/2.0/GrallocLoader.h)
selects different adapters for module major versions 0 and 1 and rejects unknown
versions. Reading gralloc0 callback slots from another module shape can call
invalid pointers. No affected device has been demonstrated here; the trigger is
conditional on that incompatible module being visible to the app.

Implemented: initialize the returned pointer to NULL and check the common
module tag, gralloc ID and major version before accessing gralloc0 callbacks.
Accept legacy major version 0 with both synchronous lock/unlock callbacks;
retain all 0.x minor versions. Invalid, missing or unsupported modules keep the
existing public-window fallback. This does not add gralloc1 support or impose
an Android API cutoff.

Host replay coverage includes a common header followed by an inaccessible page,
so reading gralloc0 callbacks before rejecting a newer major version faults the
test. It also covers missing symbols/modules/callbacks, malformed headers and
legacy minor versions. Device compatibility and performance remain to be checked.

### P2: query errors are ignored during buffer setup

`android_buffer_setup()` calls the `NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS` query
without assigning its return value to `err`. The following `CHECK_ERR()` tests
the previous operation's status. A failed query can therefore feed an unverified
count into buffer negotiation. The diagnostic `NATIVE_WINDOW_CONCRETE_TYPE` query
also ignores failure and may print an uninitialized local value.

Recommended fix: check the functional query result before using it, and initialize
or conditionally log the diagnostic result. No header update is required.

### P2: gralloc acquisitions are not unwound on later lock failure

In the gralloc branches of `android_buffer_alloc()`, `android_buffer_dequeue()`
and `android_buffer_dequeue_with_buffer()`, a successful dequeue can be followed
by a failed legacy lock or gralloc CPU lock. `CHECK_ERR()` returns immediately
without cancelling the acquired native-window buffer. It is then absent from
normal queue ownership and can exhaust available buffers if failures repeat.
The public-window ownership fix does not cover this separate branch.

Recommended fix: unwind according to acquired state. Cancel a dequeued buffer
on subsequent failure, unlock only after a successful CPU lock, and publish output
handles only once acquisition succeeds. Verify failure paths with replay tests;
keep successful gralloc and public-window behavior unchanged.

## Next action

Keep the current six headers. Choose the gralloc follow-up order according to
the gated migration decision above, with any fixes in focused changes. Public rotation/gralloc migration remains separately gated on
device compatibility and performance checks. Do not run a new AOSP bulk dump over
`external/android/include/21`.
