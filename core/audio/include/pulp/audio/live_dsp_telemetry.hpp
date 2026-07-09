#pragma once

/// @file live_dsp_telemetry.hpp
/// Fixed-slot per-node live-DSP timing telemetry.
///
/// Measures per-node/per-block DSP cost on the LIVE audio thread with zero
/// allocation and zero locks on that thread, drains off-thread, and publishes
/// rolling percentile/jitter summaries for UI + inspector polling.
///
/// This is deliberately NOT Perfetto: Perfetto's `TRACE_EVENT` takes a mutex at
/// ring-chunk rollover, so it cannot run on a real-time callback. The audio
/// thread here only reads a monotonic clock and writes into pre-allocated slots
/// via relaxed/acquire-release atomics — the same discipline as
/// `AudioProcessLoadMeasurer`, extended to per-node granularity. Percentiles,
/// jitter, and overload attribution are computed on the draining (non-RT) side.
///
/// Lifecycle mirrors the load-measurer / audio-probe pattern:
///   control thread : prepare(config, nodes)  — allocates everything up front
///   audio thread   : begin_block() → node scopes → finish()  — RT-safe writes
///   owner/UI thread: drain(...) then latest()  — aggregate + read a snapshot
///
/// Disabled by default; enable explicitly via set_enabled(true). When disabled
/// the audio path is a single predicted-not-taken branch.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace pulp::audio {

/// Coarse node classification, interned at prepare() time (never on the RT thread).
enum class LiveDspNodeKind : std::uint8_t {
    Unknown = 0,
    AudioInput,
    AudioOutput,
    Plugin,
    Gain,
    MidiInput,
    MidiOutput,
    Custom,
    Utility,
};

const char* to_string(LiveDspNodeKind kind) noexcept;

/// Immutable per-node identity, supplied to prepare() and copied into the store.
/// `name` is truncated into a fixed buffer so no string work happens on the RT thread.
struct LiveDspNodeInfo {
    std::uint64_t node_id = 0;
    LiveDspNodeKind kind = LiveDspNodeKind::Unknown;
    std::uint32_t input_ports = 0;
    std::uint32_t output_ports = 0;
    char name[64] = {};

    void set_name(std::string_view value) noexcept;
};

struct LiveDspTelemetryConfig {
    /// Number of block records the fixed-slot ring can hold before dropping.
    std::uint32_t ring_blocks = 256;
    /// Per-node rolling window (in blocks) over which percentiles are computed.
    std::uint32_t percentile_window_blocks = 256;
    /// A block whose graph load reaches this fraction is counted "over budget"
    /// and attributed to its most expensive node.
    float graph_overload_load = 1.0f;
};

/// Per-node rolling summary produced by drain().
struct LiveDspNodeSummary {
    std::uint64_t node_id = 0;
    LiveDspNodeKind kind = LiveDspNodeKind::Unknown;
    char name[64] = {};

    std::uint64_t sample_count = 0;   ///< blocks observed for this node
    std::int64_t last_elapsed_ns = 0;
    std::int64_t min_elapsed_ns = 0;
    std::int64_t max_elapsed_ns = 0;
    std::int64_t mean_elapsed_ns = 0;
    std::int64_t p50_elapsed_ns = 0;
    std::int64_t p95_elapsed_ns = 0;
    std::int64_t p99_elapsed_ns = 0;
    std::int64_t jitter_ns = 0;       ///< p95 - p50
    float last_budget_fraction = 0.0f;
    std::uint64_t over_budget_attributions = 0;
};

/// Latest published summary. Owned by the store; valid until the next drain().
struct LiveDspTelemetrySnapshot {
    bool available = false;
    bool enabled = false;
    std::uint64_t sequence_number = 0;

    std::uint32_t node_count = 0;
    std::uint64_t blocks_written = 0;
    std::uint64_t blocks_drained = 0;
    std::uint64_t blocks_dropped = 0;
    std::uint64_t graph_over_budget_blocks = 0;

    double sample_rate = 0.0;
    std::uint32_t last_frame_count = 0;
    std::int64_t last_available_ns = 0;
    std::int64_t last_graph_elapsed_ns = 0;
    float last_graph_load = 0.0f;

    std::vector<LiveDspNodeSummary> nodes;  ///< reused; never reallocated after prepare()
};

class LiveDspTelemetryStore;

/// RAII per-node timing scope, constructed inside begin_block()'s active block.
/// ctor stamps the start; dtor writes elapsed into the block's fixed node slot.
/// Copyable-free, no allocation, no locks.
class LiveDspTelemetryNodeScope {
public:
    LiveDspTelemetryNodeScope() noexcept = default;
    LiveDspTelemetryNodeScope(LiveDspTelemetryStore* store, std::uint32_t ring_slot,
                              std::uint32_t node_slot) noexcept;
    ~LiveDspTelemetryNodeScope() noexcept;

    LiveDspTelemetryNodeScope(const LiveDspTelemetryNodeScope&) = delete;
    LiveDspTelemetryNodeScope& operator=(const LiveDspTelemetryNodeScope&) = delete;
    LiveDspTelemetryNodeScope(LiveDspTelemetryNodeScope&& other) noexcept;
    LiveDspTelemetryNodeScope& operator=(LiveDspTelemetryNodeScope&&) = delete;

private:
    LiveDspTelemetryStore* store_ = nullptr;
    std::uint32_t ring_slot_ = 0;
    std::uint32_t node_slot_ = 0;
    std::chrono::steady_clock::time_point start_{};
};

/// Handle for one audio block. Inactive when telemetry is disabled or the ring
/// is full (drop-on-full). All work is RT-safe.
class LiveDspTelemetryBlockWriter {
public:
    LiveDspTelemetryBlockWriter() noexcept = default;
    LiveDspTelemetryBlockWriter(LiveDspTelemetryStore* store, std::uint32_t ring_slot,
                                std::uint32_t frame_count, double sample_rate) noexcept;

    LiveDspTelemetryBlockWriter(const LiveDspTelemetryBlockWriter&) = delete;
    LiveDspTelemetryBlockWriter& operator=(const LiveDspTelemetryBlockWriter&) = delete;
    LiveDspTelemetryBlockWriter(LiveDspTelemetryBlockWriter&& other) noexcept;
    LiveDspTelemetryBlockWriter& operator=(LiveDspTelemetryBlockWriter&&) = delete;
    ~LiveDspTelemetryBlockWriter() noexcept { finish(); }

    bool active() const noexcept { return store_ != nullptr; }

    /// Start timing node `node_slot` (0-based, < node_count). No-op if inactive
    /// or the slot is out of range.
    LiveDspTelemetryNodeScope node(std::uint32_t node_slot) noexcept;

    /// Publish the block. Idempotent; called by the destructor if not called
    /// explicitly. Records the total graph elapsed for the block.
    void finish() noexcept;

private:
    LiveDspTelemetryStore* store_ = nullptr;
    std::uint32_t ring_slot_ = 0;
    std::chrono::steady_clock::time_point start_{};
};

/// The store. One producer (audio thread) and one consumer (drain/owner thread).
class LiveDspTelemetryStore {
public:
    LiveDspTelemetryStore() = default;

    /// Allocate all storage for `nodes.size()` nodes and the ring. Control
    /// thread only. Returns false on an empty node set or invalid config.
    /// Re-preparing resets all state.
    bool prepare(const LiveDspTelemetryConfig& config, std::span<const LiveDspNodeInfo> nodes);

    void set_enabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    std::uint32_t node_count() const noexcept { return node_count_; }
    bool prepared() const noexcept { return node_count_ != 0; }

    /// AUDIO THREAD. Begin a block. Returns an inactive writer if disabled,
    /// not prepared, or the ring is full (increments blocks_dropped).
    LiveDspTelemetryBlockWriter begin_block(std::uint32_t frame_count, double sample_rate) noexcept;

    /// AUDIO THREAD (or replay). Feed a complete block record with explicit
    /// per-node elapsed times, taking the same ring path as begin_block() +
    /// node scopes + finish(). RT-safe: no allocation, no locks, drop-on-full.
    /// Entries in `node_ns` beyond node_count() are ignored; missing nodes
    /// default to 0. Used both for path-agnostic recording from externally
    /// measured per-node timings (e.g. the graph's persistent load measurers,
    /// which cover every execution path) and for deterministic tests / replay.
    void inject_block(std::span<const std::int64_t> node_ns, std::int64_t graph_elapsed_ns,
                      std::uint32_t frame_count, double sample_rate) noexcept;

    /// AUDIO THREAD. A pre-allocated node_count()-sized scratch for the caller to
    /// fill with per-node elapsed values before calling inject_block() over it —
    /// so a recording site needs no stack VLA and no allocation. Returns nullptr
    /// when not prepared. The buffer is owned by the store and reused each block.
    std::int64_t* external_record_scratch() noexcept {
        return record_scratch_.empty() ? nullptr : record_scratch_.data();
    }

    /// NON-RT. Consume all complete block records, update rolling windows,
    /// compute percentiles/jitter/attribution, and republish the snapshot.
    void drain() noexcept;

    /// NON-RT, single reader. The latest published snapshot.
    const LiveDspTelemetrySnapshot& latest() const noexcept { return snapshot_; }

    std::uint64_t blocks_written() const noexcept { return write_index_.load(std::memory_order_acquire); }
    std::uint64_t blocks_dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    friend class LiveDspTelemetryBlockWriter;
    friend class LiveDspTelemetryNodeScope;

    // Audio-thread write helpers (called by the writer/scope friends).
    void write_node_elapsed(std::uint32_t ring_slot, std::uint32_t node_slot,
                            std::int64_t elapsed_ns) noexcept;
    void publish_block(std::uint32_t ring_slot, std::int64_t graph_elapsed_ns) noexcept;

    struct BlockHeader {
        std::uint32_t frame_count = 0;
        double sample_rate = 0.0;
        std::int64_t available_ns = 0;
        std::int64_t graph_elapsed_ns = 0;
    };

    std::uint32_t node_count_ = 0;
    std::uint32_t ring_blocks_ = 0;
    std::uint32_t window_blocks_ = 0;
    float graph_overload_load_ = 1.0f;

    std::atomic<bool> enabled_{false};

    // SPSC ring: producer = audio thread, consumer = drain(). write_/read_index_
    // are monotonic; slot = index % ring_blocks_.
    std::atomic<std::uint64_t> write_index_{0};
    std::atomic<std::uint64_t> read_index_{0};
    std::atomic<std::uint64_t> dropped_{0};

    std::vector<BlockHeader> ring_headers_;   // [ring_blocks_]
    std::vector<std::int64_t> ring_node_ns_;  // [ring_blocks_ * node_count_]

    // Drain-side rolling windows (one ring of the last window_blocks_ samples per node).
    std::vector<std::int64_t> window_ns_;     // [node_count_ * window_blocks_]
    std::vector<std::uint32_t> window_pos_;   // [node_count_] next write pos
    std::vector<std::uint32_t> window_len_;   // [node_count_] filled length
    std::vector<std::int64_t> scratch_;       // [window_blocks_] percentile scratch
    std::vector<std::int64_t> record_scratch_;  // [node_count_] external-record staging

    std::vector<LiveDspNodeInfo> info_;       // [node_count_]
    std::uint64_t drained_ = 0;
    std::uint64_t over_budget_blocks_ = 0;
    LiveDspTelemetrySnapshot snapshot_;
};

}  // namespace pulp::audio
