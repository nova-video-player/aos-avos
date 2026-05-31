# AVOS Debug System

Runtime debug commands and log level control via the `avsh` socket interface.

---

## How avsh works

`avsh` is a Unix domain socket server running inside the AVOS process. It listens on:

- **Android (Nova):** `/data/data/com.archos.mediacenter.videoti/files/avsh.sock`
- **Linux desktop:** `/tmp/avshsocket`

The matching client binary (`avsh_client`) sends a text command string to that socket.
The server calls `debug_do_cmd()` which dispatches to the registered handler.

**All commands are only active in `DEBUG_MSG` builds.** In release builds the macros expand
to nothing and the socket listener still exists but `debug_do_cmd()` is a no-op.

---

## Sending a command from adb

```sh
# one-shot
adb shell run-as com.archos.mediacenter.videoti \
    /data/data/com.archos.mediacenter.videoti/files/avsh_client <command> [arg]

# interactive shell (type 'q' to quit)
adb shell run-as com.archos.mediacenter.videoti \
    /data/data/com.archos.mediacenter.videoti/files/avsh_client
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
| `dbgap`    | DBG_AUDIO_PLAYER    | 2       | Audio player engine |
| `dbgvp`    | DBG_VIDEO_PLAYER    | 2       | Video player engine |
| `dbgp`     | DBG_PARSER          | 1       | Stream parser (FFmpeg demux) |
| `dbgs`     | DBG_STREAM          | 2       | Stream engine |
| `dbgy`     | DBG_SYNC            | 1       | A/V sync |
| `dbgsink`  | DBG_SINK            | 2       | Video/audio sink |
| `dbgca`    | DBG_CA              | 0       | Codec audio |
| `dbgcv`    | DBG_CV              | 0       | Codec video |
| `dbgad`    | DBG_AUDIODEVICE     | 0       | Audio device (low-level) |
| `dbgdss`   | DBG_DSS             | 0       | DSS subsystem |
| `dbgpr`    | DBG_PROBE           | 0       | Format probing |
| `dbgt`     | DBG_THREADS         | 0       | Thread management |

To get verbose AudioTrack logs (DBG, DBG2, DBG3 all active):

```sh
avsh dbga 3
```

### DECLARE_DEBUG_PARAM — integer parameter

```
avsh <name>          # print current value
avsh <name> <n>      # set to n
```

### DECLARE_DEBUG_COMMAND — arbitrary function

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

---

## Stream and sync parameters

| avsh name | Variable                  | Effect |
|-----------|---------------------------|--------|
| `sep`     | command                   | A/V delay +10ms |
| `sem`     | command                   | A/V delay -10ms |
| `ses`     | command                   | A/V delay set to value |
| `smd`     | stream_max_delay          | Max allowed A/V delay before drop (ms) |
| `ssmd`    | stream_sink_max_delay     | Max sink queue depth |
| `sfps`    | stream_fps_mode           | FPS mode override |
| `svd`     | stream_vcodec_delay       | Video codec extra delay offset |
| `samc`    | stream_audio_max_channels | Cap audio channel count |
| `sdra`    | stream_force_drop_audio   | Force audio frame drops |

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
