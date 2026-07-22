# MediaCodec Audio Decoder

## Purpose

`Source/codec_mediacodec_audio.c` lets AVOS use Android MediaCodec as an
alternative to `Source/codec_ffmpeg_audio.c` for compressed audio decoding.
It does not hand audio rendering to Android. MediaCodec only converts the
compressed stream to PCM; AVOS still owns filtering, synchronization, and
delivery to AudioTrack.

The effective PCM path is:

```text
compressed packets
    -> AVOS access-unit framing
    -> Android MediaCodec decoder
    -> AVOS-owned PCM copy
    -> AVOS audio filters and channel policy
    -> AudioTrack PCM sink
```

This is distinct from compressed passthrough, where AVOS sends AC3, E-AC3,
DTS, TrueHD, or another compressed representation to AudioTrack and the
downstream device performs the decode.

## Main components

- `Source/codec_mediacodec_audio.c` implements the `STREAM_DEC_AUDIO`
  contract and adapts MediaCodec output to `AUDIO_FRAME`.
- `external/android/libsfdec/dec_audio.c` is the C dispatch layer used by the
  AVOS decoder.
- `external/android/libsfdec/codec_audio_mediacodec.cpp` owns the NDK
  `AMediaCodec` instance, input/output buffer calls, format discovery, and EOS.
- `Source/stream_config.c` orders MediaCodec and FFmpeg candidates according to
  the configured audio-decoder preference.
- `Source/stream_audio.c` consumes the resulting PCM through the same filter,
  accumulation, clock, and AudioTrack paths used by FFmpeg-decoded PCM.

## Decoder selection

MediaCodec is considered only when the device capability mask or legacy codec
probe reports the source format as supported. With the MediaCodec preference,
the candidate order is MediaCodec then FFmpeg. Otherwise FFmpeg remains first
and MediaCodec is a later fallback.

The registered formats are:

- MPEG audio and MP3;
- AAC and AAC-LATM;
- AC3;
- E-AC3 and E-AC3 JOC;
- DTS, DTS-HD, and DTS-HD MA;
- TrueHD;
- Opus.

Registration does not guarantee that a particular device can decode the
format. Selection is still gated by the reported MediaCodec capability. JOC
may use the E-AC3 base decoder when the device reports E-AC3 but not a distinct
JOC decoder.

MediaCodec is rejected before open when `audio->request_channels` asks for a
different PCM channel count. Channel conversion and downmix remain the FFmpeg
path's responsibility; the MediaCodec adapter does not silently ignore the
requested output layout.

## Input contract

MediaCodec input must be a complete compressed access unit. AVOS must never
truncate a large unit or report bytes as consumed before their complete unit
has been accepted.

MP3, MPEG audio, AAC-LATM, AC3, and DTS can arrive fragmented or with multiple
frames in one input chunk. These formats pass through an FFmpeg parser. Parser
output is copied into an AVOS-owned retained buffer and remains there while a
MediaCodec input buffer is temporarily unavailable.

Other registered formats use the demuxer's packet framing directly. A direct
or parsed access unit larger than the 512 KiB configured maximum is rejected
instead of being submitted partially.

`decoded` reports only compressed bytes accepted or safely retained by the
decoder contract. A temporary input-buffer shortage returns zero consumption
and is retried; negative or partial MediaCodec acceptance is an error.

## Output contract

MediaCodec output buffers belong to Android and become invalid when released.
The adapter therefore:

1. validates the output pointer, offset, size, sample rate, and channel count;
2. accepts only signed 16-bit PCM;
3. copies the PCM into reusable AVOS-owned storage;
4. releases the MediaCodec output buffer exactly once;
5. publishes the copied storage through `AUDIO_FRAME`.

Output-format changes update cached channel count, sample rate, channel mask,
and PCM encoding. A normal `TRY_AGAIN_LATER` or format-change notification is
a successful decoder call with no PCM, not a fatal decode error.

The reported MediaCodec channel mask is propagated to `AUDIO_PROPERTIES` when
available. Sample rate is reported directly and is never multiplied by the
number of channels.

## Flush, drain, and shutdown

Seek and natural end of stream have different contracts:

- Seek flush discards retained access units, resets the parser and drain state,
  and flushes MediaCodec so old-epoch PCM cannot escape after the seek.
- Natural EOF drains parser output first, queues MediaCodec input EOS, returns
  delayed PCM through the normal filter/sink path, and reports completion only
  after output EOS.
- Close calls `AMediaCodec_stop()` through `dec_audio_stop()` so an in-flight
  dequeue is interrupted before the codec is destroyed.

Input dequeue waits are bounded to 5 ms and output polling is bounded. Stop,
seek, and track changes must not depend on an unbounded MediaCodec call
returning voluntarily.

## Synchronization

Once decoded, MediaCodec PCM follows the ordinary AVOS PCM timing model. The
stream clock advances from PCM frames actually written to AudioTrack, and the
heard timestamp subtracts the AudioTrack/filter latency used by the selected
sync mode. MediaCodec presentation timestamps do not replace the AVOS heard
clock or directly schedule video.

This keeps decoder choice separate from renderer clock ownership: choosing
MediaCodec instead of FFmpeg must not create a second A/V anchor.

## Playback speed limitation

Variable-speed playback is disabled while MediaCodec is the active PCM audio
decoder. `stream_set_av_speed()` rejects non-1.0 requests and restores the
native audio-speed state to 1.0.

MediaCodec is an API, not a guarantee that the selected implementation is a
software decoder or can produce PCM faster than real time. On the tested TV,
AC3 MediaCodec output stayed near 1.0x even when AVOS requested 1.4x-1.5x and
configured a 2x MediaCodec operating-rate hint. Both speed mechanisms then
failed for the same upstream reason:

- atempo needed more decoded source PCM per wall-clock second;
- AudioTrack PlaybackParams consumed decoded PCM faster;
- MediaCodec did not feed either path fast enough;
- AudioTrack accumulated more than 300 underruns and the heard clock stalled;
- video waited for audio, appearing to glitch and freeze while sound ran.

FFmpeg variable-speed playback remains supported because software decoding can
supply PCM faster than real time on the tested devices. Do not automatically
switch an active MediaCodec playback to FFmpeg when speed changes: reopening a
decoder mid-stream creates an audible gap, loses decoder state, and adds a
separate lifecycle path. The UI may also disable speed controls when
MediaCodec is selected, but AVOS retains the enforcement guard for restored
settings and non-UI callers.

See `avos-132.log` and `avos-133.log` for the measured failure and
`REMEMBER.md` for the short guardrail summary.

## Known limitations

- MediaCodec output encoder delay and padding are not yet applied where Android
  exposes them.
- The adapter does not currently distinguish vendor hardware/DSP codecs from
  Android platform software codecs.
- Unsupported PCM encodings fall back or fail; there is no shared PCM format
  converter in this path.
- Requested channel conversion falls back to FFmpeg.
- Device codec advertisements can be inaccurate, so successful capability
  selection does not guarantee successful open or correct vendor behavior.

## Validation checklist

For each claimed codec/device combination, verify:

- playback from start and repeated playback;
- forward and backward seek;
- pause/resume and rapid stop;
- audio-track change;
- natural EOF and short clips;
- PCM sample rate, channel count, channel mask, and sample count versus FFmpeg;
- no old-epoch PCM after flush;
- no truncated or duplicated compressed access units;
- balanced MediaCodec output-buffer release;
- stable A/V sync at 1.0x;
- rejection of non-1.0 speed without an AudioTrack underrun or video freeze.
