# A/V Synchronization Rules (atempo + android_sync=0)

This document outlines the fundamental principles learned while stabilizing audio-speed control in Nova Video Player. Adhering to these rules is critical to prevent visual stutter, audio dropouts, and permanent A/V desync.

## 1. Single Authority (Audio Master)
When `atempo` software resampling is active, the audio clock is the **authoritative master**. 
*   **The Principle:** The video scheduler must be a "slave" to the audio clock. It should not attempt to independently "fix" its timeline in steady state.
*   **The Implementation:** In `videosink_put_time`, implement **Master Clock Deference**. Once a master anchor is established, the video sink must trust incoming timestamps and skip independent Hard Resets.

## 2. Domain Consistency (TS vs RST)
Math must be performed in the correct domain to avoid scaling errors.
*   **RST (Real Stream Time):** The media PTS domain. This domain flows at `Speed` seconds per physical second.
*   **TS (Time-Scaled):** The wall-clock domain. This domain flows at 1.0 second per physical second.
*   **The Rule:** `atempo` output duration is equivalent to the TS domain. Always convert video PTS to TS via `rst_to_ts_time()` before calculating synchronization differences.

## 3. Unified Anchoring
Synchronization depends on a single, shared point of truth between media time and system time.
*   **The Principle:** At the moment of speed change, the global timeline map and the hardware video sink must be reset **simultaneously** to the same "Heard Audio" position.
*   **The heard_ts:** `audio_time - buffered_latency`.
*   **The Rule:** Use the **Authoritative RST** calculation: $RST_{anchor} = TS_{heard} \cdot Speed_{new}$. This clears historical drift and establishes a mathematically perfect starting point for the new speed segment.

## 4. Strict Grace Windows
The pipeline is noisy immediately after a speed change.
*   **The Principle:** Implement a Hard Barrier (typically 40 frames) after any speed change.
*   **The Rule:** During the grace period, **return early** from the video sink. Do not allow jittery transition measurements to trigger secondary Hard Resets, which would corrupt the master anchor established in Rule 3.

## 5. Mistake Anti-Patterns (What NOT to do)
*   **Maginitude-Based Resets:** Never trigger a Hard Reset based solely on the magnitude of system latency (e.g., `smoothed_av_delay > 150ms`). High-latency devices naturally exceed this, leading to catastrophic reset loops.
*   **Old Timeline Reliance:** Do not use the old (potentially drifted) timeline mapping to calculate the new RST anchor. This carries error forward. Always use the authoritative $RST = TS \cdot S$ relationship at speed changes.
*   **Split Authority:** Never allow the player thread and the decoder thread to independently reset the monotonic baseline. One initiates (Player), the other adheres (Decoder/Sink).
*   **Leaky Suppression:** Ensure that "suppressing" a reset means returning early, not just skipping a log message. Fall-through resets are the primary cause of desync during rapid speed increments.

## 6. Threshold Strategy
*   **Atempo Mode:** Trust is high. Use a massive deference threshold in steady state to avoid fighting the audio master.
    *   **The Rule:** Use `MAX(500ms, smoothed_av_delay * 1.5)`. This accommodates high-latency hardware while preserving strict authority.
*   **Normal Mode:** Trust is lower due to hardware clock drift. Use tight adaptive thresholds ($MAX(50ms, latency / 2)$) to maintain sync on jittery devices.

## 7. Transition Stability (Rate-Limiting)
Rapid speed changes can thrash the media pipeline and cause permanent desync.
*   **The Principle:** Debounce and coalesce rapid speed requests. Ensure the system has a "drain margin" before applying a transition.
*   **The Rule:** Defer speed changes if they occur more than once per second or if the video sink is not at least 100ms ahead of the "heard" audio. Coalesce intermediate requests to the latest target speed.

## 8. Stability Clamp (heard_ts)
The "heard" audio position calculation relies on accurate latency reporting.
*   **The Principle:** Jittery latency reports can cause the audio master clock to oscillate.
*   **The Rule:** In `_stream_av_diff`, clamp the used latency to `MAX(measured_latency, smoothed_latency - 50ms)`. This prevents the smoothed baseline from pulling the clock too far forward during rapid latency drops.