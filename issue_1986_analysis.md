# Issue #1986 — Analysis: player freezes when seeking past the end of the video

- Issue: https://github.com/nova-video-player/aos-AVP/issues/1986
- Reporter fix (app layer): https://github.com/nova-video-player/aos-Video/pull/216

## Symptom

In Binge Play mode, seeking forward past the end of the current video freezes the
player. Instead of playing the next episode, the seek never completes and playback
stalls. Reported on Nova 6.5.3, Android 14, Xiaomi TV box (gen 2). The reporter also
notes the problem does **not** occur on the Google Play 6.4.72 build and appears
starting with the 6.5.x line, and supplied PR aos-Video#216 ("Fix for issue #1986
about video freeze when seek past video duration", branch `patch-1` → `v6.4-lint`,
+54/-0 in `Player.java`).

## Root cause

The freeze is a **native engine (avos) regression introduced in the 6.5.x line**, not
an app bug. `Player.java` cannot prevent it; PR #216 only recognises the end case and
forces completion on the app side.

### 1. A seek at/after the end arms an unsatisfiable drop target

Duration-known files (MP4/MKV, i.e. almost everything) route a seek through the
time-based path, never the byte/pos path:

- `avos_mp_video_seek` → `stream_seek_pos` → `stream_seek_time`
  (`Source/avos_mp_video.c:408`, `Source/stream_video.c:5430`, `Source/stream_video.c:5368`).

`stream_seek_time` only clamps negative targets; a target at or beyond the duration is
passed through unchanged. The parser then arms a video drop target at that position
(`_output_frame_no_resize`, `Source/stream_video.c:3330`): every frame with
`frame->time < seek_video_target_ts` is discarded, and the "target reached" flag
`seek_video_target_pending` is only cleared once a frame at/beyond the target is seen.
When the target is past the last frame, that never happens:

```c
// Source/stream_video.c, _output_frame_no_resize
if( s->seek_video_drop && s->seek_video_target_ts > 0 ) {
    if( frame->time < s->seek_video_target_ts ) goto Discard;  // all trailing frames
    ...
    if( s->seek_video_target_pending ) {
        s->seek_video_target_ts = frame->time;
        s->seek_video_target_pending = 0;   // never reached when target > last frame
    }
}
```

### 2. The 6.5.x engine waits for a stricter "drained" condition before completion

Commit native/avos `1c2c002` "Fix video frame retention, EOF draining and decoder
recovery" (2025-09-18, present only on the 6.5.x `v6.4-lint` engine, not on the
6.4.72 `v6.4-stable` engine) replaced the previous "end of file" test with
`_video_presentation_drained()`:

```c
// Source/stream_video.c:3947
static int _video_presentation_drained(STREAM *s)
{
    if (!s->video_end || s->cdata_now.valid || s->cdata_next.valid)
        return 0;
    if (s->video_dec && s->video_dec->drain && !s->video_decoder_drained)
        return 0;
    if (frame_q_count(&s->disp_q) || s->video_sink_count > 0)
        return 0;
    ...sink drained...
}
```

- **6.4.72** declared end as soon as the parser reached EOF with no buffered chunk:
  `s->video_end && !s->cdata_now.valid && !s->cdata_next.valid` (v6.4-stable
  `Source/stream_video.c:3900`). A beyond-end seek therefore still completed, and the
  app advanced to the next episode.
- **6.5.x** additionally requires the decoder drain and the presentation/queue
  bookkeeping to settle. That bookkeeping never settles when the trailing frames were
  discarded by the unsatisfiable drop target, so `_check_end` never reaches the
  end-of-stream state, `stream_stop_handler` never emits `MEDIA_PLAYBACK_COMPLETE`
  (`Source/avos_mp_video.c:211`), and the app sits in `mIsBusy` waiting for
  `onAllSeekComplete` → freeze.

### Why PR #216 "works" but is not the fix

PR #216 (`Video/src/main/java/com/archos/mediacenter/video/player/Player.java`)
adds a watchdog that resets `mIsBusy` and a check that, when
`msec >= duration - endMargin`, forces `onSeekComplete`/`onAllSeekComplete` and
`onCompletion`. That re-implements the old 6.4.72 "reached the end ⇒ complete" rule in
the app. It only covers Nova's own `seekTo` caller; any other avos consumer (MediaLib,
scrub/thumbnail paths, the AVOS player API) still hits the same engine state.

## Fix (native engine)

Three coordinated, low-risk changes. #1 is the primary fix; #2 and #3 are defensive
and cover other entry points (frame-accurate/scrub seeks, byte seeks).

### #1 — Clamp at/after-end seeks inside the content (`stream_seek_time`)

`Source/stream_video.c` (new constant `SEEK_END_MARGIN_MS`, default 500):

```c
if( s && s->duration > 0 && time >= s->duration ) {
    int target  = time;
    int margin  = SEEK_END_MARGIN_MS;
    if( margin > s->duration )
        margin = s->duration / 2;
    time = s->duration - margin;
    DBG serprintf( "SEEK_CLAMP_END: target=%d duration=%d clamped=%d\n", target, s->duration, time );
}
```

A seek to the end now lands just inside the content, so a real frame satisfies the
drop target, playback reaches a genuine EOF, and the 6.5.x completion gate settles
normally. This removes the bad state at the source and also removes any dependence on
`avformat_seek_file`'s beyond-EOF behaviour.

### #2 — Symmetric beyond-end guard in the parser time branch (`_seek`)

`Source/stream_parser_ffmpeg.c:1835`, mirroring the existing pos-based guard at
`:1822`:

```c
if( ff_p->duration > 0 && time >= ff_p->duration ) {
    int margin = 500;
    if( margin > ff_p->duration )
        margin = ff_p->duration / 2;
    DBGP serprintf( "at end (time) %d %d\r\n", time, ff_p->duration );
    s->video_parse_end = 1;
    s->audio_parse_end = 1;
    time = ff_p->duration - margin;
}
```

This ensures a beyond-end time seek can never surface as a failed
`avformat_seek_file` → `stream_set_error(VE_FILE_ERROR)` → `MEDIA_ERROR` → app error
dialog (`Source/stream_video.c:5150`, `Source/avos_mp_video.c:211`). (The parse-end
flags are reset after a successful seek at `Source/stream_parser_ffmpeg.c:1871`, same
as the pos branch.)

### #3 — Make end detection robust to an armed beyond-end drop target

`Source/stream_video.c`, `_video_presentation_drained()`:

```c
if (s->seek_video_drop && s->seek_video_target_pending)
    return 1;
```

Once the parser has genuinely reached EOF with no buffered data (`video_end` set,
`cdata_now/next` invalid), an unreached drop target (`seek_video_target_pending` still
set) means the seek aimed past the last frame and every trailing frame was
intentionally discarded — the stream is drained. This lets completion be reported even
if a beyond-end seek slips through #1/#2 (e.g. a frame-accurate/scrub preview seek).
Normal playback and in-content seeks are unaffected (`seek_video_drop` is 0 or the
target was reached, so `seek_video_target_pending` is already clear).

## Files changed

| File | Change |
| --- | --- |
| `Source/stream_video.c` | `#define SEEK_END_MARGIN_MS 500`; clamp at/after-end seek in `stream_seek_time`; short-circuit `_video_presentation_drained` for an unreached beyond-end drop target |
| `Source/stream_parser_ffmpeg.c` | beyond-end guard in the `_seek` time branch |

## Validation

- `git diff --check` on the two files: clean.
- `./compile.sh` builds (the workspace `make native_avos_full` target).
- Not reproduced on-device here. To confirm the exact gate on a real freeze, capture
  the engine state at the stall (`video_end`, `cdata_now/next.valid`,
  `video_decoder_drained`, `frame_q_count(disp_q)`, `sink_drained`,
  `seek_video_target_pending`) via the `Debug[]`/`avsh` surface; the `SEEK_CLAMP_END`
  and `VIDEO_SEEK_TARGET_READY` log strings bracket the two code paths.

## Note on the app-side workaround

With the native fix in place, the `mIsBusy` watchdog and `endMargin` special-casing
added in aos-Video#216 are no longer required; if kept, they become redundant
insurance rather than the actual fix.
