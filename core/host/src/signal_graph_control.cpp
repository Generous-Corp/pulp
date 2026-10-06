#include <pulp/host/signal_graph_control.hpp>

#include <algorithm>

namespace pulp::host {

SignalGraphControlAuthority::SignalGraphControlAuthority(SignalGraph& graph, double sample_rate,
                                                         int maximum_block_size) noexcept
    : graph_(graph), sample_rate_(sample_rate), maximum_block_size_(maximum_block_size) {}

SignalGraphRouteResult
SignalGraphControlAuthority::apply(std::span<const SignalGraphRouteCommand> commands) {
    SignalGraphRouteResult result;
    result.capacity = kMaxDenseCommands;
    result.generation = generation_;
    if (commands.empty()) {
        result.code = SignalGraphRouteResultCode::EmptyBatch;
        return result;
    }
    if (commands.size() > kMaxDenseCommands) {
        result.code = SignalGraphRouteResultCode::DenseQueueOverflow;
        return result;
    }
    if (!(sample_rate_ > 0.0) || maximum_block_size_ <= 0) {
        result.code = SignalGraphRouteResultCode::InvalidCommand;
        return result;
    }

    auto edit = graph_.begin_prepared_topology_edit();
    if (!edit) {
        result.code = SignalGraphRouteResultCode::PrepareFailed;
        return result;
    }
    for (const auto& command : commands) {
        bool accepted = false;
        switch (command.kind) {
        case SignalGraphRouteCommand::Kind::Insert:
            accepted = command.audio_rate
                           ? edit->connect_audio_rate_modulation(
                                 command.source, command.source_port, command.destination,
                                 command.parameter_id, command.range_lo, command.range_hi,
                                 command.smoothing_ms, command.mix)
                           : edit->connect_automation(command.source, command.source_port,
                                                      command.destination, command.parameter_id,
                                                      command.range_lo, command.range_hi,
                                                      command.smoothing_ms, command.mix);
            break;
        case SignalGraphRouteCommand::Kind::Rewire:
            accepted = edit->disconnect_modulation(
                command.previous_source, command.previous_source_port, command.destination,
                command.parameter_id, command.audio_rate);
            if (accepted) {
                accepted = command.audio_rate
                               ? edit->connect_audio_rate_modulation(
                                     command.source, command.source_port, command.destination,
                                     command.parameter_id, command.range_lo, command.range_hi,
                                     command.smoothing_ms, command.mix)
                               : edit->connect_automation(command.source, command.source_port,
                                                          command.destination, command.parameter_id,
                                                          command.range_lo, command.range_hi,
                                                          command.smoothing_ms, command.mix);
            }
            break;
        case SignalGraphRouteCommand::Kind::Remove:
            accepted = edit->disconnect_modulation(command.source, command.source_port,
                                                   command.destination, command.parameter_id,
                                                   command.audio_rate);
            break;
        default:
            accepted = false;
            break;
        }
        if (!accepted) {
            result.code = SignalGraphRouteResultCode::InvalidCommand;
            return result;
        }
        ++result.applied;
    }

    result.topology_result = edit->prepare(sample_rate_, maximum_block_size_);
    if (result.topology_result != SignalGraph::PreparedTopologyEdit::Result::Prepared) {
        result.code = SignalGraphRouteResultCode::PrepareFailed;
        return result;
    }
    if (!edit->routed_execution_ready(maximum_block_size_)) {
        result.code = SignalGraphRouteResultCode::PrepareFailed;
        return result;
    }
    result.topology_result = edit->commit();
    if (result.topology_result != SignalGraph::PreparedTopologyEdit::Result::Committed) {
        result.code = SignalGraphRouteResultCode::CommitFailed;
        return result;
    }
    ++generation_;
    result.generation = generation_;
    result.code = SignalGraphRouteResultCode::Applied;
    return result;
}

void SignalGraphControlAuthority::reset() noexcept {
    ++generation_;
}

} // namespace pulp::host
