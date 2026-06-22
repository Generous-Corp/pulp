#pragma once

/// @file motion_cost.hpp
/// Motion cost attribution — correlates render-pass cost and dirty
/// area with the motion traces that were emitting samples on the
/// same frame. Off by default. When enabled, the `CostAttributor`
/// subscribes to the bound FrameClock and the Coordinator's sink
/// fan-out, snapshots which trace_ids emitted any event during a
/// frame, and emits one `CostSample` per frame.
///
/// Render-pass and dirty-rect stats come from `RenderPassManager` /
/// `DirtyTracker` via a caller-supplied `CostProbe`. Both are thin
/// subsystems; the probe is intentionally defensive — when a probe
/// isn't wired the cost stream still produces samples with zeroed
/// render fields so trace_id attribution remains useful on its own.
///
/// The cost stream is a separate JSONL file (`*.motion-cost.jsonl`)
/// with its own version header. It does NOT extend the
/// `kFixtureSchemaVersion` event stream.

#include <pulp/view/motion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulp::view::motion {

// ── CostSample ───────────────────────────────────────────────────────

constexpr int kCostSchemaVersion = 1;

/// Per-frame cost attribution sample. Emitted by CostAttributor on
/// each FrameClock tick while cost attribution is enabled. A frame
/// with no active traces still emits a sample so the timeline of
/// render cost is contiguous.
struct CostSample {
    std::uint64_t frame = 0;
    double t_seconds = 0.0;

    /// Wall time spent in the most recently completed render pass.
    /// Snapshotted from `RenderPassManager::total_time_ms()` when a
    /// probe is wired; 0 otherwise. Defensive: callers should not
    /// assume Pulp tracks this on every platform.
    double render_pass_duration_ms = 0.0;

    /// Total area (in pixels^2) of all dirty rectangles snapshotted
    /// from `DirtyTracker::dirty_rects()`. 0 when no probe is wired.
    double dirty_rect_area_px = 0.0;

    /// Number of dirty rectangles snapshotted this frame. 0 when no
    /// probe is wired or no rectangles are pending.
    int dirty_rect_count = 0;

    /// Trace ids that emitted at least one SampleEvent on this
    /// frame. Sorted ascending. Empty when no motion is active.
    std::vector<int> active_trace_ids;

    /// Provenance envelopes for each entry in `active_trace_ids`,
    /// in the same order. Entries without
    /// provenance carry a default-constructed Provenance.
    std::vector<Provenance> active_provenance;
};

// ── CostProbe ────────────────────────────────────────────────────────

/// Snapshot of render-layer cost for the current frame. Filled by a
/// caller-supplied probe. The CostAttributor calls the probe under
/// no lock — implementations should be cheap and avoid blocking.
struct RenderCostSnapshot {
    double render_pass_duration_ms = 0.0;
    double dirty_rect_area_px = 0.0;
    int dirty_rect_count = 0;
};

using CostProbe = std::function<RenderCostSnapshot()>;

// ── CostAttributor ───────────────────────────────────────────────────

/// Process-wide cost attribution stream. Off by default. When
/// enabled, subscribes to the Coordinator's bound FrameClock, joins
/// per-frame trace-id activity with the supplied probe's render
/// stats, and emits a CostSample to each registered cost sink.
///
/// Cost attribution is independent of `Coordinator::tracing_enabled`
/// — the attributor reads the same trace registry to populate
/// `active_provenance`, but its sink fan-out lives on its own
/// channel so cost samples never appear in the event fixture
/// stream.
class CostAttributor {
public:
    using CostSink = std::function<void(const CostSample&)>;

    static CostAttributor& instance();

    /// Enable / disable cost emission. Off by default.
    void set_enabled(bool on);
    bool enabled() const noexcept;

    /// Replace the active render-cost probe. Defensive: when nullptr
    /// (the default) every CostSample reports zeroed render fields.
    void set_probe(CostProbe probe);

    /// Add a sink that receives one CostSample per frame while
    /// enabled. Returns an id for later removal.
    int add_sink(CostSink sink);
    void remove_sink(int sink_id);
    void clear_sinks();

    /// Reset all state (sinks, probe, enabled flag, frame counter).
    /// For tests.
    void reset();

    /// Cumulative count of CostSamples emitted since `reset()`. For
    /// tests / diagnostics.
    std::size_t emitted_sample_count() const noexcept;

private:
    CostAttributor();
    ~CostAttributor();
    CostAttributor(const CostAttributor&) = delete;
    CostAttributor& operator=(const CostAttributor&) = delete;

    friend class Coordinator;

    /// Called by Coordinator at the top of `on_tick` with the frame
    /// number and elapsed time of the tick about to run, and at the
    /// bottom with the trace_ids that emitted during the tick.
    void note_tick_begin(std::uint64_t frame, double t_seconds);
    void note_trace_activity(int trace_id);
    void note_provenance(int trace_id, const Provenance& prov);
    void emit_frame();

    struct State;
    std::unique_ptr<State> state_;
};

// ── JSONL cost sink ──────────────────────────────────────────────────

/// Sink that appends each `CostSample` as a JSONL line to `path`.
/// Opens (truncates) the file on first sample, writing a one-line
/// header `{"motion_cost_version":1}` then a JSONL body. Closes
/// implicitly when the returned CostSink is dropped.
CostAttributor::CostSink make_cost_sink(std::string path);

/// Sink that appends each `CostSample` to a caller-owned buffer.
/// For tests / in-process consumers; the buffer pointer must
/// outlive the sink.
CostAttributor::CostSink make_cost_buffer_sink(std::vector<CostSample>* buffer);

/// Serialize one CostSample as a single JSON line (without trailing
/// newline). Exposed for tests and the inspector domain.
std::string serialize_cost_sample(const CostSample& s);

/// Parse a JSONL CostSample stream from `path`. Returns the body
/// events in file order. Empty vector on missing / unreadable /
/// unknown-version files.
std::vector<CostSample> load_cost_stream(const std::string& path);

} // namespace pulp::view::motion
