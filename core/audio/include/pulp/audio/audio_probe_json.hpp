#pragma once

/// @file audio_probe_json.hpp
/// Snapshot → JSON serializer for the live Audio Inspector's programmatic dump.
///
/// The live Audio Inspector (the floating dev window in the standalone host)
/// observes `pulp::audio::AudioProbe`. For agents and CI, a visible window is
/// useless — they need the scalar facts as text. This helper turns an
/// `AudioProbeSnapshot` (plus the optional `AudioStats` counter subset) into a
/// flat JSON object so `pulp run --audio-probe-json PATH` can write the probe
/// state after a headless one-shot and exit.
///
/// Pure and allocation-tolerant: it runs on the NON-RT side (after the audio
/// callback has published), never on the audio thread. Factored out of the
/// standalone host so it can be unit-tested without opening an audio device.
///
/// dBFS convention: `peak_dbfs` / `rms_dbfs` are `20*log10(linear)`. A linear
/// value of 0 maps to `-inf` (JSON `null`, since JSON has no infinity literal),
/// so a reader can distinguish "true silence" from a finite low level.

#include <pulp/audio/audio_probe_snapshot.hpp>
#include <pulp/audio/audio_stats.hpp>

#include <string>
#include <string_view>

namespace pulp::audio {

/// Stable string name for a probe stage, used as the JSON `stage` field.
std::string_view audio_probe_stage_name(AudioProbeStage stage);

/// Serialize a probe snapshot (and the release-safe counter subset) to a flat
/// JSON object string. `pretty` controls indentation (default on, matching the
/// offline Audio Doctor artifacts). The object always carries:
///   stage, sample_rate, block_size, channel_count, sequence_number,
///   peak_max, rms_max, peak_dbfs, rms_dbfs, clip_count, nan_inf_count,
///   clipped_blocks, nan_blocks, silence_run_blocks, callbacks, underruns,
///   device_xruns, cpu_overloads.
/// `peak_dbfs` / `rms_dbfs` are `null` when the corresponding linear value is 0.
std::string audio_probe_snapshot_to_json(const AudioProbeSnapshot& snapshot,
                                         const AudioStats& stats,
                                         bool pretty = true);

}  // namespace pulp::audio
