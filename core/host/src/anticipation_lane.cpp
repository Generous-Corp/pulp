#include <pulp/host/anticipation_lane.hpp>

#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>

#include <cstdint>

namespace pulp::host {

bool AnticipationLane::prepare(
    const AnticipationSubgraph& sub,
    const std::function<std::atomic<float>*(NodeId)>& gain_for,
    const std::function<PluginSlot*(NodeId)>& plugin_for, double sample_rate,
    int block_frames, int lead_blocks,
    const std::function<ParameterEventInjectionBinding(NodeId)>&
        parameter_events_for) {
    // Reset to a clean not-prepared state so a mid-way failure can't leave the
    // lane half-configured (every method gates on prepared_, but keep it tidy for
    // a re-prepare).
    prepared_ = false;
    out_channels_ = 0;
    block_frames_ = 0;
    if (!sub.ok || sub.outputs.empty() || block_frames <= 0 || lead_blocks < 1) {
        return false;
    }
    if (!build_executor_snapshot(sub.nodes, sub.connections, gain_for, plugin_for,
                                 plugin_ctx_, scratch_, snapshot_)) {
        return false;
    }
    if (parameter_events_for) {
        for (auto& context : plugin_ctx_) {
            const auto injection = parameter_events_for(context.node_id);
            context.parameter_events_user_data = injection.user_data;
            context.append_parameter_events = injection.append;
            context.parameter_events_sequence_seen = injection.sequence_seen;
        }
    }
    if (!pool_.reset(snapshot_.buffer_slot_count(),
                     static_cast<std::uint32_t>(block_frames),
                     snapshot_.buffer_assignment().connection_delay_samples)) {
        return false;
    }

    const std::size_t channels = sub.outputs.size();
    if (!ring_.prepare(static_cast<std::uint32_t>(channels),
                       static_cast<std::uint64_t>(lead_blocks) *
                           static_cast<std::uint64_t>(block_frames))) {
        return false;
    }

    // Producer scratch: a zeroed two-channel main input the interior ignores (the
    // partition guarantees the interior has no live boundary input), and the
    // boundary-channel capture the executor writes before each block goes to the
    // ring.
    prod_in_.assign(2, std::vector<float>(static_cast<std::size_t>(block_frames), 0.0f));
    prod_out_.assign(channels,
                     std::vector<float>(static_cast<std::size_t>(block_frames), 0.0f));
    prod_in_ptrs_.clear();
    prod_out_ptrs_.clear();
    for (auto& c : prod_in_) prod_in_ptrs_.push_back(c.data());
    for (auto& c : prod_out_) prod_out_ptrs_.push_back(c.data());

    sample_rate_ = sample_rate;
    block_frames_ = block_frames;
    out_channels_ = channels;
    prepared_ = true;
    return true;
}

bool AnticipationLane::render_one_block() {
    const auto frames = static_cast<std::uint32_t>(block_frames_);
    pulp::audio::BufferView<const float> iv(prod_in_ptrs_.data(), prod_in_ptrs_.size(),
                                            frames);
    pulp::audio::BufferView<float> ov(prod_out_ptrs_.data(), prod_out_ptrs_.size(),
                                      frames);
    pulp::format::BusBufferSet buses;
    if (!buses.add_input("main", iv, pulp::format::BusRole::Main)) return false;
    if (!buses.add_output("main", ov, pulp::format::BusRole::Main)) return false;
    pulp::format::ProcessBlock block;
    block.sample_rate = sample_rate_;
    block.frame_count = frames;
    block.buses = &buses;
    if (!block.validate()) return false;
    reset_plugin_parameter_event_sequences(plugin_ctx_);
    if (!exec_.process_routed(block, snapshot_, pool_).ok()) return false;
    commit_plugin_parameter_event_sequences(plugin_ctx_);
    return ring_.write(ov, block_frames_) == static_cast<std::uint64_t>(block_frames_);
}

int AnticipationLane::render_ahead(int max_blocks) {
    if (!prepared_ || max_blocks <= 0) return 0;
    int rendered = 0;
    const auto f = static_cast<std::uint64_t>(block_frames_);
    while (rendered < max_blocks &&
           ring_.available_frames() + f <= ring_.capacity_frames()) {
        if (!render_one_block()) break;
        ++rendered;
    }
    return rendered;
}

bool AnticipationLane::consume(pulp::audio::BufferView<float>& out) noexcept {
    if (!prepared_ || out.num_channels() != out_channels_ ||
        out.num_samples() < static_cast<std::size_t>(block_frames_)) {
        return false;
    }
    if (ring_.available_frames() < static_cast<std::uint64_t>(block_frames_)) {
        return false;  // underrun
    }
    return ring_.read(out, static_cast<std::uint64_t>(block_frames_));
}

} // namespace pulp::host
