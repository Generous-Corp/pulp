#include <catch2/catch_test_macros.hpp>

#include <pulp/host/graph_timing_contract.hpp>
#include <pulp/host/signal_graph.hpp>

#include <cstdint>

namespace {

using namespace pulp::host;
constexpr double kSampleRate = 48000.0;
constexpr int kPartition = 64;

void prepare_graph(SignalGraph& graph) {
    REQUIRE(graph.add_gain_node("timing-gain") != 0);
    REQUIRE(graph.prepare(kSampleRate, kPartition));
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

    const auto evidence = evaluate_graph_timing_contract(graph, valid_declaration());

    REQUIRE(evidence.disposition == GraphTimingDisposition::Proven);
    REQUIRE(evidence.reason == GraphTimingReason::None);
    REQUIRE(evidence.measured_latency_samples == 0);
    REQUIRE(evidence.semantic_delay_samples == 0);
    REQUIRE(evidence.partition_size == kPartition);
    REQUIRE(evidence.block_size == 32);
    REQUIRE(graph.authoring_receipt() == before);
    REQUIRE(graph.nodes().size() == node_count);
}

TEST_CASE("DSPX-06 evaluator refuses invalid and mismatched declarations",
          "[dspx-06][graph][timing][negative]") {
    SignalGraph graph;
    prepare_graph(graph);
    const auto before = graph.authoring_receipt();

    SECTION("negative field") {
        auto declaration = valid_declaration();
        declaration.semantic_delay_samples = -1;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::InvalidField);
    }
    SECTION("algorithmic and PDC mismatch") {
        auto declaration = valid_declaration();
        declaration.algorithmic_latency_samples = 1;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::LatencyMismatch);
    }
    SECTION("unmeasured semantic delay") {
        auto declaration = valid_declaration();
        declaration.semantic_delay_samples = 128;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::SemanticDelayUnmeasured);
    }
    SECTION("unmeasured host PDC") {
        auto declaration = valid_declaration();
        declaration.host_pdc_samples = 1;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::HostPdcUnmeasured);
    }
    SECTION("fractional and multi-tap are typed unsupported") {
        auto fractional = valid_declaration();
        fractional.fractional_delay = true;
        auto evidence = evaluate_graph_timing_contract(graph, fractional);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Unsupported);
        REQUIRE(evidence.reason == GraphTimingReason::FractionalDelay);

        auto multi_tap = valid_declaration();
        multi_tap.multi_tap = true;
        evidence = evaluate_graph_timing_contract(graph, multi_tap);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Unsupported);
        REQUIRE(evidence.reason == GraphTimingReason::MultiTap);
    }

    REQUIRE(graph.authoring_receipt() == before);
}

TEST_CASE("DSPX-06 evaluator refuses partition, tail, and scheduling gaps",
          "[dspx-06][graph][timing][negative]") {
    SignalGraph graph;
    prepare_graph(graph);

    SECTION("partition") {
        auto declaration = valid_declaration();
        declaration.partition_size = kPartition * 2;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Unsupported);
        REQUIRE(evidence.reason == GraphTimingReason::PartitionUnsupported);
    }
    SECTION("tail") {
        auto declaration = valid_declaration();
        declaration.tail_samples = 1;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Unsupported);
        REQUIRE(evidence.reason == GraphTimingReason::TailUnsupported);
    }
    SECTION("block capacity") {
        auto declaration = valid_declaration();
        declaration.block_size = kPartition + 1;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::BlockCapacity);
    }
    SECTION("anticipation scheduling mismatch") {
        auto declaration = valid_declaration();
        declaration.anticipation_required = true;
        const auto evidence = evaluate_graph_timing_contract(graph, declaration);
        REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
        REQUIRE(evidence.reason == GraphTimingReason::SchedulingUnavailable);
    }
}

TEST_CASE("DSPX-06 evaluator refuses an unprepared graph without state change",
          "[dspx-06][graph][timing][negative][state]") {
    SignalGraph graph;
    const auto before = graph.authoring_receipt();
    auto declaration = valid_declaration();

    const auto evidence = evaluate_graph_timing_contract(graph, declaration);

    REQUIRE(evidence.disposition == GraphTimingDisposition::Refused);
    REQUIRE(evidence.reason == GraphTimingReason::Unprepared);
    REQUIRE(graph.authoring_receipt() == before);
    REQUIRE(graph.nodes().empty());
}
