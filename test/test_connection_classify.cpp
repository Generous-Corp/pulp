// Pins the single connection-classification surface. classify() is the one
// helper both the executor-routing gather and the legacy reference-walk edge
// bucketer route through, so the two can never disagree about which runtime
// lane a host Connection carries. Each host Connection variant must map to one
// {kind, feedback, audio_rate} triple; connection_affects_latency() must agree
// with that lane for the PDC/latency passes.

#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>

#include <catch2/catch_test_macros.hpp>

using pulp::host::Connection;
using pulp::host::ConnectionClass;
using pulp::host::classify;
using pulp::host::connection_affects_latency;
using Kind = pulp::graph::GraphRuntimeConnectionKind;

namespace {

// Builds a Connection with the flags each SignalGraph::connect_* path sets,
// without needing a live graph/plugin: connect()/connect_sidechain set plain
// audio (optionally sidechain), connect_midi sets midi, connect_automation sets
// automation, connect_audio_rate_modulation sets audio_rate_modulation, and the
// explicit feedback path sets feedback.
Connection make(bool feedback = false, bool midi = false, bool automation = false,
                bool audio_rate = false, bool sidechain = false) {
    Connection c{};
    c.source_node = 1;
    c.dest_node = 2;
    c.feedback = feedback;
    c.midi = midi;
    c.automation = automation;
    c.audio_rate_modulation = audio_rate;
    c.sidechain = sidechain;
    return c;
}

} // namespace

TEST_CASE("classify maps each host Connection variant to one runtime lane",
          "[host][signal-graph][routing][classify]") {
    SECTION("plain audio edge is the Audio lane") {
        const ConnectionClass cls = classify(make());
        CHECK(cls.kind == Kind::Audio);
        CHECK_FALSE(cls.feedback);
        CHECK_FALSE(cls.audio_rate);
        CHECK(connection_affects_latency(make()));
    }
    SECTION("MIDI edge is the Event lane") {
        const ConnectionClass cls = classify(make(/*feedback=*/false, /*midi=*/true));
        CHECK(cls.kind == Kind::Event);
        CHECK_FALSE(cls.feedback);
        CHECK_FALSE(connection_affects_latency(make(false, true)));
    }
    SECTION("sparse automation edge is the Automation lane, not audio-rate") {
        const ConnectionClass cls =
            classify(make(false, false, /*automation=*/true));
        CHECK(cls.kind == Kind::Automation);
        CHECK_FALSE(cls.audio_rate);
        // Sparse automation carries no latency-aligned audio.
        CHECK_FALSE(connection_affects_latency(make(false, false, true)));
    }
    SECTION("dense audio-rate modulation edge is the Automation lane, audio-rate") {
        const ConnectionClass cls =
            classify(make(false, false, false, /*audio_rate=*/true));
        CHECK(cls.kind == Kind::Automation);
        CHECK(cls.audio_rate);
        // A dense audio-rate edge IS sampled per block-position and participates
        // in PDC like audio.
        CHECK(connection_affects_latency(make(false, false, false, true)));
    }
    SECTION("sidechain edge folds into the Audio lane") {
        const ConnectionClass cls =
            classify(make(false, false, false, false, /*sidechain=*/true));
        CHECK(cls.kind == Kind::Audio);
        CHECK_FALSE(cls.feedback);
        CHECK(connection_affects_latency(make(false, false, false, false, true)));
    }
    SECTION("feedback edge is plain Audio with the orthogonal feedback flag set") {
        const ConnectionClass cls = classify(make(/*feedback=*/true));
        CHECK(cls.kind == Kind::Audio);
        CHECK(cls.feedback);
        // A back-edge reads the previous block; it carries no forward latency.
        CHECK_FALSE(connection_affects_latency(make(true)));
    }
    SECTION("MIDI takes lane precedence over the feedback flag") {
        const ConnectionClass cls = classify(make(/*feedback=*/true, /*midi=*/true));
        CHECK(cls.kind == Kind::Event);
        CHECK(cls.feedback);
    }
}

TEST_CASE("classify is total and deterministic for malformed multi-lane flags",
          "[host][signal-graph][routing][classify]") {
    // The SignalGraph connect_* builders only ever set ONE lane flag (midi /
    // automation / audio_rate_modulation are mutually exclusive by construction),
    // so these combinations are not reachable through the public graph API. But
    // build_executor_snapshot() accepts an arbitrary Connection span, so classify()
    // must still collapse any flag combination to exactly one lane with a defined
    // precedence rather than aliasing the old "two lanes at once" bool state. This
    // pins that precedence: midi wins the lane; otherwise any automation flavor is
    // the Automation lane; audio_rate tracks audio_rate_modulation independently.
    SECTION("midi + automation resolves to the Event lane (midi wins)") {
        const ConnectionClass cls =
            classify(make(/*feedback=*/false, /*midi=*/true, /*automation=*/true));
        CHECK(cls.kind == Kind::Event);
        CHECK_FALSE(cls.audio_rate);
    }
    SECTION("midi + audio_rate_modulation resolves to the Event lane (midi wins)") {
        const ConnectionClass cls = classify(
            make(/*feedback=*/false, /*midi=*/true, /*automation=*/false,
                 /*audio_rate=*/true));
        CHECK(cls.kind == Kind::Event);
        CHECK(cls.audio_rate);  // the flag is reported; the lane is still Event
    }
    SECTION("automation + audio_rate_modulation resolves to the dense Automation lane") {
        const ConnectionClass cls = classify(
            make(/*feedback=*/false, /*midi=*/false, /*automation=*/true,
                 /*audio_rate=*/true));
        CHECK(cls.kind == Kind::Automation);
        CHECK(cls.audio_rate);  // audio-rate (dense) wins the sparse/dense split
        CHECK(connection_affects_latency(
            make(false, false, true, true)));  // dense participates in PDC
    }
}
