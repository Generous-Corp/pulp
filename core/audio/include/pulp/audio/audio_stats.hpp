#pragma once

/// @file audio_stats.hpp
/// Small POD counter struct for release-safe audio observability.
///
/// `AudioStats` is the always-available, minimal counter subset described in
/// the audio observability harness plan
/// (`planning/2026-06-09-audio-observability-and-validation-harness-plan.md`,
/// Section 4 "Runtime Probe Infrastructure", "Release builds" tier). It is a
/// trivially-copyable aggregate so it can be published lock-free and read on
/// any thread.
///
/// OWNERSHIP BOUNDARY (do not relitigate without a code change):
///   - Signal-content counters — `callbacks`, `underruns`, `clipped_blocks`,
///     `nan_blocks` — are owned by the probe / host audio path and reflect the
///     content of rendered blocks.
///   - Device/backend counters — `device_xruns`, `cpu_overloads` — are MIRRORS.
///     They are populated by the HOST from `AudioDeviceManager::xrun_count()`
///     and the CoreAudio overload listener, which already own that accounting.
///     `pulp::audio::AudioProbe` MUST NOT increment `device_xruns` or
///     `cpu_overloads` and must not shadow the device's own counters; doing so
///     creates a second copy that silently diverges from the device owner.
///
/// This struct is intentionally tiny and has no behavior. It carries no
/// std::vector, no per-channel data, and no FFT/spectrum fields. Richer
/// per-stage signal facts (NaN/Inf counts, exact clip counts, silence runs,
/// sequence/stale accounting, stage metadata) live on
/// `pulp::audio::AudioProbeSnapshot`, not here.

#include <cstdint>

namespace pulp::audio {

/// Minimal release-safe audio counter aggregate. POD; trivially copyable.
///
/// All counters are monotonic, free-running totals (never reset per block).
/// The host is free to publish this through a `runtime::TripleBuffer` for a
/// UI to poll; nothing here allocates or locks.
struct AudioStats {
    /// Number of audio callbacks observed at this stage.
    std::uint64_t callbacks = 0;

    /// Blocks where the source-backed audio path underran (e.g. a streaming
    /// handoff had to zero-fill). Owned by the audio path, not the device.
    std::uint64_t underruns = 0;

    /// Blocks that contained at least one clipped sample (|x| > ceiling).
    std::uint64_t clipped_blocks = 0;

    /// Blocks that contained at least one NaN or Inf sample.
    std::uint64_t nan_blocks = 0;

    /// MIRROR of `AudioDeviceManager::xrun_count()`. Populated by the host from
    /// the device owner — never incremented by the probe.
    std::uint64_t device_xruns = 0;

    /// MIRROR of the CoreAudio overload listener's count. Populated by the host
    /// from the device owner — never incremented by the probe.
    std::uint64_t cpu_overloads = 0;
};

static_assert(sizeof(AudioStats) == 6 * sizeof(std::uint64_t),
              "AudioStats must stay a tight POD counter aggregate");

}  // namespace pulp::audio
