#include <pulp/host/signal_graph.hpp>
#include "spectral_graph_internal.hpp"

namespace pulp::host {
SpectralError SignalGraph::stop_spectral_controller() {
    GraphMutationLock lock(*this);
    if (spectral_access_closed_.load(std::memory_order_seq_cst)) return SpectralError::Busy;
    spectral_reclaiming_.store(true, std::memory_order_seq_cst);
    if (spectral_readers_.load(std::memory_order_seq_cst)) {
        spectral_reclaiming_.store(false, std::memory_order_seq_cst);
        return SpectralError::Busy;
    }
    spectral_mode_.store(false, std::memory_order_release);
    for (auto& slot : spectral_slots_) slot.reset();
    spectral_render_target_ = nullptr;
    spectral_reclaiming_.store(false, std::memory_order_seq_cst);
    return SpectralError::None;
}
SignalGraph::SnapshotAccess SignalGraph::read_snapshot_(bool access) const noexcept {
    if (!spectral_mode_.load(std::memory_order_acquire)) return SnapshotAccess{live_slot_.read()};
    spectral_readers_.fetch_add(1, std::memory_order_seq_cst);
    // The increment and recheck pair with the control-side retirement gate.
    // A reader admitted after the zero-reader check never dereferences a slot.
    if (spectral_reclaiming_.load(std::memory_order_seq_cst)
        || !spectral_mode_.load(std::memory_order_acquire))
        return SnapshotAccess{this, nullptr, false};
    if (access) {
        spectral_accesses_.fetch_add(1, std::memory_order_seq_cst);
        if (spectral_access_closed_.load(std::memory_order_seq_cst)) {
            spectral_error_.store(SpectralError::TransitionPending, std::memory_order_relaxed);
            return SnapshotAccess{this, nullptr, true};
        }
    }
    const int active = spectral_active_.load(std::memory_order_seq_cst);
    return SnapshotAccess{this, spectral_slots_[active].get(), access};
}

bool SignalGraph::spectral_mailboxes_pending_(const CompiledGraph& graph) const noexcept {
    for (const auto& [id, runtime] : graph.runtime) {
        (void)id;
        const auto pending = [](const auto& mailbox) {
            return mailbox && mailbox->next_sequence.load(std::memory_order_acquire)
                != mailbox->sequence_seen.load(std::memory_order_acquire);
        };
        if (pending(runtime.midi_input_mailbox) || pending(runtime.parameter_input_mailbox)
            || pending(runtime.exact_parameter_input_mailbox)) return true;
        if (runtime.midi_output_mailbox && (!runtime.midi_output_mailbox->pending.empty()
            || runtime.midi_output_mailbox->consumer_has_retry
            || runtime.midi_output_mailbox->incomplete.load(std::memory_order_acquire))) return true;
    }
    return false;
}

SignalGraph::SpectralTransitionState SignalGraph::spectral_transition_state() const noexcept {
    const int ack = spectral_ack_.load(std::memory_order_acquire);
    if (ack >= 0) return spectral_ack_error_ == SpectralError::None
        ? SpectralTransitionState::Activated : SpectralTransitionState::Rejected;
    return spectral_access_closed_.load(std::memory_order_seq_cst)
        ? SpectralTransitionState::Staged : SpectralTransitionState::Idle;
}

SpectralError SignalGraph::offer_spectral_candidate_locked_(std::unique_ptr<SignalGraph>& candidate,
    std::shared_ptr<CompiledGraph> snapshot, std::uint64_t generation) {
    if (!spectral_mode_.load(std::memory_order_acquire) || !snapshot->spectral)
        return SpectralError::RequiresStoppedController;
    if (spectral_candidate_authoring_ || spectral_access_closed_.load(std::memory_order_seq_cst))
        return SpectralError::Busy;
    for (const auto& n : candidate->nodes_)
        if (n.type == NodeType::Plugin || n.type == NodeType::Custom)
            return SpectralError::UnsupportedTransactionalReplacement;
    spectral_access_closed_.store(true, std::memory_order_seq_cst);
    if (spectral_accesses_.load(std::memory_order_seq_cst)
        || spectral_readers_.load(std::memory_order_seq_cst)) {
        spectral_access_closed_.store(false, std::memory_order_seq_cst);
        return SpectralError::Busy;
    }
    const int active = spectral_active_.load(std::memory_order_seq_cst);
    const int offered = (active + 1) % 3;
    snapshot->execution_generation = generation + 1;
    spectral_slots_[offered] = std::move(snapshot);
    spectral_candidate_authoring_ = std::move(candidate);
    spectral_offer_generation_ = generation;
    spectral_ack_.store(-1, std::memory_order_relaxed);
    spectral_pending_.store(offered, std::memory_order_release);
    return SpectralError::None;
}

SpectralError SignalGraph::finalize_spectral_transition() {
    GraphMutationLock lock(*this);
    const int ack = spectral_ack_.load(std::memory_order_acquire);
    if (ack < 0) return spectral_access_closed_.load(std::memory_order_seq_cst)
        ? SpectralError::TransitionPending : SpectralError::None;
    spectral_reclaiming_.store(true, std::memory_order_seq_cst);
    if (spectral_readers_.load(std::memory_order_seq_cst)) {
        spectral_reclaiming_.store(false, std::memory_order_seq_cst);
        return SpectralError::Busy;
    }
    const auto result = spectral_ack_error_;
    if (result == SpectralError::None) {
        adopt_candidate_authoring_locked_(*spectral_candidate_authoring_);
        auto& active = *spectral_slots_[spectral_active_.load(std::memory_order_seq_cst)];
        publish_prepared_stats_locked_(active);
        ++authoring_generation_;
    }
    spectral_candidate_authoring_.reset();
    const int active = spectral_active_.load(std::memory_order_seq_cst);
    for (int index = 0; index < 3; ++index)
        if (index != active) spectral_slots_[index].reset();
    spectral_ack_.store(-1, std::memory_order_release);
    spectral_access_closed_.store(false, std::memory_order_seq_cst);
    spectral_error_.store(result, std::memory_order_relaxed);
    spectral_reclaiming_.store(false, std::memory_order_seq_cst);
    return result;
}

SignalGraph::SpectralBoundary SignalGraph::capture_spectral_boundary_() noexcept {
    const int active = spectral_active_.load(std::memory_order_seq_cst);
    const int offered = spectral_pending_.exchange(-1, std::memory_order_acquire);
    return {active, offered};
}

void SignalGraph::process_spectral_(audio::BufferView<float>& output,
    const audio::BufferView<const float>& input, int count, const format::ProcessContext* transport) {
    if (count <= 0) return;
    process_spectral_boundary_(capture_spectral_boundary_(), output, input, count, transport);
}

void SignalGraph::process_spectral_boundary_(SpectralBoundary boundary,
    audio::BufferView<float>& output, const audio::BufferView<const float>& input,
    int count, const format::ProcessContext* transport) {
    int active = boundary.active;
    auto* target = spectral_slots_[active].get();
    const int offered = boundary.offered;
    if (!target || count > target->max_block_size) {
        spectral_error_.store(SpectralError::CapacityExceeded, std::memory_order_relaxed);
        if (offered >= 0) {
            spectral_ack_error_ = SpectralError::CapacityExceeded;
            spectral_ack_.store(offered, std::memory_order_release);
        }
        output.clear();
        return;
    }
    const bool reset = transport && transport->should_reset_stream_history();
    bool activate = false;
    if (offered >= 0) {
        spectral_ack_error_ = spectral_offer_generation_ != spectral_render_generation_
            ? SpectralError::InvalidConnection
            : count > spectral_slots_[offered]->max_block_size ? SpectralError::CapacityExceeded
            : spectral_mailboxes_pending_(*target) ? SpectralError::PendingMailboxData
            : spectral_transition_serial_ == std::numeric_limits<std::uint32_t>::max()
                ? SpectralError::EpochExhausted : SpectralError::None;
        activate = spectral_ack_error_ == SpectralError::None;
    }
    if (activate) {
        spectral_retired_ = active;
        active = offered;
        target = spectral_slots_[active].get();
        spectral_active_.store(active, std::memory_order_seq_cst);
        ++spectral_render_generation_;
        total_latency_samples_.store(target->total_latency_samples, std::memory_order_relaxed);
    }
    if (activate || reset || spectral_render_target_ != target->spectral.get()) {
        if (spectral_transition_serial_ == std::numeric_limits<std::uint32_t>::max()) {
            spectral_terminal_ = true;
            spectral_error_.store(SpectralError::EpochExhausted, std::memory_order_relaxed);
        } else {
            ++spectral_transition_serial_;
            target->spectral->reset((diagnostic_graph_identity_ << 32) | spectral_transition_serial_);
            target->routed.serial.pool.clear_history();
            spectral_render_target_ = target->spectral.get();
        }
    }
    if (spectral_terminal_) output.clear();
    else process_snapshot_impl(output, input, count, transport, target);
    if (offered >= 0) spectral_ack_.store(offered, std::memory_order_release);
}

SpectralError SpectralRuntimeBank::prepare(std::span<const GraphNode> authored,
    std::span<const Connection> connections, double rate, int block) {
    if (block < 1 || block > 4096) return SpectralError::CapacityExceeded;
    nodes.clear();
    edges.clear();
    charged_bytes = 0;
    auto charge = [&](std::uint64_t bytes) {
        if (bytes > memory_limit - charged_bytes) return false;
        charged_bytes += static_cast<size_t>(bytes);
        return true;
    };
    const auto diagnostic_bytes = authored.size() * 258ull * sizeof(audio::LiveDspSpectralDiagnostic);
    if (!charge(diagnostic_bytes)) return SpectralError::CapacityExceeded;
    for (const auto& n : authored) {
        if (!n.spectral_identity) continue;
        const auto identity = *n.spectral_identity;
        if (identity.sample_rate_bits() != std::bit_cast<std::uint64_t>(rate))
            return SpectralError::IdentityMismatch;
        if (n.type != NodeType::SpectralAnalysis && n.type != NodeType::SpectralStage
            && n.type != NodeType::SpectralSynthesis) return SpectralError::InvalidIdentity;
        if (n.type == NodeType::SpectralStage) {
            if (!n.spectral_stage || n.spectral_stage->identity() != identity)
                return SpectralError::InvalidMask;
            switch (n.spectral_stage->kind()) {
            case QualifiedSpectralStage::Kind::IdentityV1:
                if (!n.spectral_stage->gains().empty()) return SpectralError::InvalidMask;
                break;
            case QualifiedSpectralStage::Kind::RealMaskV1:
                if (n.spectral_stage->gains().size() != static_cast<size_t>(identity.channels()) * identity.bins())
                    return SpectralError::InvalidMask;
                for (float gain : n.spectral_stage->gains())
                    if (!std::isfinite(gain) || gain < 0 || gain > 1) return SpectralError::InvalidMask;
                break;
            default: return SpectralError::InvalidMask;
            }
        }
        signal::SpectralFrameEngineConfig cfg;
        cfg.fft_size = identity.fft_size();
        cfg.analysis_hop = identity.hop();
        cfg.channels = identity.channels();
        cfg.max_block = block;
        cfg.max_synthesis_hop = identity.hop();
        cfg.defer_synthesis = false;
        cfg.guarded_split = true;
        const auto geometry = signal::checked_spectral_frame_engine_geometry<float>(cfg);
        if (!geometry || !charge(sizeof(Node) + 8ull * identity.channels() * identity.bins()
                                  + (n.spectral_stage ? n.spectral_stage->gains().size_bytes() : 0))
            || (n.type != NodeType::SpectralStage && !charge(geometry->retained_bytes)))
            return SpectralError::CapacityExceeded;
        auto state = std::make_unique<Node>(n);
        state->working.resize(static_cast<size_t>(identity.channels()) * identity.bins());
        if (n.type != NodeType::SpectralStage) {
            state->engine = std::make_unique<signal::SpectralFrameEngine>();
            state->engine->prepare(cfg);
        }
        nodes.push_back(std::move(state));
    }
    for (const auto& c : connections) {
        if (c.feedback) return SpectralError::InvalidConnection;
        auto* source = find(c.source_node);
        auto* destination = find(c.dest_node);
        if (!c.spectral) {
            if ((source || destination) && (c.midi || c.automation || c.audio_rate_modulation
                                           || c.sidechain || c.feedback))
                return SpectralError::InvalidConnection;
            if ((source && source->type != NodeType::SpectralSynthesis)
                || (destination && destination->type != NodeType::SpectralAnalysis))
                return SpectralError::InvalidConnection;
            continue;
        }
        if (!source || !destination || source->identity != destination->identity
            || source->type == NodeType::SpectralSynthesis || destination->type == NodeType::SpectralAnalysis
            || destination->input || c.source_port != 0 || c.dest_port != 0 || c.feedback
            || c.midi || c.automation || c.audio_rate_modulation || c.sidechain)
            return SpectralError::InvalidConnection;
        const auto& identity = source->identity;
        const size_t capacity = (block + identity.hop() - 1) / identity.hop();
        const size_t bins = static_cast<size_t>(identity.channels()) * identity.bins();
        if (!charge(sizeof(Edge) + capacity * (sizeof(graph::SpectralFrameStamp) + bins * sizeof(std::complex<float>))
                    + sizeof(graph::SpectralFrameBatchView*))) return SpectralError::CapacityExceeded;
        auto edge = std::make_unique<Edge>();
        edge->stamps.resize(capacity);
        edge->bins.resize(capacity * bins);
        edge->view = {edge->stamps, edge->bins, 0, static_cast<std::uint32_t>(bins)};
        destination->input = &edge->view;
        source->outputs.push_back(&edge->view);
        edges.push_back(std::move(edge));
    }
    for (const auto& n : nodes) {
        if (n->type != NodeType::SpectralAnalysis && !n->input) return SpectralError::InvalidConnection;
        if (n->type != NodeType::SpectralSynthesis && n->outputs.empty()) return SpectralError::InvalidConnection;
        const Node* ancestor = n.get();
        size_t steps = 0;
        while (ancestor->type != NodeType::SpectralAnalysis) {
            if (++steps > nodes.size()) return SpectralError::InvalidConnection;
            const Node* parent = nullptr;
            for (const auto& candidate : nodes)
                for (auto* output : candidate->outputs)
                    if (output == ancestor->input) parent = candidate.get();
            if (!parent) return SpectralError::InvalidConnection;
            ancestor = parent;
        }
        n->root = ancestor->id;
    }
    charged_bytes = 0;
    if (!charge(diagnostic_bytes + nodes.capacity() * sizeof(std::unique_ptr<Node>)
                + edges.capacity() * sizeof(std::unique_ptr<Edge>))) return SpectralError::CapacityExceeded;
    for (const auto& n : nodes) {
        if (!charge(sizeof(Node) + n->working.capacity() * sizeof(std::complex<float>)
                    + n->outputs.capacity() * sizeof(graph::SpectralFrameBatchView*)
                    + (n->stage ? n->stage->retained_config_bytes() : 0))
            || (n->engine && !charge(sizeof(signal::SpectralFrameEngine) + n->engine->retained_storage_bytes())))
            return SpectralError::CapacityExceeded;
    }
    for (const auto& edge : edges)
        if (!charge(sizeof(Edge) + edge->stamps.capacity() * sizeof(graph::SpectralFrameStamp)
                    + edge->bins.capacity() * sizeof(std::complex<float>))) return SpectralError::CapacityExceeded;
    for (const auto& n : authored)
        if (n.spectral_stage && !charge(n.spectral_stage->retained_config_bytes()))
            return SpectralError::CapacityExceeded;
    return SpectralError::None;
}
namespace {
GraphNode spectral_node(NodeId id, NodeType type, SpectralIdentity identity,
                         const std::string& name, std::uint64_t owner,
                         std::uint64_t incarnation) {
    GraphNode n;
    n.id = id;
    n.type = type;
    n.name = name;
    n.spectral_identity = identity;
    if (type == NodeType::SpectralAnalysis) {
        n.num_input_ports = identity.channels();
        for (int ch = 0; ch < identity.channels(); ++ch)
            n.input_ports.push_back({id, static_cast<PortIndex>(ch), false,
                ch == 0 ? "audio" : "in" + std::to_string(ch), PortKind::Audio, owner, incarnation});
        n.output_ports.push_back({id, 0, true, "frames", PortKind::Spectral, owner, incarnation});
    } else if (type == NodeType::SpectralSynthesis) {
        n.input_ports.push_back({id, 0, false, "frames", PortKind::Spectral, owner, incarnation});
        n.num_output_ports = identity.channels();
        for (int ch = 0; ch < identity.channels(); ++ch)
            n.output_ports.push_back({id, static_cast<PortIndex>(ch), true,
                ch == 0 ? "audio" : "out" + std::to_string(ch), PortKind::Audio, owner, incarnation});
    } else {
        n.input_ports.push_back({id, 0, false, "frames_in", PortKind::Spectral, owner, incarnation});
        n.output_ports.push_back({id, 0, true, "frames_out", PortKind::Spectral, owner, incarnation});
    }
    return n;
}
} // namespace

SpectralResult<SpectralAnalysisPorts> SignalGraph::add_spectral_analysis(
    SpectralIdentity identity, const std::string& name) {
    GraphMutationLock lock(*this);
    if (spectral_mode_.load(std::memory_order_acquire) || live_slot_.live())
        return {std::nullopt, SpectralError::RequiresStoppedController};
    auto n = spectral_node(next_id_++, NodeType::SpectralAnalysis, identity, name,
                           diagnostic_graph_identity_, authoring_generation_ + 1);
    SpectralAnalysisPorts result{n.id, n.input_ports, {n.output_ports.front()}};
    nodes_.push_back(std::move(n));
    invalidate_live_locked_();
    return {std::move(result)};
}

SpectralResult<SpectralStagePorts> SignalGraph::add_spectral_stage(
    SpectralIdentity identity, QualifiedSpectralStage stage, const std::string& name) {
    GraphMutationLock lock(*this);
    if (spectral_mode_.load(std::memory_order_acquire) || live_slot_.live())
        return {std::nullopt, SpectralError::RequiresStoppedController};
    if (stage.identity() != identity) return {std::nullopt, SpectralError::IdentityMismatch};
    auto n = spectral_node(next_id_++, NodeType::SpectralStage, identity, name,
                           diagnostic_graph_identity_, authoring_generation_ + 1);
    n.spectral_stage = std::move(stage);
    SpectralStagePorts result{n.id, {n.input_ports.front()}, {n.output_ports.front()}};
    nodes_.push_back(std::move(n));
    invalidate_live_locked_();
    return {std::move(result)};
}

SpectralResult<SpectralSynthesisPorts> SignalGraph::add_spectral_synthesis(
    SpectralIdentity identity, const std::string& name) {
    GraphMutationLock lock(*this);
    if (spectral_mode_.load(std::memory_order_acquire) || live_slot_.live())
        return {std::nullopt, SpectralError::RequiresStoppedController};
    auto n = spectral_node(next_id_++, NodeType::SpectralSynthesis, identity, name,
                           diagnostic_graph_identity_, authoring_generation_ + 1);
    SpectralSynthesisPorts result{n.id, {n.input_ports.front()}, n.output_ports};
    nodes_.push_back(std::move(n));
    invalidate_live_locked_();
    return {std::move(result)};
}
} // namespace pulp::host
