#pragma once

#include <pulp/host/signal_graph.hpp>

namespace pulp::host {

namespace detail {
class TimelineAutomationDelivery;
}

/// Passkey authorizing exact-generation parameter-event injection into a pinned
/// ExecutionSnapshot. Only the timeline automation delivery path can mint one,
/// so the injection entry point stays closed to ordinary hosts without granting
/// TimelineAutomationDelivery a blanket friendship over the snapshot internals.
class SnapshotParameterIngressPasskey {
    SnapshotParameterIngressPasskey() = default;
    friend class detail::TimelineAutomationDelivery;
};

/// Strong, typed handle to one exact compiled SignalGraph generation. The
/// owning SignalGraph and its worker pool must outlive the handle. Copying is
/// control-thread-only.
class SignalGraph::ExecutionSnapshot {
  public:
    ExecutionSnapshot() = default;
    explicit operator bool() const noexcept { return snapshot_ != nullptr; }

    bool inject_midi(NodeId midi_input_node,
                     const midi::MidiBuffer& events) const noexcept;
    // Generic live-mailbox injection into this pinned generation. Available to
    // any host holding the snapshot handle.
    bool inject_parameter_events(
        NodeId plugin_node,
        const state::ParameterEventQueue& events) const noexcept;
    // Exact-generation injection into a node's owner-claimed exact mailbox,
    // gated by a passkey only the timeline delivery path can construct. Separate
    // from the generic live path above so a claimed node's timeline stream and
    // ordinary live injection never share one mailbox.
    bool inject_exact_parameter_events(
        NodeId plugin_node,
        const state::ParameterEventQueue& events,
        SnapshotParameterIngressPasskey) const noexcept;
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 int num_samples) const noexcept;
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 int num_samples,
                 const format::ProcessContext& transport) const noexcept;

  private:
    friend class PreparedTopologyEdit;
    ExecutionSnapshot(SignalGraph& owner,
                      std::shared_ptr<SignalGraph::CompiledGraph> snapshot) noexcept
        : owner_(&owner), snapshot_(std::move(snapshot)) {}

    SignalGraph* owner_ = nullptr;
    std::shared_ptr<SignalGraph::CompiledGraph> snapshot_;
};

} // namespace pulp::host
