#include <pulp/host/graph_timing_contract.hpp>

#include <pulp/host/signal_graph.hpp>

#include <cstdint>
#include <limits>

namespace pulp::host {

GraphTimingEvidence
evaluate_graph_timing_contract(const SignalGraph& graph,
                               const GraphTimingDeclaration& declaration) noexcept {
    GraphTimingEvidence evidence;
    evidence.semantic_delay_samples = declaration.semantic_delay_samples;
    evidence.algorithmic_latency_samples = declaration.algorithmic_latency_samples;
    evidence.host_pdc_samples = declaration.host_pdc_samples;
    evidence.partition_size = declaration.partition_size;
    evidence.tail_samples = declaration.tail_samples;
    evidence.block_size = declaration.block_size;
    evidence.scheduling_guaranteed = declaration.scheduling_guaranteed;
    evidence.anticipation_required = declaration.anticipation_required;

    // These shapes need an independent group-delay proof.  They are typed
    // unsupported instead of being rounded into a false integer receipt.
    if (declaration.fractional_delay) {
        evidence.disposition = GraphTimingDisposition::Unsupported;
        evidence.reason = GraphTimingReason::FractionalDelay;
        return evidence;
    }
    if (declaration.multi_tap) {
        evidence.disposition = GraphTimingDisposition::Unsupported;
        evidence.reason = GraphTimingReason::MultiTap;
        return evidence;
    }

    if (declaration.semantic_delay_samples < 0 || declaration.algorithmic_latency_samples < 0 ||
        declaration.host_pdc_samples < 0 || declaration.partition_size <= 0 ||
        declaration.tail_samples < 0 || declaration.block_size <= 0) {
        evidence.reason = GraphTimingReason::InvalidField;
        return evidence;
    }
    if (!graph.is_prepared()) {
        evidence.reason = GraphTimingReason::Unprepared;
        return evidence;
    }

    const int prepared_block = graph.prepared_max_block_size();
    if (prepared_block <= 0 || declaration.partition_size != prepared_block) {
        evidence.disposition = GraphTimingDisposition::Unsupported;
        evidence.reason = GraphTimingReason::PartitionUnsupported;
        return evidence;
    }
    if (declaration.tail_samples != 0) {
        evidence.disposition = GraphTimingDisposition::Unsupported;
        evidence.reason = GraphTimingReason::TailUnsupported;
        return evidence;
    }
    // SignalGraph exposes one measured graph latency, not independent
    // semantic-delay or host-PDC measurements. Never turn caller metadata into
    // proof: a non-zero value in either unmeasured domain is refused.
    if (declaration.semantic_delay_samples != 0) {
        evidence.reason = GraphTimingReason::SemanticDelayUnmeasured;
        return evidence;
    }
    if (declaration.host_pdc_samples != 0) {
        evidence.reason = GraphTimingReason::HostPdcUnmeasured;
        return evidence;
    }
    if (declaration.block_size > prepared_block) {
        evidence.reason = GraphTimingReason::BlockCapacity;
        return evidence;
    }

    evidence.measured_latency_samples = graph.latency_samples();
    const auto expected = static_cast<std::int64_t>(declaration.algorithmic_latency_samples) +
                          static_cast<std::int64_t>(declaration.host_pdc_samples);
    if (expected > std::numeric_limits<int>::max() ||
        evidence.measured_latency_samples != static_cast<int>(expected)) {
        evidence.reason = GraphTimingReason::LatencyMismatch;
        return evidence;
    }

    // Anticipation is a scheduling requirement, not a latency field.  A
    // declaration that asks for it must match the prepared graph's scheduling
    // mode, and a guaranteed schedule must have a ready routed path for this
    // block size.
    if (declaration.anticipation_required != graph.anticipation_enabled()) {
        evidence.reason = GraphTimingReason::SchedulingUnavailable;
        return evidence;
    }
    if (declaration.scheduling_guaranteed &&
        !graph.routed_execution_status(declaration.block_size).routed_path_ready()) {
        evidence.reason = GraphTimingReason::SchedulingUnavailable;
        return evidence;
    }

    evidence.disposition = GraphTimingDisposition::Proven;
    evidence.reason = GraphTimingReason::None;
    return evidence;
}

} // namespace pulp::host
