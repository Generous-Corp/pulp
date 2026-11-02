#include <pulp/host/signal_graph.hpp>
#include <catch2/catch_test_macros.hpp>

using pulp::host::GraphResult;
using pulp::host::PortKind;
using pulp::host::SignalGraph;

TEST_CASE("named graph ports report actionable diagnostics") {
    SignalGraph graph;
    const auto source = graph.add_gain_node("source");
    const auto sink = graph.add_gain_node("sink");
    const auto src = graph.node(source)->out("audio");
    const auto dst = graph.node(sink)->in("audio");
    REQUIRE(graph.connect(src, dst));

    auto unknown = graph.node(source)->out("missing");
    auto result = graph.connect(unknown, dst);
    CHECK(result.code == GraphResult::Code::UnknownPort);
    CHECK(result.message().find("unknown output port 'missing'") != std::string::npos);

    auto mismatch = src;
    mismatch.kind = PortKind::Midi;
    result = graph.connect(mismatch, dst);
    CHECK(result.code == GraphResult::Code::KindMismatch);
    CHECK(result.message().find("port kind mismatch") != std::string::npos);
}

TEST_CASE("named graph ports reject an instantaneous cycle with a message") {
    SignalGraph graph;
    const auto a = graph.add_gain_node("a");
    const auto b = graph.add_gain_node("b");
    REQUIRE(graph.connect(graph.node(a)->out("audio"), graph.node(b)->in("audio")));
    const auto result = graph.connect(graph.node(b)->out("audio"), graph.node(a)->in("audio"));
    CHECK(result.code == GraphResult::Code::IllegalCycle);
    CHECK(result.message().find("illegal instantaneous cycle") != std::string::npos);
}
