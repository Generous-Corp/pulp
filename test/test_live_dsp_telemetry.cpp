#include <catch2/catch_test_macros.hpp>

#include "pulp/audio/live_dsp_telemetry.hpp"
#include "pulp/runtime/scoped_no_alloc.hpp"

#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <thread>
#include <vector>

using namespace pulp::audio;

namespace {

std::vector<LiveDspNodeInfo> make_nodes(std::initializer_list<const char*> names) {
    std::vector<LiveDspNodeInfo> nodes;
    std::uint64_t id = 1;
    for (const char* name : names) {
        LiveDspNodeInfo info;
        info.node_id = id++;
        info.kind = LiveDspNodeKind::Plugin;
        info.set_name(name);
        nodes.push_back(info);
    }
    return nodes;
}

}  // namespace

TEST_CASE("live-dsp telemetry disabled by default and after prepare", "[live-dsp-telemetry]") {
    LiveDspTelemetryStore store;
    REQUIRE_FALSE(store.enabled());
    REQUIRE_FALSE(store.prepared());

    const auto nodes = make_nodes({"in", "gain", "out"});
    REQUIRE(store.prepare({}, nodes));
    REQUIRE(store.prepared());
    REQUIRE(store.node_count() == 3);
    REQUIRE_FALSE(store.enabled());  // prepare does not enable

    // Disabled: begin_block yields an inactive writer and records nothing.
    auto writer = store.begin_block(128, 48000.0);
    REQUIRE_FALSE(writer.active());
    { auto scope = writer.node(0); (void)scope; }
    writer.finish();
    REQUIRE(store.blocks_written() == 0);
    REQUIRE(store.blocks_dropped() == 0);
}

TEST_CASE("live-dsp telemetry records and drains a block", "[live-dsp-telemetry]") {
    LiveDspTelemetryStore store;
    const auto nodes = make_nodes({"a", "b"});
    REQUIRE(store.prepare({}, nodes));
    store.set_enabled(true);

    {
        auto writer = store.begin_block(256, 48000.0);
        REQUIRE(writer.active());
        {
            auto scope = writer.node(0);
            // A little real work so the measured elapsed is > 0.
            volatile double acc = 0.0;
            for (int i = 0; i < 4096; ++i) acc += static_cast<double>(i) * 0.5;
            (void)acc;
        }
        writer.finish();
    }

    REQUIRE(store.blocks_written() == 1);
    store.drain();

    const auto& snap = store.latest();
    REQUIRE(snap.available);
    REQUIRE(snap.enabled);
    REQUIRE(snap.node_count == 2);
    REQUIRE(snap.blocks_written == 1);
    REQUIRE(snap.blocks_drained == 1);
    REQUIRE(snap.nodes.size() == 2);
    REQUIRE(snap.nodes[0].sample_count == 1);
    REQUIRE(snap.nodes[0].last_elapsed_ns >= 0);
    REQUIRE(std::string(snap.nodes[0].name) == "a");
    REQUIRE(snap.nodes[0].node_id == 1);
    // Node b was never timed this block -> zero slot.
    REQUIRE(snap.nodes[1].last_elapsed_ns == 0);
}

TEST_CASE("live-dsp telemetry audio-thread write path does not allocate", "[live-dsp-telemetry][rt-safety]") {
    LiveDspTelemetryStore store;
    const auto nodes = make_nodes({"a", "b", "c"});
    REQUIRE(store.prepare({}, nodes));
    store.set_enabled(true);

    // Warm up one block outside the probe (no lazy state remains after prepare,
    // but this keeps the assertion focused on the steady-state write path).
    { auto w = store.begin_block(128, 48000.0); { auto s = w.node(0); (void)s; } w.finish(); }
    store.drain();

    {
        pulp::test::RtAllocationProbe probe;
        pulp::runtime::ScopedNoAlloc no_alloc;
        for (int block = 0; block < 64; ++block) {
            auto writer = store.begin_block(128, 48000.0);
            for (std::uint32_t n = 0; n < 3; ++n) {
                auto scope = writer.node(n);
                volatile int x = block + static_cast<int>(n);
                (void)x;
            }
            writer.finish();
        }
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("live-dsp telemetry drops blocks when the ring is full", "[live-dsp-telemetry]") {
    LiveDspTelemetryStore store;
    const auto nodes = make_nodes({"a"});
    LiveDspTelemetryConfig config;
    config.ring_blocks = 2;
    REQUIRE(store.prepare(config, nodes));
    store.set_enabled(true);

    const std::array<std::int64_t, 1> ns{100};
    // No draining between injects -> ring saturates at 2, the rest drop.
    for (int i = 0; i < 5; ++i) {
        store.inject_block(ns, 100, 128, 48000.0);
    }

    REQUIRE(store.blocks_written() == 2);
    REQUIRE(store.blocks_dropped() == 3);

    store.drain();
    REQUIRE(store.latest().blocks_dropped == 3);

    // After draining, the ring has room again.
    store.inject_block(ns, 100, 128, 48000.0);
    REQUIRE(store.blocks_written() == 3);
    REQUIRE(store.blocks_dropped() == 3);
}

TEST_CASE("live-dsp telemetry computes deterministic percentiles and jitter", "[live-dsp-telemetry]") {
    LiveDspTelemetryStore store;
    const auto nodes = make_nodes({"a"});
    LiveDspTelemetryConfig config;
    config.ring_blocks = 128;
    config.percentile_window_blocks = 128;
    REQUIRE(store.prepare(config, nodes));
    store.set_enabled(true);

    // Feed elapsed = 1..100 ns. Budget is large so nothing is over budget.
    for (std::int64_t v = 1; v <= 100; ++v) {
        const std::array<std::int64_t, 1> ns{v};
        store.inject_block(ns, v, 4096, 48000.0);
        if (v % 64 == 0) {
            store.drain();  // periodic drains keep the ring from overflowing
        }
    }
    store.drain();

    const auto& node = store.latest().nodes[0];
    REQUIRE(node.sample_count == 100);
    REQUIRE(node.min_elapsed_ns == 1);
    REQUIRE(node.max_elapsed_ns == 100);
    REQUIRE(node.mean_elapsed_ns == 50);  // (1+..+100)/100 == 5050/100 == 50 (trunc)
    REQUIRE(node.p50_elapsed_ns == 50);
    REQUIRE(node.p95_elapsed_ns == 95);
    REQUIRE(node.p99_elapsed_ns == 99);
    REQUIRE(node.jitter_ns == node.p95_elapsed_ns - node.p50_elapsed_ns);
    REQUIRE(node.jitter_ns == 45);
    REQUIRE(store.latest().graph_over_budget_blocks == 0);
}

TEST_CASE("live-dsp telemetry attributes over-budget blocks to the worst node", "[live-dsp-telemetry]") {
    LiveDspTelemetryStore store;
    const auto nodes = make_nodes({"cheap", "expensive"});
    REQUIRE(store.prepare({}, nodes));
    store.set_enabled(true);

    // 128 frames @ 48kHz -> budget = 128/48000 s ~= 2.667 ms == 2'666'666 ns.
    // Graph elapsed 5 ms is over budget; node 1 (expensive) dominates so it
    // takes the attribution.
    const std::array<std::int64_t, 2> ns{500'000, 4'000'000};
    store.inject_block(ns, 5'000'000, 128, 48000.0);
    // A comfortably-under-budget block (0.4 ms << 2.667 ms): no attribution.
    const std::array<std::int64_t, 2> ns_ok{100'000, 200'000};
    store.inject_block(ns_ok, 400'000, 128, 48000.0);
    store.drain();

    const auto& snap = store.latest();
    REQUIRE(snap.graph_over_budget_blocks == 1);
    REQUIRE(snap.nodes[0].over_budget_attributions == 0);
    REQUIRE(snap.nodes[1].over_budget_attributions == 1);
    REQUIRE(snap.last_graph_load < 1.0f);  // last block (ns_ok) was under budget
}

TEST_CASE("live-dsp telemetry survives a concurrent producer and drainer", "[live-dsp-telemetry][rt-safety]") {
    LiveDspTelemetryStore store;
    const auto nodes = make_nodes({"a", "b"});
    LiveDspTelemetryConfig config;
    config.ring_blocks = 64;
    REQUIRE(store.prepare(config, nodes));
    store.set_enabled(true);

    constexpr int kBlocks = 5000;
    std::thread producer([&] {
        for (int i = 0; i < kBlocks; ++i) {
            auto writer = store.begin_block(128, 48000.0);
            { auto s0 = writer.node(0); (void)s0; }
            { auto s1 = writer.node(1); (void)s1; }
            writer.finish();
        }
    });

    std::thread drainer([&] {
        for (int i = 0; i < kBlocks; ++i) {
            store.drain();
            std::this_thread::yield();
        }
    });

    producer.join();
    drainer.join();
    store.drain();

    // Every produced block is either drained or dropped; none vanish.
    const auto& snap = store.latest();
    REQUIRE(store.blocks_written() == static_cast<std::uint64_t>(kBlocks) - store.blocks_dropped());
    REQUIRE(snap.blocks_drained + store.blocks_dropped() == static_cast<std::uint64_t>(kBlocks));
}

// A [[clang::nonblocking]] wrapper so RealtimeSanitizer verifies the telemetry
// write path is free of locks and syscalls too — not only allocations, which
// the RtAllocationProbe cases above already cover. Under a non-RTSan build the
// attribute is inert (the RTSan lane configures without -Wfunction-effects), so
// this compiles everywhere and runs as a plain functional smoke; under the
// advisory `-fsanitize=realtime` lane it arms the runtime RT-violation checks
// over the whole begin_block -> node scopes -> finish path. Guarded because only
// Clang understands the attribute.
#if defined(__clang__)
#  define PULP_TEST_RT_NONBLOCKING [[clang::nonblocking]]
#else
#  define PULP_TEST_RT_NONBLOCKING
#endif

namespace {

// The attribute is a function-type attribute: it sits after the parameter list
// (like noexcept), never as a leading declaration attribute.
void write_one_telemetry_block(LiveDspTelemetryStore& store, std::uint32_t frames)
    PULP_TEST_RT_NONBLOCKING {
    auto writer = store.begin_block(frames, 48000.0);
    for (std::uint32_t n = 0; n < 3; ++n) {
        auto scope = writer.node(n);
        volatile int x = static_cast<int>(n);
        (void)x;
    }
    writer.finish();
}

}  // namespace

TEST_CASE("live-dsp telemetry write path is nonblocking (RTSan-checkable)", "[live-dsp-telemetry][rt-safety]") {
    LiveDspTelemetryStore store;
    const auto nodes = make_nodes({"a", "b", "c"});
    REQUIRE(store.prepare({}, nodes));
    store.set_enabled(true);

    // Warm one block (prepare pre-allocates the ring, so nothing lazy remains,
    // but this keeps the checked loop on the pure steady-state path).
    write_one_telemetry_block(store, 128);
    store.drain();

    for (int b = 0; b < 64; ++b) write_one_telemetry_block(store, 128);
    store.drain();

    REQUIRE(store.latest().blocks_drained >= 64);
}
