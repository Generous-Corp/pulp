#include "pulp/audio/live_dsp_telemetry.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>

namespace pulp::audio {

namespace {

/// Nanoseconds of budget for `frame_count` frames at `sample_rate`. Mirrors the
/// arithmetic in AudioProcessLoadMeasurer::begin so the two agree on "load".
std::int64_t budget_ns(std::uint32_t frame_count, double sample_rate) noexcept {
    if (frame_count == 0 || !(sample_rate > 0.0)) {
        return 0;
    }
    const double ns =
        static_cast<double>(frame_count) / sample_rate * 1e9;
    if (!(ns > 0.0) ||
        ns > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return 0;
    }
    return static_cast<std::int64_t>(ns);
}

std::int64_t percentile_at(const std::int64_t* sorted, std::uint32_t len, double q) noexcept {
    if (len == 0) {
        return 0;
    }
    if (len == 1) {
        return sorted[0];
    }
    double pos = q * static_cast<double>(len - 1);
    if (pos < 0.0) {
        pos = 0.0;
    }
    auto idx = static_cast<std::uint32_t>(pos);
    if (idx >= len - 1) {
        return sorted[len - 1];
    }
    // Nearest-rank with linear interpolation between neighbours.
    const double frac = pos - static_cast<double>(idx);
    const double lo = static_cast<double>(sorted[idx]);
    const double hi = static_cast<double>(sorted[idx + 1]);
    return static_cast<std::int64_t>(lo + (hi - lo) * frac);
}

}  // namespace

const char* to_string(LiveDspNodeKind kind) noexcept {
    switch (kind) {
        case LiveDspNodeKind::Unknown:     return "unknown";
        case LiveDspNodeKind::AudioInput:  return "audio-input";
        case LiveDspNodeKind::AudioOutput: return "audio-output";
        case LiveDspNodeKind::Plugin:      return "plugin";
        case LiveDspNodeKind::Gain:        return "gain";
        case LiveDspNodeKind::MidiInput:   return "midi-input";
        case LiveDspNodeKind::MidiOutput:  return "midi-output";
        case LiveDspNodeKind::Custom:      return "custom";
        case LiveDspNodeKind::Utility:     return "utility";
    }
    return "unknown";
}

void LiveDspNodeInfo::set_name(std::string_view value) noexcept {
    const std::size_t n = std::min(value.size(), sizeof(name) - 1);
    std::memcpy(name, value.data(), n);
    name[n] = '\0';
    for (std::size_t i = n + 1; i < sizeof(name); ++i) {
        name[i] = '\0';
    }
}

// ---------------------------------------------------------------------------
// LiveDspTelemetryNodeScope
// ---------------------------------------------------------------------------

LiveDspTelemetryNodeScope::LiveDspTelemetryNodeScope(LiveDspTelemetryStore* store,
                                                     std::uint32_t ring_slot,
                                                     std::uint32_t node_slot) noexcept
    : store_(store), ring_slot_(ring_slot), node_slot_(node_slot),
      start_(std::chrono::steady_clock::now()) {}

LiveDspTelemetryNodeScope::LiveDspTelemetryNodeScope(LiveDspTelemetryNodeScope&& other) noexcept
    : store_(other.store_), ring_slot_(other.ring_slot_), node_slot_(other.node_slot_),
      start_(other.start_) {
    other.store_ = nullptr;
}

LiveDspTelemetryNodeScope::~LiveDspTelemetryNodeScope() noexcept {
    if (store_ == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start_;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    store_->write_node_elapsed(ring_slot_, node_slot_, ns);
    store_ = nullptr;
}

// ---------------------------------------------------------------------------
// LiveDspTelemetryBlockWriter
// ---------------------------------------------------------------------------

LiveDspTelemetryBlockWriter::LiveDspTelemetryBlockWriter(LiveDspTelemetryStore* store,
                                                         std::uint32_t ring_slot,
                                                         std::uint32_t /*frame_count*/,
                                                         double /*sample_rate*/) noexcept
    : store_(store), ring_slot_(ring_slot), start_(std::chrono::steady_clock::now()) {}

LiveDspTelemetryBlockWriter::LiveDspTelemetryBlockWriter(LiveDspTelemetryBlockWriter&& other) noexcept
    : store_(other.store_), ring_slot_(other.ring_slot_), start_(other.start_) {
    other.store_ = nullptr;
}

LiveDspTelemetryNodeScope LiveDspTelemetryBlockWriter::node(std::uint32_t node_slot) noexcept {
    if (store_ == nullptr || node_slot >= store_->node_count()) {
        return LiveDspTelemetryNodeScope{};
    }
    return LiveDspTelemetryNodeScope{store_, ring_slot_, node_slot};
}

void LiveDspTelemetryBlockWriter::finish() noexcept {
    if (store_ == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start_;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    store_->publish_block(ring_slot_, ns);
    store_ = nullptr;
}

// ---------------------------------------------------------------------------
// LiveDspTelemetryStore
// ---------------------------------------------------------------------------

bool LiveDspTelemetryStore::prepare(const LiveDspTelemetryConfig& config,
                                    std::span<const LiveDspNodeInfo> nodes) {
    if (nodes.empty()) {
        return false;
    }

    node_count_ = static_cast<std::uint32_t>(nodes.size());
    ring_blocks_ = config.ring_blocks > 0 ? config.ring_blocks : 1;
    window_blocks_ = config.percentile_window_blocks > 0 ? config.percentile_window_blocks : 1;
    graph_overload_load_ =
        config.graph_overload_load > 0.0f ? config.graph_overload_load : 1.0f;

    write_index_.store(0, std::memory_order_relaxed);
    read_index_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    drained_ = 0;
    over_budget_blocks_ = 0;

    ring_headers_.assign(ring_blocks_, BlockHeader{});
    ring_node_ns_.assign(static_cast<std::size_t>(ring_blocks_) * node_count_, 0);

    window_ns_.assign(static_cast<std::size_t>(node_count_) * window_blocks_, 0);
    window_pos_.assign(node_count_, 0);
    window_len_.assign(node_count_, 0);
    scratch_.assign(window_blocks_, 0);
    record_scratch_.assign(node_count_, 0);

    info_.assign(nodes.begin(), nodes.end());

    snapshot_ = LiveDspTelemetrySnapshot{};
    snapshot_.node_count = node_count_;
    snapshot_.nodes.assign(node_count_, LiveDspNodeSummary{});
    for (std::uint32_t j = 0; j < node_count_; ++j) {
        auto& s = snapshot_.nodes[j];
        s.node_id = info_[j].node_id;
        s.kind = info_[j].kind;
        std::memcpy(s.name, info_[j].name, sizeof(s.name));
        s.min_elapsed_ns = std::numeric_limits<std::int64_t>::max();
    }
    return true;
}

LiveDspTelemetryBlockWriter LiveDspTelemetryStore::begin_block(std::uint32_t frame_count,
                                                               double sample_rate) noexcept {
    if (!enabled_.load(std::memory_order_relaxed) || node_count_ == 0) {
        return LiveDspTelemetryBlockWriter{};
    }

    const std::uint64_t w = write_index_.load(std::memory_order_relaxed);
    const std::uint64_t r = read_index_.load(std::memory_order_acquire);
    if (w - r >= ring_blocks_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return LiveDspTelemetryBlockWriter{};
    }

    const std::uint32_t slot = static_cast<std::uint32_t>(w % ring_blocks_);
    BlockHeader& header = ring_headers_[slot];
    header.frame_count = frame_count;
    header.sample_rate = sample_rate;
    header.available_ns = budget_ns(frame_count, sample_rate);
    header.graph_elapsed_ns = 0;

    // Reset this slot's per-node timings so a node that is skipped this block
    // reads zero rather than a stale value from a prior wrap.
    std::int64_t* base = ring_node_ns_.data() + static_cast<std::size_t>(slot) * node_count_;
    for (std::uint32_t j = 0; j < node_count_; ++j) {
        base[j] = 0;
    }

    return LiveDspTelemetryBlockWriter{this, slot, frame_count, sample_rate};
}

void LiveDspTelemetryStore::inject_block(std::span<const std::int64_t> node_ns,
                                         std::int64_t graph_elapsed_ns,
                                         std::uint32_t frame_count,
                                         double sample_rate) noexcept {
    if (!enabled_.load(std::memory_order_relaxed) || node_count_ == 0) {
        return;
    }

    const std::uint64_t w = write_index_.load(std::memory_order_relaxed);
    const std::uint64_t r = read_index_.load(std::memory_order_acquire);
    if (w - r >= ring_blocks_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const std::uint32_t slot = static_cast<std::uint32_t>(w % ring_blocks_);
    BlockHeader& header = ring_headers_[slot];
    header.frame_count = frame_count;
    header.sample_rate = sample_rate;
    header.available_ns = budget_ns(frame_count, sample_rate);
    header.graph_elapsed_ns = graph_elapsed_ns;

    std::int64_t* base = ring_node_ns_.data() + static_cast<std::size_t>(slot) * node_count_;
    const std::uint32_t supplied =
        static_cast<std::uint32_t>(std::min<std::size_t>(node_ns.size(), node_count_));
    for (std::uint32_t j = 0; j < supplied; ++j) {
        base[j] = node_ns[j];
    }
    for (std::uint32_t j = supplied; j < node_count_; ++j) {
        base[j] = 0;
    }

    write_index_.fetch_add(1, std::memory_order_release);
}

void LiveDspTelemetryStore::write_node_elapsed(std::uint32_t ring_slot, std::uint32_t node_slot,
                                               std::int64_t elapsed_ns) noexcept {
    // Single producer; consumer reads only after the release-store of
    // write_index_ in publish_block, so a plain store is race-free.
    ring_node_ns_[static_cast<std::size_t>(ring_slot) * node_count_ + node_slot] = elapsed_ns;
}

void LiveDspTelemetryStore::publish_block(std::uint32_t ring_slot,
                                          std::int64_t graph_elapsed_ns) noexcept {
    ring_headers_[ring_slot].graph_elapsed_ns = graph_elapsed_ns;
    write_index_.fetch_add(1, std::memory_order_release);
}

void LiveDspTelemetryStore::drain() noexcept {
    if (node_count_ == 0) {
        return;
    }

    const std::uint64_t r = read_index_.load(std::memory_order_relaxed);
    const std::uint64_t w = write_index_.load(std::memory_order_acquire);
    const std::uint64_t count = w - r;

    if (count > 0) {
        for (std::uint64_t i = r; i < w; ++i) {
            const std::uint32_t slot = static_cast<std::uint32_t>(i % ring_blocks_);
            const BlockHeader& header = ring_headers_[slot];
            const std::int64_t* ns =
                ring_node_ns_.data() + static_cast<std::size_t>(slot) * node_count_;

            std::int64_t worst_ns = -1;
            std::uint32_t worst_node = 0;
            for (std::uint32_t j = 0; j < node_count_; ++j) {
                const std::int64_t e = ns[j];
                // Push into the node's rolling window.
                const std::size_t wbase = static_cast<std::size_t>(j) * window_blocks_;
                window_ns_[wbase + window_pos_[j]] = e;
                window_pos_[j] = (window_pos_[j] + 1) % window_blocks_;
                if (window_len_[j] < window_blocks_) {
                    ++window_len_[j];
                }

                auto& s = snapshot_.nodes[j];
                s.last_elapsed_ns = e;
                s.last_budget_fraction =
                    header.available_ns > 0
                        ? static_cast<float>(static_cast<double>(e) /
                                             static_cast<double>(header.available_ns))
                        : 0.0f;

                if (e > worst_ns) {
                    worst_ns = e;
                    worst_node = j;
                }
            }

            // Graph-level metrics + over-budget attribution.
            const float graph_load =
                header.available_ns > 0
                    ? static_cast<float>(static_cast<double>(header.graph_elapsed_ns) /
                                         static_cast<double>(header.available_ns))
                    : 0.0f;
            snapshot_.last_frame_count = header.frame_count;
            snapshot_.sample_rate = header.sample_rate;
            snapshot_.last_available_ns = header.available_ns;
            snapshot_.last_graph_elapsed_ns = header.graph_elapsed_ns;
            snapshot_.last_graph_load = graph_load;

            if (graph_load >= graph_overload_load_ && worst_ns >= 0) {
                ++over_budget_blocks_;
                ++snapshot_.nodes[worst_node].over_budget_attributions;
            }
        }

        read_index_.store(w, std::memory_order_release);
        drained_ += count;
    }

    // Recompute per-node summaries from the rolling windows (non-RT work).
    for (std::uint32_t j = 0; j < node_count_; ++j) {
        auto& s = snapshot_.nodes[j];
        const std::uint32_t len = window_len_[j];
        s.sample_count = len;
        if (len == 0) {
            s.min_elapsed_ns = 0;
            s.max_elapsed_ns = 0;
            s.mean_elapsed_ns = 0;
            s.p50_elapsed_ns = 0;
            s.p95_elapsed_ns = 0;
            s.p99_elapsed_ns = 0;
            s.jitter_ns = 0;
            continue;
        }

        const std::size_t wbase = static_cast<std::size_t>(j) * window_blocks_;
        std::int64_t* buf = scratch_.data();
        std::copy(window_ns_.begin() + static_cast<std::ptrdiff_t>(wbase),
                  window_ns_.begin() + static_cast<std::ptrdiff_t>(wbase + len), buf);
        std::sort(buf, buf + len);

        s.min_elapsed_ns = buf[0];
        s.max_elapsed_ns = buf[len - 1];
        const std::int64_t sum = std::accumulate(buf, buf + len, std::int64_t{0});
        s.mean_elapsed_ns = sum / static_cast<std::int64_t>(len);
        s.p50_elapsed_ns = percentile_at(buf, len, 0.50);
        s.p95_elapsed_ns = percentile_at(buf, len, 0.95);
        s.p99_elapsed_ns = percentile_at(buf, len, 0.99);
        s.jitter_ns = s.p95_elapsed_ns - s.p50_elapsed_ns;
    }

    snapshot_.available = true;
    snapshot_.enabled = enabled_.load(std::memory_order_relaxed);
    snapshot_.blocks_written = w;
    snapshot_.blocks_drained = drained_;
    snapshot_.blocks_dropped = dropped_.load(std::memory_order_relaxed);
    snapshot_.graph_over_budget_blocks = over_budget_blocks_;
    ++snapshot_.sequence_number;
}

}  // namespace pulp::audio
