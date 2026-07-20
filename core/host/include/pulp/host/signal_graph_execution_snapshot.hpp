#pragma once

#include <pulp/host/signal_graph.hpp>

namespace pulp::host {

namespace detail {
class TimelineAutomationDelivery;
}

/// Strong, typed handle to one exact compiled SignalGraph generation. The
/// owning SignalGraph and its worker pool must outlive the handle. Copying is
/// control-thread-only.
class SignalGraph::ExecutionSnapshot {
  public:
    ExecutionSnapshot() = default;
    explicit operator bool() const noexcept { return snapshot_ != nullptr; }

    bool inject_midi(NodeId midi_input_node,
                     const midi::MidiBuffer& events) const noexcept;
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 int num_samples) const noexcept;
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 int num_samples,
                 const format::ProcessContext& transport) const noexcept;

  private:
    friend class PreparedTopologyEdit;
    friend class detail::TimelineAutomationDelivery;
    bool inject_parameter_events(
        NodeId plugin_node,
        const state::ParameterEventQueue& events) const noexcept;
    ExecutionSnapshot(SignalGraph& owner,
                      std::shared_ptr<SignalGraph::CompiledGraph> snapshot) noexcept
        : owner_(&owner), snapshot_(std::move(snapshot)) {}

    SignalGraph* owner_ = nullptr;
    std::shared_ptr<SignalGraph::CompiledGraph> snapshot_;
};

} // namespace pulp::host
