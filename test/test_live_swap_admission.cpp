// Unit test for evaluate_live_swap_admission — the pure CPU-budget gate for a live
// plugin-instance swap. Deny on uncertainty (no history), deny over budget, admit
// within budget. No graph needed; the helper is pure over load snapshots.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/signal_graph.hpp>

#include <string>
#include <vector>

using pulp::host::evaluate_live_swap_admission;
using Snap = pulp::audio::AudioProcessLoadSnapshot;

TEST_CASE("live-swap admission denies on no history and over budget, admits within",
          "[host][graph][live-swap][admission]") {
    constexpr float kThreshold = 0.75f;
    constexpr std::uint64_t kMinCallbacks = 8;

    SECTION("graph with no measured history -> deny") {
        Snap graph;  // callback_count defaults to 0
        graph.load = 0.10f;
        const auto a = evaluate_live_swap_admission(graph, {}, kThreshold, kMinCallbacks);
        CHECK_FALSE(a.admit);
        CHECK(std::string(a.reason) == "no history");
    }
    SECTION("a staged node with no history -> deny even if the graph is measured") {
        Snap graph;
        graph.callback_count = 100;
        graph.load = 0.20f;
        Snap node;  // callback_count 0
        const auto a = evaluate_live_swap_admission(graph, {node}, kThreshold, kMinCallbacks);
        CHECK_FALSE(a.admit);
        CHECK(std::string(a.reason) == "no history");
    }
    SECTION("within budget -> admit (graph 0.32 + node 0.12 = 0.44 <= 0.75)") {
        Snap graph;
        graph.callback_count = 100;
        graph.load = 0.30f;
        graph.last_load = 0.32f;
        Snap node;
        node.callback_count = 100;
        node.load = 0.10f;
        node.last_load = 0.12f;
        const auto a = evaluate_live_swap_admission(graph, {node}, kThreshold, kMinCallbacks);
        CHECK(a.admit);
        CHECK(std::string(a.reason) == "ok");
    }
    SECTION("over budget -> deny (graph 0.65 + node 0.25 = 0.90 > 0.75)") {
        Snap graph;
        graph.callback_count = 100;
        graph.load = 0.60f;
        graph.last_load = 0.65f;
        Snap node;
        node.callback_count = 100;
        node.load = 0.20f;
        node.last_load = 0.25f;
        const auto a = evaluate_live_swap_admission(graph, {node}, kThreshold, kMinCallbacks);
        CHECK_FALSE(a.admit);
        CHECK(std::string(a.reason) == "over budget");
    }
    SECTION("uses the worst-case of load vs last_load") {
        Snap graph;
        graph.callback_count = 100;
        graph.load = 0.10f;
        graph.last_load = 0.90f;  // a recent spike dominates
        const auto a = evaluate_live_swap_admission(graph, {}, kThreshold, kMinCallbacks);
        CHECK_FALSE(a.admit);
    }
}
