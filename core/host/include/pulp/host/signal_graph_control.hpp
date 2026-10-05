#pragma once

#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace pulp::host {

/// One typed mutation accepted by the host's canonical graph authority.
/// Modulation routes are edited through PreparedTopologyEdit, so a rejected
/// batch leaves the live snapshot untouched.
struct SignalGraphRouteCommand {
    enum class Kind : std::uint8_t { Insert, Rewire, Remove };
    Kind kind = Kind::Insert;
    bool audio_rate = false;
    NodeId source = 0;
    PortIndex source_port = 0;
    NodeId destination = 0;
    std::uint32_t parameter_id = 0;
    float range_lo = 0.0f;
    float range_hi = 1.0f;
    float smoothing_ms = 0.0f;
    AutomationMix mix = AutomationMix::Replace;
    NodeId previous_source = 0;
    PortIndex previous_source_port = 0;
};

enum class SignalGraphRouteResultCode : std::uint8_t {
    Applied,
    EmptyBatch,
    DenseQueueOverflow,
    InvalidCommand,
    PrepareFailed,
    CommitFailed,
};

struct SignalGraphRouteResult {
    SignalGraphRouteResultCode code = SignalGraphRouteResultCode::InvalidCommand;
    std::size_t applied = 0;
    std::size_t capacity = 0;
    std::uint64_t generation = 0;
    SignalGraph::PreparedTopologyEdit::Result topology_result =
        SignalGraph::PreparedTopologyEdit::Result::NotPrepared;

    explicit operator bool() const noexcept {
        return code == SignalGraphRouteResultCode::Applied;
    }
};

/// The sole host-side authority for live graph modulation-route lifecycle.
/// It owns no second runtime: each batch is compiled and published by the
/// graph's existing PreparedTopologyEdit transaction.
class SignalGraphControlAuthority final {
  public:
    static constexpr std::size_t kMaxDenseCommands = 64;

    SignalGraphControlAuthority(SignalGraph& graph, double sample_rate,
                                int maximum_block_size) noexcept;

    SignalGraphRouteResult apply(std::span<const SignalGraphRouteCommand> commands);
    void reset() noexcept;
    std::uint64_t generation() const noexcept {
        return generation_;
    }

  private:
    SignalGraph& graph_;
    double sample_rate_ = 0.0;
    int maximum_block_size_ = 0;
    std::uint64_t generation_ = 0;
};

} // namespace pulp::host
