#include "signal_graph_internal.hpp"
#include <pulp/host/signal_graph_runtime.hpp>

#include <algorithm>
#include <limits>

namespace pulp::host {

SignalGraph::EdgeProbeStatus SignalGraph::attach_audio_edge_probe(const PortHandle& source,
                                                                  const PortHandle& destination) {
    GraphMutationLock lock(*this);
    if (refuse_spectral_mutation_()) return EdgeProbeStatus::UnsupportedExecution;
    if (in_swap_edit_)
        return EdgeProbeStatus::Busy;
    const auto cg = read_snapshot_(true);
    if (!cg)
        return EdgeProbeStatus::Unprepared;
    if (!canonical_executor_routing_enabled() || parallel_routing_enabled() ||
        anticipation_enabled() || !cg->routed.serial.valid ||
        cg->pdc_execution_domain == PdcExecutionDomain::Legacy ||
        cg->pdc_execution_domain == PdcExecutionDomain::RoutedParallel)
        return EdgeProbeStatus::UnsupportedExecution;
    if (source.kind != PortKind::Audio || destination.kind != PortKind::Audio)
        return EdgeProbeStatus::UnsupportedKind;
    const auto valid_port = [&](const PortHandle& handle, bool output) {
        const auto* n = node(handle.node);
        if (!n || handle.output != output)
            return false;
        const auto& ports = output ? n->output_ports : n->input_ports;
        return std::any_of(ports.begin(), ports.end(), [&](const auto& p) {
            return p.name == handle.name && p.index == handle.index && p.kind == handle.kind;
        });
    };
    if (!valid_port(source, true) || !valid_port(destination, false))
        return EdgeProbeStatus::UnknownPort;
    const auto& plan = cg->routed.serial.snapshot.plan();
    for (std::size_t i = 0; i < cg->connections.size(); ++i) {
        const auto& edge = cg->connections[i];
        if (edge.source_node != source.node || edge.source_port != source.index ||
            edge.dest_node != destination.node || edge.dest_port != destination.index)
            continue;
        if (edge.feedback || edge.midi || edge.automation || edge.audio_rate_modulation ||
            edge.sidechain)
            return EdgeProbeStatus::UnsupportedKind;
        for (std::uint32_t j = 0; j < plan.connections.size(); ++j) {
            const auto& resolved = plan.connections[j];
            if (plan.nodes[resolved.source_index].id != source.node ||
                plan.nodes[resolved.dest_index].id != destination.node ||
                resolved.source_port != source.index || resolved.dest_port != destination.index)
                continue;
            auto probe = std::make_unique<format::GraphRuntimeAudioEdgeProbe>();
            probe->source_index = resolved.source_index;
            probe->source_port = resolved.source_port;
            probe->edge_index = j;
            probe->samples.attach();
            cg->audio_edge_probe_identity = {{diagnostic_graph_identity_, authoring_generation_},
                                             cg->connection_identities[i],
                                             source.node,
                                             destination.node,
                                             source.index,
                                             destination.index};
            cg->audio_edge_probe = std::move(probe);
            return EdgeProbeStatus::Attached;
        }
        return EdgeProbeStatus::UnsupportedExecution;
    }
    return EdgeProbeStatus::MissingEdge;
}

void SignalGraph::detach_audio_edge_probe() {
    GraphMutationLock lock(*this);
    if (refuse_spectral_mutation_()) return;
    if (const auto cg = read_snapshot_(true)) {
        cg->audio_edge_probe.reset();
        cg->audio_edge_probe_identity = {};
    }
}

bool SignalGraph::read_audio_edge_probe(graph::GraphRuntimeProbeSample& sample) {
    GraphMutationLock lock(*this);
    const auto cg = read_snapshot_(true);
    return cg && cg->audio_edge_probe && cg->audio_edge_probe->samples.try_pop(sample);
}

std::uint64_t SignalGraph::audio_edge_probe_dropped() const {
    GraphMutationLock lock(*this);
    const auto cg = read_snapshot_(true);
    return cg && cg->audio_edge_probe ? cg->audio_edge_probe->samples.dropped() : 0;
}

SignalGraph::EdgeProbeStatus SignalGraph::audio_edge_probe_status() const {
    GraphMutationLock lock(*this);
    const auto cg = read_snapshot_(true);
    if (!cg)
        return EdgeProbeStatus::Unprepared;
    return cg->audio_edge_probe && cg->audio_edge_probe->samples.attached()
               ? EdgeProbeStatus::Attached
               : EdgeProbeStatus::Detached;
}

SignalGraph::EdgeProbeIdentity SignalGraph::audio_edge_probe_identity() const {
    GraphMutationLock lock(*this);
    const auto cg = read_snapshot_(true);
    return cg && cg->audio_edge_probe ? cg->audio_edge_probe_identity : EdgeProbeIdentity{};
}

std::uint64_t SignalGraph::next_diagnostic_graph_identity_() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
    return 0; // Exhaustion fails closed instead of recycling an identity.
}

GraphAuthoringReceipt SignalGraph::authoring_receipt() const {
    GraphMutationLock lock(*this);
    return {diagnostic_graph_identity_, authoring_generation_};
}

GraphAuthoringReceiptStatus
SignalGraph::validate_authoring_receipt(GraphAuthoringReceipt receipt) const {
    GraphMutationLock lock(*this);
    if (receipt.graph_identity != diagnostic_graph_identity_)
        return GraphAuthoringReceiptStatus::WrongGraph;
    if (receipt.generation != authoring_generation_)
        return GraphAuthoringReceiptStatus::Stale;
    return GraphAuthoringReceiptStatus::Current;
}

bool SignalGraph::register_custom_node_diagnostics(CustomNodeDiagnosticsDescriptor descriptor) {
    if (descriptor.type_id.empty() || descriptor.type_id.size() >= 128 ||
        descriptor.type_id.find('\0') != std::string::npos ||
        descriptor.producer_type_id.find('\0') != std::string::npos ||
        descriptor.type_version <= 0 || descriptor.producer_type_id.size() >= 128 ||
        (!descriptor.producer_type_id.empty() && descriptor.producer_type_version <= 0) ||
        !descriptor.schema || !descriptor.query || !descriptor.report_bytes ||
        descriptor.report_bytes > CustomNodeDiagnosticsDescriptor::kMaximumReportBytes)
        return false;
    GraphMutationLock lock(*this);
    const auto key = custom_node_key(descriptor.type_id, descriptor.type_version);
    if (!custom_node_types_.contains(key) || in_swap_edit_)
        return false;
    if (descriptor.producer_type_id.empty()) {
        descriptor.producer_type_id = descriptor.type_id;
        descriptor.producer_type_version = descriptor.type_version;
    }
    custom_node_diagnostics_[key] = std::move(descriptor);
    ++authoring_generation_;
    return true;
}

CustomNodeDiagnosticResult SignalGraph::custom_node_diagnostic_handle(NodeId id) const {
    CustomNodeDiagnosticResult result;
    GraphMutationLock lock(*this, std::try_to_lock);
    if (!lock.owns_lock() || in_swap_edit_) {
        result.availability = CustomNodeDiagnosticAvailability::Busy;
        return result;
    }
    result.handle = {diagnostic_graph_identity_, authoring_generation_, id};
    const auto found =
        std::find_if(nodes_.begin(), nodes_.end(), [id](const auto& n) { return n.id == id; });
    if (found == nodes_.end()) {
        result.availability = CustomNodeDiagnosticAvailability::MissingNode;
        return result;
    }
    const auto descriptor = custom_node_diagnostics_.find(
        custom_node_key(found->custom_type_id, found->custom_type_version));
    if (found->type != NodeType::Custom || descriptor == custom_node_diagnostics_.end())
        return result;
    result.schema = descriptor->second.schema;
    result.type_version = found->custom_type_version;
    std::copy(found->custom_type_id.begin(), found->custom_type_id.end(), result.type_id.begin());
    const auto& producer = descriptor->second.producer_type_id;
    std::copy(producer.begin(), producer.end(), result.producer_type_id.begin());
    result.producer_type_version = descriptor->second.producer_type_version;
    result.availability = diagnostic_graph_identity_ == 0
                              ? CustomNodeDiagnosticAvailability::StaleHandle
                              : CustomNodeDiagnosticAvailability::Available;
    return result;
}

CustomNodeDiagnosticResult
SignalGraph::query_custom_node_diagnostics(CustomNodeDiagnosticHandle handle, std::uint64_t schema,
                                           std::span<std::byte> output,
                                           CustomNodeDiagnosticConsistency consistency) const {
    CustomNodeDiagnosticResult result;
    result.handle = handle;
    result.schema = schema;
    result.consistency = consistency;
    // Never spend unbounded work clearing an arbitrary caller buffer.
    if (output.size() > CustomNodeDiagnosticsDescriptor::kMaximumReportBytes) {
        result.availability = CustomNodeDiagnosticAvailability::SchemaMismatch;
        return result;
    }
    std::fill(output.begin(), output.end(), std::byte{});
    GraphMutationLock lock(*this, std::try_to_lock);
    if (!lock.owns_lock() || in_swap_edit_) {
        result.availability = CustomNodeDiagnosticAvailability::Busy;
        return result;
    }
    if (!handle.graph_identity || handle.graph_identity != diagnostic_graph_identity_ ||
        handle.graph_generation != authoring_generation_) {
        result.availability = CustomNodeDiagnosticAvailability::StaleHandle;
        return result;
    }
    const auto found = std::find_if(nodes_.begin(), nodes_.end(),
                                    [handle](const auto& n) { return n.id == handle.node_id; });
    if (found == nodes_.end()) {
        result.availability = CustomNodeDiagnosticAvailability::MissingNode;
        return result;
    }
    const auto descriptor = custom_node_diagnostics_.find(
        custom_node_key(found->custom_type_id, found->custom_type_version));
    if (found->type != NodeType::Custom || descriptor == custom_node_diagnostics_.end())
        return result;
    const auto& inspector = descriptor->second;
    result.type_version = found->custom_type_version;
    std::copy(found->custom_type_id.begin(), found->custom_type_id.end(), result.type_id.begin());
    const auto& producer = descriptor->second.producer_type_id;
    std::copy(producer.begin(), producer.end(), result.producer_type_id.begin());
    result.producer_type_version = descriptor->second.producer_type_version;
    if (schema != inspector.schema || output.size() != inspector.report_bytes) {
        result.availability = CustomNodeDiagnosticAvailability::SchemaMismatch;
        return result;
    }
    // Slot::live() is a shared_ptr control-thread owner, not the RT atomic raw
    // pointer. Copy under the same mutation lock used by publication/reclamation.
    // No reader pin is acquired under this lock (release waits for those pins).
    const auto live = read_snapshot_();
    if (!live || !found->custom_instance) {
        result.availability = CustomNodeDiagnosticAvailability::NotPrepared;
        return result;
    }
    const auto instance = live->custom_instances.find(handle.node_id);
    if (instance == live->custom_instances.end() ||
        instance->second != found->custom_instance.get()) {
        result.availability = CustomNodeDiagnosticAvailability::StaleHandle;
        return result;
    }
    result.availability =
        inspector.query(found->custom_instance.get(), output, result.preparation_generation);
    if (result.availability != CustomNodeDiagnosticAvailability::Available) {
        std::fill(output.begin(), output.end(), std::byte{});
        result.preparation_generation = 0;
    }
    return result;
}

} // namespace pulp::host
