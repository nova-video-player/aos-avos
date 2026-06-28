# AVOS Debug System

Runtime debug commands and log level control via the `avsh` socket interface.

---

## How avsh works

`avsh` is a Unix domain socket server running inside the AVOS process. It listens on:

- **Android (Nova):** `/data/data/com.archos.mediacenter.video/files/avsh.sock`
- **Linux desktop:** `/tmp/avshsocket`

The matching client binary (`avsh_client`) sends a text command string to that socket.
The server calls `debug_do_cmd()` which dispatches to the registered handler.

**All commands are only active in `DEBUG_MSG` builds.** In release builds the macros expand
to nothing and the socket listener still exists but `debug_do_cmd()` is a no-op.

---

## Sending a command from adb

```sh
# one-shot
adb shell run-as com.archos.mediacenter.video \
    /data/data/com.archos.mediacenter.video/files/avsh_client <command> [arg]

# interactive shell (type 'q' to quit)
adb shell run-as com.archos.mediacenter.video \
    /data/data/com.archos.mediacenter.video/files/avsh_client
```

Or if the device is rooted / you have a helper script:

```sh
avsh <command> [arg]
```

---

## Command types

### DECLARE_DEBUG_SWITCH — log level control

Sets `Debug[DBG_*]` to an integer level. Macros like `DBG`, `DBG2`, `DBG3` check
`Debug[DBG_AUD] > 0`, `> 1`, `> 2` respectively.

```
avsh <name>          # print current level
avsh <name> <n>      # set to integer n (0 = off)
avsh <name> 0x<hex>  # set via hex value
```

| avsh name  | Debug flag          | Default | Notes |
|------------|---------------------|---------|-------|
| `dbga`     | DBG_AUD             | 2       | AudioTrack / audio interface |
| `dbgad`    | DBG_AUDIODEVICE     | 0       | Audio device (low-level) |
| `dbgagc`   | DBG_AGC             | 0       | Audio Auto Gain Control |
| `dbgap`    | DBG_AUDIO_PLAYER    | 2       | Audio player engine |
| `dbblend`  | DBG_BLENDING        | 0       | Graphics alpha blending |
| `dbgc`     | DBG_CHU             | 0       | CHU files / chunks parsing |
| `dbgca`    | DBG_CA              | 0       | Codec audio |
| `dbgcomp`  | DBG_COMP            | 0       | Compositor / rendering engine |
| `dbgcv`    | DBG_CV              | 0       | Codec video |
| `dbgd`     | DBG_DSP             | 0       | DSP hardware decoding / DSP interface |
| `dbgde`    | DBG_DE              | 0       | Mainloop data events |
| `dbgdrm`   | DBG_DRM             | 0       | DRM / content security engine |
| `dbgdss`   | DBG_DSS             | 0       | DSS subsystem |
| `dbgdv`    | DBG_DV              | 0       | DataViews UI rendering |
| `dbgdvs`   | DBG_DVS             | 0       | DVSource network sources |
| `dbgf`     | DBG_FILE            | 0       | File I/O |
| `dbgfb`    | DBG_FB              | 0       | Framebuffer / OSD |
| `dbghd`    | DBG_HD              | 0       | Hard drive operations / interface |
| `dbgimg`   | DBG_IMG             | 0       | Static image loading / rendering |
| `dbgkey`   | DBG_KEY             | 0       | Input key/button events and mapping |
| `dblayout` | DBG_LAYOUT          | 0       | GUI layout components positioning |
| `dbgm`     | DBG_MALLOC          | 0       | Memory allocation tracking / statistics |
| `dbgmng`   | DBG_MANGLER         | 0       | Video frame mangler / format conversion |
| `dbgp`     | DBG_PARSER          | 1       | Stream parser (FFmpeg demux) |
| `dbgpath`  | DBG_PATH            | 0       | File path parsing / URL routing |
| `dbgpm`    | DBG_PM              | 0       | Power management (HDD spin-down, etc.) |
| `dbgpr`    | DBG_PROBE           | 0       | Format probing |
| `dbgpsi`   | DBG_PSI             | 0       | MPEG Program Specific Information (PSI/SI) tables |
| `dbgq`     | DBG_Q               | 0       | Queue management |
| `dbgrd`    | DBG_REDRAW          | 0       | Screen redraw / composition updates |
| `dbgrsz`   | DBG_RESIZER         | 0       | Video resizer |
| `dbgs`     | DBG_STREAM          | 2       | Stream engine |
| `dbgsink`  | DBG_SINK            | 2       | Video/audio sink |
| `dbgstate` | DBG_STATE           | 0       | Player state machine |
| `dbgsub`   | DBG_SUB             | 0       | Subtitles |
| `dbgt`     | DBG_THREADS         | 0       | Thread management |
| `dbgthumb` | DBG_THUMB           | 0       | Thumbnail generation / scaling |
| `dbgv`     | DBG_VID             | 0       | Video rendering / codec interface |
| `dbgvl`    | DBG_V4L             | 0       | Video4Linux driver / camera input |
| `dbgvp`    | DBG_VIDEO_PLAYER    | 2       | Video player engine |
| `dbgw`     | DBG_WMA             | 0       | WMA format decoding |
| `dbgy`     | DBG_SYNC            | 1       | A/V sync |

To get verbose AudioTrack logs (DBG, DBG2, DBG3 all active):

```sh
avsh dbga 3
```

### DECLARE_DEBUG_PARAM — integer parameter

Used for integer variables that can be tuned at runtime.

```
avsh <name>          # print current value
avsh <name> <n>      # set to n
```

### DECLARE_DEBUG_TOGGLE — boolean toggle

Used for boolean flags (0 or 1).

```
avsh <name>          # toggle value (0 -> 1 or 1 -> 0)
avsh <name> <n>      # set to exactly n (0 or 1)
```

### DECLARE_DEBUG_COMMAND — arbitrary function

Executes a C function with `argc/argv` arguments.

```
avsh <name> [args...]
```

---

## Audio-specific parameters and commands

### AudioTrack (audio_interface_audiotrack_java.c)

| avsh name           | Type  | Default | Effect |
|---------------------|-------|---------|--------|
| `at_underrun`       | param | 0       | Log AudioTrack underrun events |
| `at_disable_recovery` | param | 1     | Disable automatic AudioTrack error recovery |
| `at_mode2_audit`    | param | 0       | Enable mode2 playhead audit (JNI, every 2s) |

#### at_mode2_audit

Enables periodic logging of the mode2 compressed-audio pipeline queue depth.
Off by default to avoid JNI overhead during normal playback.

When enabled, every 2 seconds during active mode2 passthrough writes:

```
mode2_playhead_audit: fmt=<hex> logical=<samples> i_written=<bytes>
    playhead=<frames> ts_frames=<frames> ts_ns=<ns>
    derived_logical=<ms> derived_playhead=<ms>
    selected=<ms> pipeline=<ms> app=<ms>
```

- `logical` — PCM-equivalent samples accumulated from fakeSize writes (audio duration written)
- `playhead` — `getPlaybackHeadPosition()` frames (frames played out)
- `ts_frames` / `ts_ns` — `AudioTrack.getTimestamp()` position and wall-clock
- `derived_playhead` — `(logical - playhead) * 1000 / rate` ms; real hardware queue depth
- `derived_logical` — same but against timestamp position
- `selected` — delay value currently returned by `audiotrack_get_latency()` (heard_ts anchor)

To enable:

```sh
avsh at_mode2_audit 1
```

To disable:

```sh
avsh at_mode2_audit 0
```

### Audio interface (audio_interface.c)

| avsh name | Type    | Effect |
|-----------|---------|--------|
| `aif`     | command | Force audio interface: `aif 0`=null, `1`=audiotrack, `2`=audiotrack_new, `3`=audiotrack_java, `4`=opensles |

### Audio SPDIF / passthrough (audio_spdif.c)

| avsh name        | Type  | Default | Effect |
|------------------|-------|---------|--------|
| `passthrough_on` | param | —       | Force passthrough on/off |

### AC3 recode passthrough mode (stream_audio.c)

| avsh name               | Type  | Default | Effect |
|-------------------------|-------|---------|--------|
| `ac3_force_mode2`       | param | 0       | Force raw AC3 AudioTrack mode2 for recoding, overriding IEC61937 capability. On a box that reports IEC support (resolves to mode1) this forces the mode2 (raw 2560-byte AC3 frame) path an eARC route would select. |
| `ac3_mode2_plain_policy`| param | 1       | Production gate. When 1, AC3 recode resolving to passthrough mode2 uses the plain-mode2 PTS-seeded STREAM_SYNC_SAMPLES clock **and** app_latency. When 0, reverts BOTH to the legacy mode1 CDATA anchor + pipeline_latency (the broken baseline). |

Both params latch at sink-resolve / startup, so **set them before starting a fresh
playback** — toggling mid-stream does not re-anchor an already-running stream. See the
eARC emulation workflow below.

---

## Stream and sync parameters

| avsh name | Type    | Variable                  | Effect |
|-----------|---------|---------------------------|--------|
| `sep`     | command | —                         | A/V delay +20ms (incremental) |
| `sem`     | command | —                         | A/V delay -20ms (incremental) |
| `ses`     | command | `stream_dbg_delay`        | A/V delay set to value (absolute ms) |
| `smd`     | param   | `stream_max_delay`        | Max allowed A/V delay before drop (ms) |
| `ssmd`    | param   | `stream_sink_max_delay`   | Max sink queue depth (ms) |
| `sfps`    | param   | `stream_fps_mode`         | FPS mode override (0=auto, 1=forced) |
| `svd`     | param   | `stream_vcodec_delay`     | Video codec extra delay offset (ms) |
| `samc`    | param   | `stream_audio_max_channels` | Cap audio channel count |
| `sdra`    | param   | `stream_force_drop_audio` | Force audio frame drops (debug) |
| `sdel`    | command | `delay_fb`                | Set internal feedback delay |
| `sxbmc`   | command | `stream_use_xbmc_smoothing` | Toggle XBMC-style sync smoothing |
| `ssm`     | command | `stream_sync_mode`        | Toggle sync mode (CDATA vs SAMPLES) |
| `smb`     | param   | `stream_buffer_size`      | Network/File buffer size (KB) |
| `sfb`     | param   | `stream_force_buffer`     | Force buffering even if stream is fast |
| `sbt`     | command | `stream_buffer_time`      | Set stream buffer time target (ms) |
| `scb`     | command | `_stream_clear_buffer`    | Clear stream buffer queue |
| `spp`     | command | `_stream_props`           | Print stream properties (codec, format, metadata) |
| `ssy`     | toggle  | `stream_no_sync`          | Disable A/V synchronization (runs video as fast as possible) |
| `sso`     | command | `_stream_no_output`       | Toggle all audio/video output rendering |
| `spa`     | toggle  | `stream_no_parser`        | Toggle / bypass parser input |
| `sps`     | param   | `stream_parser_sleep`     | Set stream parser sleep delay (ms) |
| `spm`     | param   | `stream_parser_max`       | Set stream parser maximum packets queue size |

---

## Video and Hardware parameters

| avsh name | Type    | Variable                  | Effect |
|-----------|---------|---------------------------|--------|
| `svid`    | command | —                         | Toggle video decoding/rendering |
| `saud`    | command | —                         | Toggle audio decoding/rendering |
| `ssub`    | command | —                         | Toggle subtitle rendering |
| `sns`     | toggle  | `stream_no_seek`          | Disable seeking |
| `sfv`     | toggle  | `stream_fake_video`       | Use a fake video sink (no display) |
| `sfa`     | toggle  | `stream_fake_audio`       | Use a fake audio sink (no output) |
| `sror`    | toggle  | `stream_no_reorder`       | Disable frame reordering (B-frames) |
| `sdbl`    | toggle  | `stream_no_deblock`       | Disable h.264 deblocking filter |
| `sfde`    | toggle  | `stream_force_sw_deinterlacing` | Force software deinterlacer |
| `szf`     | toggle  | `stream_zero_fill`        | Fill gaps in stream with zeros |
| `sif`     | toggle  | `stream_io_fail`          | Simulate I/O failure |
| `sih`     | toggle  | `stream_io_hang`          | Simulate I/O hang |

### MediaCodec (sfdec / sfdec2) parameters

| avsh name | Type    | Variable                  | Effect |
|-----------|---------|---------------------------|--------|
| `sfhw`    | param   | `sfdec_force_hw`          | Force hardware decoding (-1=auto, 0=SW fallback, 1=force HW) |
| `sffb`    | toggle  | `sfdec_force_blit`        | Force blitting to texture/surface |
| `sfnd`    | toggle  | `sfdec_no_drop`           | Disable late frame dropping in decoder |
| `sfmf`    | param   | `sfdec_max_frames`        | Max queued frames in decoder (default 2) |
| `sfth`    | param   | `sfdec_threshold`         | Late threshold for drop decision in ms (default 200ms) |

### FFmpeg Video Decoder parameters

| avsh name | Type    | Variable                  | Effect |
|-----------|---------|---------------------------|--------|
| `ffca`    | toggle  | `_ff_force_cached`        | Force cached frames rendering |
| `ffdr`    | toggle  | `_ff_do_render`           | Enable rendering of decoded frames |
| `ffcs`    | toggle  | `_ff_colorspace`          | Enable colorspace conversion |
| `fftc`    | param   | `_ff_thread_count`        | FFmpeg video decoder thread count |
| `fffc`    | param   | `_ff_frame_count`         | FFmpeg video decoder max queued frame count |
| `fff`     | toggle  | `_ff_fake`                | Enable fake decoding (bypass real decompression) |
| `ffde`    | toggle  | `_ff_deinterlace`         | Enable FFmpeg deinterlacer |
| `ffdel`   | param   | `_ff_deinterlacing_max_height` | Max height for deinterlacing filter |

### OMX Video Decoder parameters

| avsh name | Type    | Variable                  | Effect |
|-----------|---------|---------------------------|--------|
| `omxf`    | toggle  | `omx_flush`               | Force flush OMX decoder buffers |
| `dblk`    | command | `_set_deblocking_value`   | Set H.264 deblocking filter strength |

### Hardware / Device configurations

| avsh name | Type    | Variable                  | Effect |
|-----------|---------|---------------------------|--------|
| `nhb`     | toggle  | `no_hw_buf`               | Disable hardware buffers usage |
| `hdd`     | toggle  | `has_hdd`                 | Force mock Hard Drive presence |
| `dsp`     | toggle  | `has_dsp`                 | Force mock DSP presence |
| `cpu`     | param   | `has_cpu`                 | Force CPU configuration override |
| `dspod`   | toggle  | `has_dsp_overdrive`       | Force mock DSP overdrive mode |

---

## FFmpeg Parser parameters

| avsh name | Type    | Variable                  | Effect |
|-----------|---------|---------------------------|--------|
| `ffmd`    | param   | `max_delay`               | FFmpeg demuxer max delay |
| `fflog`   | param   | `log_debug`               | FFmpeg internal log level |
| `ffpts`   | toggle  | `use_pts`                 | Use PTS instead of DTS for timing |
| `ffreo`   | toggle  | `force_reorder`           | Force packet reordering in demuxer |
| `ffvp`    | param   | `force_vpid`              | Force video PID/Stream ID |
| `ffap`    | param   | `force_apid`              | Force audio PID/Stream ID |
| `fffs`    | param   | `ff_force_seek`           | Force seek mode behavior options |

---

## Global Player Controls

| avsh name | Type    | Effect |
|-----------|---------|--------|
| `avp`     | command | Toggle pause / resume playback |
| `avs`     | command | Stop playback |
| `avt`     | command | Print current playback time and total duration (ms) |

---

## File and Stream Selection / Information

| avsh name | Type    | Effect |
|-----------|---------|--------|
| `fi`      | command | Print media file metadata / info |
| `ft`      | command | Print media container/file type |
| `vidp`    | toggle  | Toggle whether video should start paused |
| `vas`     | command | Select audio stream index (`vas [index]` or cycle if no index) |
| `vss`     | command | Select subtitle stream index (`vss [index]` or cycle if no index) |

---

## help command

```sh
avsh help
```

Prints all registered commands with their source file location.

---

## Workflow examples

### Investigate EAC3 A/V sync

```sh
# Enable audio and sync logs
avsh dbga 3
avsh dbgy 2
# Enable mode2 playhead queue depth audit
avsh at_mode2_audit 1
# Play content, collect logcat
adb logcat -s avos | tee avos-eac3.log
# When done
avsh at_mode2_audit 0
avsh dbga 2
avsh dbgy 1
```

### Investigate TrueHD / DTS-HD passthrough

Same as above. The audit logs `fmt=4646` (TrueHD) or `fmt=4949` (DTS-HD MA) so you can
filter: `grep mode2_playhead_audit avos-truehd.log`.

### Emulate an eARC mode2 AC3-recode sink on a non-TV box (e.g. Shield)

**Why.** With AC3 recoding, a real eARC soundbar route reports no IEC61937 support, so the
sink resolves to passthrough **mode2** (raw 2560-byte AC3 frames). An Android TV box like
the Shield reports IEC support and resolves to **mode1** (IEC-wrapped, 6144-byte bursts),
so the mode2 path can't normally be hit there. `ac3_force_mode2` forces mode2 on the box so
the eARC timing path can be reproduced and validated without the eARC hardware.

Because the mode/policy latch when the AC3 sink resolves, **set the knobs first, then start
a fresh AC3-recode playback** (stop any current playback before starting).

```sh
# logging: stream engine (covers mode2_sync_mode + startup_anchor_commit) and sync
avsh dbgs 2
avsh dbgy 1
avsh at_mode2_audit 0          # keep the JNI audit OFF (it is a Heisenbug, skews sync)

# A) production candidate, forced mode2  -> expect IN SYNC
avsh ac3_force_mode2 1
avsh ac3_mode2_plain_policy 1
# start a FRESH AC3-recode playback, collect: adb logcat -s avos_player | tee avos-A.log

# B) broken baseline (policy off), forced mode2  -> expect OUT OF SYNC (audio leads)
avsh ac3_force_mode2 1
avsh ac3_mode2_plain_policy 0
# fresh playback -> avos-B.log

# C) mode1 regression check, policy on  -> expect UNCHANGED / in sync
avsh ac3_force_mode2 0
avsh ac3_mode2_plain_policy 1
# fresh playback -> avos-C.log
```

What to confirm in each log:

- **All:** `stream_audio_setup_ac3_sink: AC3 recode mode=<m> iec=<i> forced_mode2=<f>`
  (this line is unguarded — prints without `dbgs`). On a forced-mode2 box you see
  `mode=2 iec=1 forced_mode2=1`; a real eARC sink instead shows `iec=0` and reaches
  mode2 organically. The timing policy under test is identical either way — only `iec`
  differs.
- **(A)** `mode2_sync_mode: forcing STREAM_SYNC_SAMPLES (was 0) ac3_recode=1` then
  `mode2_sync_mode: ref=0`, and **no** `startup_anchor_commit` → samples clock + app
  latency; `heard_delay` ≈ static 171 + pacer 96 = **267 ms**.
- **(B)** no `mode2_sync_mode` line, and `startup_anchor_commit: ... static=724 start=724`
  → the pipeline-latency anchor that desyncs the eARC route.
- **(C)** `mode=1 ... forced_mode2=0` and `startup_anchor_commit: ... static=171 start=171`
  → mode1 path, unaffected by the flag.

Note: internal `diff` stays bounded near 0 in **all three** even when (B) is audibly out
of sync — judge this path by `heard_delay` / the `startup_anchor_commit` value and the
acoustic result, never by the `diff` column.

### Check current A/V delay offset

```sh
avsh ses        # prints current value without changing it (pass no arg)
```

### Force audio interface for testing

```sh
avsh aif 3      # force audiotrack_java
avsh aif 2      # force audiotrack_new
avsh aif        # print current
```
