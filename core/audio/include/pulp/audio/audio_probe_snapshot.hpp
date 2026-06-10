#pragma once

/// @file audio_probe_snapshot.hpp
/// The realtime-produced probe summary — the *shared schema* between the RT
/// producer (`pulp::audio::AudioProbe`, the audio callback) and the non-RT
/// consumers (UI / offline analysis). Producer and consumers share this data
/// type only; they NEVER share an implementation path
/// (`planning/2026-06-09-audio-observability-and-validation-harness-plan.md`,
/// Section 4, "Two layers, one schema").
///
/// `AudioProbeSnapshot` is POD-like and trivially copyable so it can ride a
/// `runtime::TripleBuffer` (latest-wins summary) without allocation. It is
/// fixed-capacity: per-channel scalar metrics live in a compile-time-sized
/// array (`kMaxChannels`). The producer fills only `channel_count` entries; a
/// `prepare()`-set capacity must be `<= kMaxChannels`.
///
/// Carries the identity tuple `{sample_rate, block_size, channel_count,
/// stage_id, sequence_number}` so a reader can detect stale or mismatched
/// data, plus explicit stale/drop/overwrite/sequence-gap accounting so a
/// silently-dropped snapshot is visible, not invisible.

#include <cstdint>
#include <type_traits>

namespace pulp::audio {

/// Identifies which audio boundary a snapshot was produced at. The first
/// wired stage is `kStandaloneOutputBoundary` (the post-`process()` output tap
/// in the standalone host). Other stages follow in later slices.
enum class AudioProbeStage : std::uint32_t {
    kUnknown = 0,
    kProcessorOutput = 1,
    kStandaloneOutputBoundary = 2,
    kMeterBridge = 3,
    kDeviceCallback = 4,
    kGraphNode = 5,
};

/// Realtime-produced, fixed-capacity, trivially-copyable probe summary.
///
/// Per-channel metrics are stored in fixed arrays sized to `kMaxChannels`.
/// Only the first `channel_count` entries are meaningful. Aggregate (across
/// active channels) scalars are also provided for cheap UI consumption.
struct AudioProbeSnapshot {
    /// Compile-time channel ceiling. The probe's runtime capacity (set at
    /// prepare()) must not exceed this. 32 covers ambisonic/surround layouts
    /// while keeping the snapshot small enough to copy through a TripleBuffer.
    static constexpr int kMaxChannels = 32;

    // ── Identity tuple (stale/mismatch detection) ──────────────────────────
    double sample_rate = 0.0;
    std::uint32_t block_size = 0;
    std::uint32_t channel_count = 0;
    AudioProbeStage stage_id = AudioProbeStage::kUnknown;
    /// Monotonic per-publish counter. A reader stores the last value it saw;
    /// a gap means snapshots were dropped between reads (TripleBuffer is
    /// latest-wins). Starts at 0; the first published snapshot is 1.
    std::uint64_t sequence_number = 0;

    // ── Per-channel scalar metrics (first `channel_count` valid) ────────────
    /// Peak absolute sample value this block, per channel (linear, >= 0).
    float peak[kMaxChannels] = {};
    /// RMS of this block, per channel (linear, >= 0).
    float rms[kMaxChannels] = {};

    // ── Aggregate scalar metrics (across active channels, this block) ───────
    /// Max peak across active channels (linear).
    float peak_max = 0.0f;
    /// Max RMS across active channels (linear).
    float rms_max = 0.0f;

    // ── Content event counters (cumulative since prepare()) ─────────────────
    /// Samples seen with |x| > clip ceiling, summed over all blocks/channels.
    std::uint64_t clip_count = 0;
    /// Samples seen as NaN or Inf, summed over all blocks/channels.
    std::uint64_t nan_inf_count = 0;
    /// Number of analyzed callbacks (blocks) since prepare().
    std::uint64_t callbacks = 0;
    /// Consecutive blocks (most recent run) below the silence threshold. Reset
    /// to 0 by the first non-silent block. Lets a UI say "silent for N blocks".
    std::uint64_t silence_run_blocks = 0;

    // ── Drop / overwrite accounting (flight-recorder honesty) ───────────────
    /// Capture frames the producer could not enqueue because the ring was full
    /// (only meaningful when last-N capture is enabled). 0 otherwise.
    std::uint64_t dropped_capture_frames = 0;
    /// Capture frames overwritten in the ring before a consumer read them
    /// (only meaningful when overwrite-on-full capture is enabled). 0 otherwise.
    std::uint64_t overwritten_capture_frames = 0;
};

static_assert(std::is_trivially_copyable_v<AudioProbeSnapshot>,
              "AudioProbeSnapshot must be trivially copyable to ride a "
              "TripleBuffer without allocation");
static_assert(std::is_trivially_destructible_v<AudioProbeSnapshot>,
              "AudioProbeSnapshot must be trivially destructible (POD-like)");

}  // namespace pulp::audio
