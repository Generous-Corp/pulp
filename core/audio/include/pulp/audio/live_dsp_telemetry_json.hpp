#pragma once

/// @file live_dsp_telemetry_json.hpp
/// Snapshot → JSON serializer for the live per-node DSP telemetry surface.
///
/// The live-DSP telemetry store (`pulp::audio::LiveDspTelemetryStore`) publishes
/// a `LiveDspTelemetrySnapshot` that the GPU Audio Inspector paints as an
/// overlay. Agents and CI have no window — they need the same scalar facts as
/// text. This helper turns a snapshot into a stable, flat JSON object so a
/// headless caller can dump the live-DSP state after a run.
///
/// Pure and allocation-tolerant: it runs on the NON-RT side (after `drain()` has
/// published), never on the audio thread. Factored out of the inspector so it
/// can be unit-tested without a GPU surface or an audio device.
///
/// Time convention: all `*_ns` fields are nanoseconds (integers). Percentiles
/// (`p50`/`p95`/`p99`) and `jitter_ns` (p95 − p50) come straight from the
/// snapshot's rolling window. `graph_load` is a 0..1 fraction of the block
/// budget; it can exceed 1.0 when the graph overruns.

#include <pulp/audio/live_dsp_telemetry.hpp>

#include <string>

namespace pulp::audio {

/// Serialize a live-DSP telemetry snapshot to a flat JSON object string.
/// `pretty` controls indentation (default on, matching the offline Audio Doctor
/// and audio-probe artifacts). The object always carries the graph-level fields:
///   available, enabled, sequence_number, node_count, blocks_written,
///   blocks_drained, blocks_dropped, graph_over_budget_blocks, sample_rate,
///   last_frame_count, last_available_ns, last_graph_elapsed_ns, last_graph_load,
/// plus a `nodes` array. Each node object carries:
///   node_id, kind, name, sample_count, last_elapsed_ns, min_elapsed_ns,
///   max_elapsed_ns, mean_elapsed_ns, p50_elapsed_ns, p95_elapsed_ns,
///   p99_elapsed_ns, jitter_ns, last_budget_fraction, over_budget_attributions.
/// `kind` is the stable `to_string(LiveDspNodeKind)` name.
std::string live_dsp_telemetry_snapshot_to_json(const LiveDspTelemetrySnapshot& snapshot,
                                                bool pretty = true);

}  // namespace pulp::audio
