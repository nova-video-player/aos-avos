# Sync transition invariants and refactoring boundaries

This inventory describes the current native sync and `sfdec2` timed-release
paths. It records the inspected contracts and their host replay coverage; it is
not an exhaustive proof of reachable states or Android thread scheduling.
Device validation of the recent speed fixes and slew extraction remains pending.

## Independent state dimensions

A single `STARTING -> SEEKING -> RESUMING -> STEADY` enum cannot describe these
paths. The relevant dimensions overlap:

| Dimension | State and ownership | Required distinction |
| --- | --- | --- |
| Output identity | `audio_lifecycle_generation`, `audio_reconfiguring`; checked under `audio_lifecycle_mutex` | A flush/seek need not replace the output, but recreation invalidates evidence from its predecessor. |
| Media epoch | `seek_epoch`, `pending_seek_reanchor`, seek admission fields | An old absolute renderer target cannot cross a seek. |
| Timestamp mapping | Timeline speed, RST anchor and TS anchor | Equal speeds alone do not establish equal mappings. |
| Pause | `paused`, `pause_armed`, `pause_start_ms` | A correction may remain owned while stepping is frozen. `seek_paused` denotes seek quiescence, not simply the user's paused state. |
| Clock qualification | Resume sampling, calibration, startup trust and Mode 2 readiness | Accepted writes, progressing presentation and trusted clock phase are different evidence. |
| Correction | Offset/target, slew flags and distinct-frame identities | Target eligibility, per-frame stepping and completion are separate decisions. |

The pause and seek public hooks acquire `video_sink_mutex`; their renderer
updates then acquire `p->locked.mtx`. Phase-matching helpers check audio output
identity under `audio_lifecycle_mutex`. Mode 2 frontier arm/reset/consume operations
use `mode2_heard_mutex`. These are existing ownership boundaries, not a proposed
new lock or a complete lock-order audit.

## Combined transitions

| Combination | Current contract | Implementation / host evidence |
| --- | --- | --- |
| Ordinary pause during cold-start PCM correction | Cancel the cold-start correction and partial observation window. It is not retained resume evidence. | `sfdec2_android_sync_on_pause_locked`; `test_slew_offset.py`. |
| Ordinary pause during retained PCM resume correction | Preserve the measured target only for an output-preserving backend, matching audio generation, seek epoch, manual delay and complete timestamp mapping. Freeze stepping while paused. | `pcm_resume_matches_audio_l`, pause hook; `test_slew_offset.py`, `test_pause_resume_policy.py`. |
| Ordinary pause during Mode 1 correction | Preserve eligible relative phase and its correction only for retained output and matching generation/manual delay; seek/startup guards still apply. | `mode1_phase_matches_audio_l`, pause hook; `test_slew_offset.py`. |
| Resume with an owned correction | Apply the mode-specific boundary correction, shift both offset and target by paused wall time, and carry the residual into distinct-frame stepping. Shift the drift reference only for the paused interval it overlaps. | Pause hook; `test_slew_offset.py`. |
| Seek while paused or correcting | Invalidate the absolute offset/target and active correction; set seek reanchor pending. Rebase an armed pause start so resume cannot add pre-seek wall time again. | `sfdec2_reset_sync_state_on_seek_locked`; `test_slew_offset.py`. |
| Mode 1 seek with valid output phase | A generation-owned relative phase can survive seek quiescence; the old absolute target and resume slew cannot. `mode1_seek_pending` records that distinction. | Seek hook and phase matcher; `test_slew_offset.py`. |
| Speed or anchor change during PCM resume sampling/calibration | A mismatching mapping invalidates the old observation/target eligibility. Calibration and partial sample windows are explicitly cancelled on speed/epoch changes. | `pcm_resume_matches_audio_l`, `videosink_put_time`; `test_slew_offset.py`, `test_video_pcm_calibration.py`. |
| Speed commit while resume notification is pending | A speed notification alone is not an accepted post-resume audio write. PCM resume eligibility requires `audio_resume_write_committed` and valid running audio. | `videosink_put_time`; the whole concurrent publication sequence still requires device coverage. |
| Speed change during startup clock qualification | Re-arm the startup comparison for the new speed epoch rather than accepting the old epoch's trust history. | `stream_sync.c` startup correction decision; `test_pcm_startup_correction.py`. |
| Output replacement or reconfiguration during pause | Reject the old PCM target or Mode 1 phase even if the media speed is unchanged. | Phase matchers and pause hook; `test_slew_offset.py`. |
| Repeated Mode 2 resets during seek | Carry the explicit empty-output frontier reason through preserving resets until it is consumed. A seek reason opens bounded refill protection; recreation keeps its existing catch-up behavior. | Mode 2 frontier helpers and heard-clock interpolation; `test_mode2_seek_startup.py`. |
| Preview output before seek admission | Mode 2 preview output cannot establish the post-seek audio/render anchor; admission and first valid publication determine it. | `test_mode2_seek_startup.py`. |
| Renderer retries the same queued frame | Do not spend another bounded slew step; a new time, epoch or buffer identity permits progress. Completion and target deadbands remain distinct. | `videosink_thread`; `test_slew_offset.py`. |

`STREAM_MODE2_FRONTIER_NONE=0`, `RECREATE=1` and `SEEK=2` preserve the old
stored values. `stream_sync_mode2_heard_frontier_pending()` retains its integer
return, including the reason value; callers using it as a truth value continue
to distinguish NONE from either pending reason. Arm callers now supply the reason
directly instead of translating a seek boolean into 1 or 2. This reason enum is
not a playback lifecycle enum.

## Resume helper comparison: keep separate

The boundary correction is deliberately different from normal per-frame slew:

| Rule | PCM retained resume | Mode 1 retained resume |
| --- | --- | --- |
| Boundary limit | Two video frames scaled by `inv_speed`, at most 100ms | Two unscaled video frames, at most 100ms |
| Negative correction | Also capped by elapsed paused duration | Symmetric negative/positive limit |
| Remaining correction | Sets `pcm_resume_slew` and `pcm_startup_slew` together | Sets `mode1_resume_slew` |
| Ownership | Output generation, seek epoch, full mapping and manual delay | Output generation and manual delay, plus mode-specific seek/startup guards |

For a 40ms frame, a 5ms pause and a negative residual exceeding the limit,
PCM applies at most -5ms at the boundary; Mode 1 can apply -80ms. At 1.6x,
PCM's two-frame limit becomes 50ms. These are characterization examples of
existing policy, not new tuning recommendations. The replay covers both signs,
short/long pauses, PCM speeds 0.5x/1x/1.6x, and residual ownership invalidation.

Both paths clamp and add an offset, but sharing that expression would require
passing different bounds and updating different ownership flags. It provides
little simplification and risks hiding the reason for the different limits.
Keep the lifecycle paths separate. `offset_slew_step()` already shares the
useful per-frame arithmetic without absorbing these boundary decisions.

## Limits on further restructuring

PCM and passthrough are mutually exclusive output modes for a live sink; that
does not make their retained flags a complete closed state machine. In
particular, `pcm_resume_slew` uses `pcm_startup_slew` as its stepping mechanism,
calibration can remain armed after a slew completes, and a paused Mode 1 seek can
retain relative phase while discarding its absolute renderer anchor. Mode 2
fast recovery is a policy of its dynamic slew, not an independent playback mode.
Do not assert that all flags are mutually exclusive.

This inventory is sufficient to explain why the two resume-boundary helpers
should stay separate. It is not sufficient to replace all evidence predicates
or lifecycle flags: first trace their producers and consumers through open,
failed-open cleanup, stop/reopen and concurrent output replacement, then cover
those interleavings. Host replays here mock time and backend behavior and do not
prove thread-order correctness.

Centralizing master-clock/sync-mode selection is a possible later structural
review, subject to those same ownership constraints. It is not scheduled by this
change. Continuous tempo nudges or a new drift estimator change playback policy
and remain separate deferred work. Refresh matching is already documented in
`android_frame_timing.md`.
