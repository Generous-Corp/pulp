#pragma once

// Control-side timing evidence for a prepared SignalGraph.  This header is a
// deliberately small public seam: it keeps the typed timing contract out of
// the large SignalGraph runtime declaration and has no audio-thread API.

#include <cstdint>

namespace pulp::host {

class SignalGraph;

enum class GraphTimingDisposition : std::uint8_t {
    Proven,
    Refused,
    Unsupported,
};

enum class GraphTimingReason : std::uint8_t {
    None,
    Unprepared,
    InvalidField,
    BlockCapacity,
    LatencyMismatch,
    SchedulingUnavailable,
    PartitionUnsupported,
    TailUnsupported,
    FractionalDelay,
    MultiTap,
    SemanticDelayUnmeasured,
    HostPdcUnmeasured,
    AlgorithmicLatencyUnmeasured,
    GroupDelayUnmeasured,
    MeasurementMismatch,
};

// A declaration keeps semantic delay (the musical/audio meaning of a delay)
// separate from algorithmic latency and host PDC.  Only integer, zero-tail,
// fixed-partition declarations are currently certifiable by this evaluator.
struct GraphTimingDeclaration {
    int semantic_delay_samples = 0;
    int algorithmic_latency_samples = 0;
    int host_pdc_samples = 0;
    int partition_size = 0;
    int tail_samples = 0;
    int block_size = 0;
    bool scheduling_guaranteed = false;
    bool anticipation_required = false;
    bool fractional_delay = false;
    bool multi_tap = false;
};

// Measurements are supplied by an independent control-side oracle. The
// evaluator never treats declaration fields as measurements.
struct GraphTimingMeasurements {
    bool semantic_delay_measured = false;
    bool algorithmic_latency_measured = false;
    bool host_pdc_measured = false;
    bool impulse_group_delay_measured = false;
    int semantic_delay_samples = 0;
    int algorithmic_latency_samples = 0;
    int host_pdc_samples = 0;
    int impulse_group_delay_samples = -1;
};

struct GraphTimingEvidence {
    GraphTimingDisposition disposition = GraphTimingDisposition::Refused;
    GraphTimingReason reason = GraphTimingReason::InvalidField;
    int measured_latency_samples = 0;
    int semantic_delay_samples = 0;
    int algorithmic_latency_samples = 0;
    int host_pdc_samples = 0;
    int partition_size = 0;
    int tail_samples = 0;
    int block_size = 0;
    bool scheduling_guaranteed = false;
    bool anticipation_required = false;
    bool runtime_snapshot_available = false;
    int impulse_group_delay_samples = -1;

    constexpr explicit operator bool() const noexcept {
        return disposition == GraphTimingDisposition::Proven;
    }
};

// Evaluate one declaration against the graph's immutable prepared snapshot.
// This is observational and control-thread only: no topology, authoring
// generation, or execution state is changed by a call.
GraphTimingEvidence
evaluate_graph_timing_contract(const SignalGraph& graph, const GraphTimingDeclaration& declaration,
                               const GraphTimingMeasurements& measurements) noexcept;

} // namespace pulp::host
