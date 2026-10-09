#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/graph_timing_contract.hpp>
#include <pulp/host/signal_graph.hpp>

#include <array>
#include <cstdint>

namespace {

using namespace pulp::host;
constexpr double kSampleRate = 48000.0;
constexpr int kPartition = 64;

void prepare_graph(SignalGraph& graph, int partition = kPartition) {
    const auto input = graph.add_input_node(1, "timing-input");
    const auto gain = graph.add_gain_node("timing-gain");
    const auto output = graph.add_output_node(1, "timing-output");
    REQUIRE(input != 0);
    REQUIRE(gain != 0);
    REQUIRE(output != 0);
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.prepare(kSampleRate, partition));
}

GraphTimingMeasurements impulse_measurements(SignalGraph& graph, int block_size) {
    std::array<float, kPartition> input{};
    std::array<float, kPartition> output{};
    input[0] = 1.0f;
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    pulp::audio::BufferView<const float> input_view(input_channels, 1, block_size);
    pulp::audio::BufferView<float> output_view(output_channels, 1, block_size);
    graph.process(output_view, input_view, block_size);

    int peak = -1;
    for (int i = 0; i < block_size; ++i) {
        if (output[static_cast<std::size_t>(i)] > 0.999f) {
            peak = i;
            break;
        }
    }
    REQUIRE(peak >= 0);
    GraphTimingMeasurements measurements;
    measurements.semantic_delay_measured = true;
    measurements.algorithmic_latency_measured = true;
    measurements.host_pdc_measured = true;
    measurements.impulse_group_delay_measured = true;
    measurements.semantic_delay_samples = 0;
    measurements.algorithmic_latency_samples = graph.latency_samples();
    measurements.host_pdc_samples = 0;
    measurements.impulse_group_delay_samples = peak;
    REQUIRE(measurements.algorithmic_latency_samples == peak);
    return measurements;
}

GraphTimingDeclaration valid_declaration() {
    GraphTimingDeclaration declaration;
    declaration.semantic_delay_samples = 0;
    declaration.algorithmic_latency_samples = 0;
    declaration.host_pdc_samples = 0;
    declaration.partition_size = kPartition;
    declaration.tail_samples = 0;
    declaration.block_size = 32;
    declaration.scheduling_guaranteed = true;
    declaration.anticipation_required = false;
    return declaration;
}

} // namespace

TEST_CASE("DSPX-06 evaluator proves typed integer graph timing",
          "[dspx-06][graph][timing][positive]") {
    SignalGraph graph;
    prepare_graph(graph);
    const auto before = graph.authoring_receipt();
    const auto node_count = graph.nodes().size();
    const auto prepared_before = graph.prepared_stats();
    const auto snapshot_before = graph.live_snapshot_handle();
    const auto measurements = impulse_measurements(graph, 32);

    const auto evidence = evaluate_graph_timing_contract(graph, valid_declaration(), measurements);

    REQUIRE(evidence.disposition == GraphTimingDisposition::Proven);
    REQUIRE(evidence.reason == GraphTimingReason::None);
    REQUIRE(evidence.measured_latency_samples == 0);
    REQUIRE(evidence.semantic_delay_samples == 0);
    REQUIRE(evidence.partition_size == kPartition);
    REQUIRE(evidence.block_size == 32);
    REQUIRE(evidence.impulse_group_delay_samples == 0);
    REQUIRE(evidence.runtime_snapshot_available);
    REQUIRE(graph.authoring_receipt() == before);
    REQUIRE(graph.nodes().size() == node_count);
    const auto prepared_after = graph.prepared_stats();
    REQUIRE(prepared_after.node_count == prepared_before.node_count);
    REQUIRE(prepared_after.connection_count == prepared_before.connection_count);
    REQUIRE(prepared_after.max_block_size == prepared_before.max_block_size);
    // The runtime snapshot remains pinned and unchanged by this observational receipt.
    REQUIRE(graph.live_snapshot_handle() == snapshot_before);
}

TEST_CASE("DSPX-06 evaluator refuses invalid and mismatched declarations",
          "[dspx-06][graph][timing][negative]") {
    SignalGraph graph;
    prepare_graph(graph);
    const auto before = graph.authoring_receipt();
    const auto measurements = impulse_measurements(graph, 32);

    SECTION("negative field") {
        auto declaration = valid_declaration();
        declaration.semantic_delay_samples = -1;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration, measurements);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::InvalidField);
    }
    SECTION("algorithmic and PDC mismatch") {
        auto declaration = valid_declaration();
        declaration.algorithmic_latency_samples = 1;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration, measurements);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::MeasurementMismatch);
    }
    SECTION("unmeasured semantic delay") {
        auto declaration = valid_declaration();
        declaration.semantic_delay_samples = 128;
        auto unmeasured = measurements;
        unmeasured.semantic_delay_measured = false;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration, unmeasured);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::SemanticDelayUnmeasured);
    }
    SECTION("unmeasured host PDC") {
        auto declaration = valid_declaration();
        declaration.host_pdc_samples = 1;
        auto unmeasured = measurements;
        unmeasured.host_pdc_measured = false;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration, unmeasured);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::HostPdcUnmeasured);
    }
    SECTION("unmeasured algorithmic latency and group delay") {
        auto declaration = valid_declaration();
        auto unmeasured = measurements;
        unmeasured.algorithmic_latency_measured = false;
        auto evidence = evaluate_graph_timing_contract(graph, declaration, unmeasured);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::AlgorithmicLatencyUnmeasured);

        unmeasured = measurements;
        unmeasured.impulse_group_delay_measured = false;
        evidence = evaluate_graph_timing_contract(graph, declaration, unmeasured);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::GroupDelayUnmeasured);
    }
    SECTION("fractional and multi-tap are typed unsupported") {
        auto fractional = valid_declaration();
        fractional.fractional_delay = true;
        auto evidence = evaluate_graph_timing_contract(graph, fractional, measurements);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Unsupported);
        REQUIRE(evidence.reason == GraphTimingReason::FractionalDelay);

        auto multi_tap = valid_declaration();
        multi_tap.multi_tap = true;
        evidence = evaluate_graph_timing_contract(graph, multi_tap, measurements);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Unsupported);
        REQUIRE(evidence.reason == GraphTimingReason::MultiTap);
    }

    REQUIRE(graph.authoring_receipt() == before);
}

TEST_CASE("DSPX-06 evaluator refuses partition, tail, and scheduling gaps",
          "[dspx-06][graph][timing][negative]") {
    SignalGraph graph;
    prepare_graph(graph);
    const auto measurements = impulse_measurements(graph, 32);

    SECTION("partition") {
        auto declaration = valid_declaration();
        declaration.partition_size = kPartition * 2;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration, measurements);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Unsupported);
        REQUIRE(evidence.reason == GraphTimingReason::PartitionUnsupported);
    }
    SECTION("tail matrix") {
        for (const int tail : {1, 2, 64}) {
            auto declaration = valid_declaration();
            declaration.tail_samples = tail;
            const auto evidence = evaluate_graph_timing_contract(graph, declaration, measurements);
            REQUIRE(evidence.disposition == GraphTimingDisposition::Unsupported);
            REQUIRE(evidence.reason == GraphTimingReason::TailUnsupported);
        }
    }
    SECTION("block capacity") {
        auto declaration = valid_declaration();
        declaration.block_size = kPartition + 1;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration, measurements);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::BlockCapacity);
    }
    SECTION("anticipation scheduling mismatch") {
        auto declaration = valid_declaration();
        declaration.anticipation_required = true;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration, measurements);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::SchedulingUnavailable);
    }
}

TEST_CASE("DSPX-06 positive partition and independent impulse matrix",
          "[dspx-06][graph][timing][positive][matrix]") {
    for (const int partition : {16, 64}) {
        SignalGraph graph;
        prepare_graph(graph, partition);
        auto declaration = valid_declaration();
        declaration.partition_size = partition;
        declaration.block_size = partition / 2;
        const auto measurements = impulse_measurements(graph, declaration.block_size);
        const auto evidence = evaluate_graph_timing_contract(graph, declaration, measurements);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Proven);
        REQUIRE(evidence.partition_size == partition);
        REQUIRE(evidence.impulse_group_delay_samples == 0);
    }
}

TEST_CASE("DSPX-06 evaluator refuses an unprepared graph without state change",
          "[dspx-06][graph][timing][negative][state]") {
    SignalGraph graph;
    const auto before = graph.authoring_receipt();
    auto declaration = valid_declaration();
    GraphTimingMeasurements measurements;

    const auto evidence = evaluate_graph_timing_contract(graph, declaration, measurements);

    REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
    REQUIRE(evidence.reason == GraphTimingReason::Unprepared);
    REQUIRE(graph.authoring_receipt() == before);
    REQUIRE(graph.nodes().empty());
}
