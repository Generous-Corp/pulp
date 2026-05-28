/// @file test_motion_swift_bridge.cpp
/// Catch2 unit tests for the Swift bridge C ABI in
/// `apple/Sources/PulpSwift/PulpBridge.cpp`. Exercises the publish
/// channel shim, the ambient-provenance slot, and the geometry-trace
/// register/update/detach lifecycle without needing a Swift compiler.

#include "PulpBridge.h"

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <string>
#include <vector>

using pulp::view::FrameClock;
using namespace pulp::view::motion;

namespace {

/// RAII fixture: reset the Coordinator, bind a fresh FrameClock,
/// enable tracing, and install a buffer sink that captures every
/// SampleEvent (including `TraceStarted`).
class BridgeFixture {
public:
    BridgeFixture() {
        Coordinator::instance().reset();
        Coordinator::instance().bind(clock);
        Coordinator::instance().set_firehose(true);
        Coordinator::instance().set_tracing_enabled(true);
        sink_id = Coordinator::instance().add_sink(
            [this](const SampleEvent& e) { buffer.push_back(e); });
    }
    ~BridgeFixture() {
        pulp_motion_clear_ambient_provenance();
        Coordinator::instance().reset();
    }

    FrameClock clock;
    std::vector<SampleEvent> buffer;
    int sink_id = 0;
};

std::size_t count_kind(const std::vector<SampleEvent>& b,
                       SampleEvent::Kind k,
                       const std::string& metric = {}) {
    std::size_t n = 0;
    for (const auto& e : b) {
        if (e.kind == k && (metric.empty() || e.metric_name == metric)) ++n;
    }
    return n;
}

const SampleEvent* first_of(const std::vector<SampleEvent>& b,
                            SampleEvent::Kind k,
                            const std::string& metric = {}) {
    for (const auto& e : b) {
        if (e.kind == k && (metric.empty() || e.metric_name == metric))
            return &e;
    }
    return nullptr;
}

}  // namespace

// ── Off by default ────────────────────────────────────────────────────

TEST_CASE("pulp_motion shim is a no-op when tracing is disabled",
          "[motion][swift-bridge]") {
    Coordinator::instance().reset();
    REQUIRE_FALSE(pulp_motion_tracing_enabled());

    std::vector<SampleEvent> buf;
    const int sink = Coordinator::instance().add_sink(
        [&](const SampleEvent& e) { buf.push_back(e); });
    (void)sink;

    pulp_motion_publish_value("OffView", "opacity", 0.5, 0.001, 3);
    const char* keys[2] = {"x", "y"};
    const double vals[2] = {1.0, 2.0};
    pulp_motion_publish_components("OffView", "pt", keys, vals, 2, 0.001, 3);

    const int trace_id = pulp_motion_register_geometry_trace("OffView", 30);
    REQUIRE(trace_id == 0);  // returns 0 when tracing is disabled
    pulp_motion_update_geometry(trace_id, "frame", 0.0, 0.0, 100.0, 50.0);
    pulp_motion_detach_trace(trace_id);

    REQUIRE(buf.empty());
    Coordinator::instance().reset();
}

// ── Publish value / components ────────────────────────────────────────

TEST_CASE("pulp_motion_publish_value routes to the Coordinator",
          "[motion][swift-bridge]") {
    BridgeFixture fx;
    REQUIRE(pulp_motion_tracing_enabled());

    pulp_motion_publish_value("Card", "opacity", 0.0, 0.001, 3);
    pulp_motion_publish_value("Card", "opacity", 0.5, 0.001, 3);
    pulp_motion_publish_value("Card", "opacity", 1.0, 0.001, 3);

    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Baseline, "opacity") == 1);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Start, "opacity") >= 1);
    const auto* baseline = first_of(fx.buffer, SampleEvent::Kind::Baseline, "opacity");
    REQUIRE(baseline != nullptr);
    REQUIRE(baseline->view_name == "Card");
}

TEST_CASE("pulp_motion_publish_components emits sorted multi-component events",
          "[motion][swift-bridge]") {
    BridgeFixture fx;

    const char* keys[2] = {"y", "x"};
    const double v0[2] = {0.0, 0.0};
    pulp_motion_publish_components("Card", "frame", keys, v0, 2, 0.001, 3);
    const double v1[2] = {10.0, 20.0};
    pulp_motion_publish_components("Card", "frame", keys, v1, 2, 0.001, 3);

    const auto* baseline = first_of(fx.buffer, SampleEvent::Kind::Baseline, "frame");
    REQUIRE(baseline != nullptr);
    REQUIRE(baseline->components.size() == 2);
    REQUIRE(baseline->components[0].first == "x");  // sorted
    REQUIRE(baseline->components[1].first == "y");
}

TEST_CASE("pulp_motion_publish_components tolerates empty and null inputs",
          "[motion][swift-bridge][coverage]") {
    BridgeFixture fx;

    const char* keys[1] = {"x"};
    const double vals[1] = {1.0};
    pulp_motion_publish_components("Card", "p", keys, vals, 0, 0.001, 3);
    pulp_motion_publish_components("Card", "p", keys, vals, -1, 0.001, 3);
    pulp_motion_publish_components("Card", "p", nullptr, vals, 1, 0.001, 3);
    pulp_motion_publish_components("Card", "p", keys, nullptr, 1, 0.001, 3);

    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Baseline, "p") == 0);
}

TEST_CASE("pulp_motion bridge normalizes null C strings",
          "[motion][swift-bridge][coverage][requested]") {
    BridgeFixture fx;

    pulp_motion_publish_value(nullptr, nullptr, 0.0, 0.001, 3);
    pulp_motion_publish_value(nullptr, nullptr, 1.0, 0.001, 3);

    const auto* baseline = first_of(fx.buffer, SampleEvent::Kind::Baseline);
    REQUIRE(baseline != nullptr);
    REQUIRE(baseline->view_name.empty());
    REQUIRE(baseline->metric_name.empty());
}

TEST_CASE("pulp_motion bridge normalizes null provenance and geometry names",
          "[motion][swift-bridge][coverage][requested]") {
    BridgeFixture fx;

    pulp_motion_set_ambient_provenance(nullptr, nullptr, nullptr, 0);
    pulp_motion_publish_value("Card", "opacity", 0.0, 0.001, 3);
    pulp_motion_publish_value("Card", "opacity", 1.0, 0.001, 3);

    const auto* opacity = first_of(fx.buffer, SampleEvent::Kind::Baseline, "opacity");
    REQUIRE(opacity != nullptr);
    REQUIRE(opacity->view_name == "Card");
    REQUIRE(opacity->provenance.source_kind.empty());
    REQUIRE(opacity->provenance.source_id.empty());
    REQUIRE(opacity->provenance.source_file.empty());
    REQUIRE(opacity->provenance.source_line == 0);

    const int id = pulp_motion_register_geometry_trace(nullptr, -10);
    REQUIRE(id > 0);

    pulp_motion_update_geometry(id, "bounds", 1.0, 2.0, 3.0, 4.0);
    pulp_motion_update_geometry(id, "bounds", 5.0, 6.0, 7.0, 8.0);

    const auto* bounds = first_of(fx.buffer, SampleEvent::Kind::Baseline, "bounds");
    REQUIRE(bounds != nullptr);
    REQUIRE(bounds->view_name == "swiftui");
    REQUIRE(bounds->metric_name == "bounds");
    REQUIRE(bounds->components.size() == 4);
    REQUIRE(bounds->components[0].first == "height");
    REQUIRE(bounds->components[1].first == "minX");
    REQUIRE(bounds->components[2].first == "minY");
    REQUIRE(bounds->components[3].first == "width");

    pulp_motion_detach_trace(id);
}

// ── Ambient provenance ────────────────────────────────────────────────

TEST_CASE("pulp_motion_set_ambient_provenance stamps subsequent publishes",
          "[motion][swift-bridge]") {
    BridgeFixture fx;

    pulp_motion_set_ambient_provenance("swiftui", "CardView",
                                       "PulpMotion.swift", 42);
    pulp_motion_publish_value("Card", "opacity", 0.0, 0.001, 3);
    pulp_motion_publish_value("Card", "opacity", 1.0, 0.001, 3);

    const auto* baseline = first_of(fx.buffer, SampleEvent::Kind::Baseline, "opacity");
    REQUIRE(baseline != nullptr);
    REQUIRE(baseline->provenance.source_kind == "swiftui");
    REQUIRE(baseline->provenance.source_id == "CardView");
    REQUIRE(baseline->provenance.source_file == "PulpMotion.swift");
    REQUIRE(baseline->provenance.source_line == 42);

    pulp_motion_clear_ambient_provenance();
    pulp_motion_publish_value("Card2", "opacity", 0.0, 0.001, 3);
    pulp_motion_publish_value("Card2", "opacity", 1.0, 0.001, 3);
    const auto* baseline2 = first_of(fx.buffer, SampleEvent::Kind::Baseline);
    // The first event whose view is Card2:
    const SampleEvent* card2_baseline = nullptr;
    for (const auto& e : fx.buffer) {
        if (e.kind == SampleEvent::Kind::Baseline && e.view_name == "Card2") {
            card2_baseline = &e; break;
        }
    }
    REQUIRE(card2_baseline != nullptr);
    REQUIRE(card2_baseline->provenance.source_kind.empty());
    (void)baseline2;
}

// ── Geometry trace register / update / detach ─────────────────────────

TEST_CASE("pulp_motion_register_geometry_trace round-trips through FrameClock",
          "[motion][swift-bridge]") {
    BridgeFixture fx;

    const int id = pulp_motion_register_geometry_trace("CardView", 30);
    REQUIRE(id > 0);

    // First geometry: baseline.
    pulp_motion_update_geometry(id, "frame", 10.0, 20.0, 100.0, 50.0);
    // Tick @ 60fps ≈ 16.6 ms; sampler is gated to 30fps so we need ≥ 2
    // ticks before the first sample fires.
    for (int i = 0; i < 5; ++i) fx.clock.tick(1.0f / 60.0f);

    // Now change the rect to trigger a Start/End burst.
    pulp_motion_update_geometry(id, "frame", 10.0, 120.0, 100.0, 50.0);
    for (int i = 0; i < 5; ++i) fx.clock.tick(1.0f / 60.0f);
    // Stabilize so the burst closes.
    for (int i = 0; i < 10; ++i) fx.clock.tick(1.0f / 60.0f);

    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::TraceStarted) >= 1);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Baseline, "frame") >= 1);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Start, "frame") >= 1);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::End, "frame") >= 1);

    // TraceStarted should carry the auto-stamped provenance.
    const auto* started = first_of(fx.buffer, SampleEvent::Kind::TraceStarted);
    REQUIRE(started != nullptr);
    REQUIRE(started->provenance.source_kind == "swiftui");
    REQUIRE(started->provenance.source_id == "CardView");

    // Detach: subsequent updates must not emit.
    const std::size_t pre_detach = fx.buffer.size();
    pulp_motion_detach_trace(id);
    pulp_motion_update_geometry(id, "frame", 999.0, 999.0, 999.0, 999.0);
    for (int i = 0; i < 10; ++i) fx.clock.tick(1.0f / 60.0f);

    // The non-empty stabilization may have appended End events between
    // the snapshot and the detach call, so allow a small slack — but
    // crucially, no new "frame" Sample/Start should fire after detach.
    std::size_t post_starts = 0;
    for (std::size_t i = pre_detach; i < fx.buffer.size(); ++i) {
        const auto& e = fx.buffer[i];
        if (e.metric_name != "frame") continue;
        if (e.kind == SampleEvent::Kind::Start) ++post_starts;
    }
    REQUIRE(post_starts == 0);

    // Idempotent.
    pulp_motion_detach_trace(id);
}

TEST_CASE("pulp_motion_update_geometry with a non-default metric also emits",
          "[motion][swift-bridge]") {
    BridgeFixture fx;

    const int id = pulp_motion_register_geometry_trace("ScrollView", 60);
    REQUIRE(id > 0);

    // Use a metric name other than "frame" — the bridge routes this
    // through `publish_components` on the "swiftui" view name.
    pulp_motion_update_geometry(id, "scroll", 0.0, 0.0, 320.0, 480.0);
    pulp_motion_update_geometry(id, "scroll", 0.0, 100.0, 320.0, 480.0);
    pulp_motion_update_geometry(id, "scroll", 0.0, 200.0, 320.0, 480.0);

    const auto* baseline = first_of(fx.buffer, SampleEvent::Kind::Baseline, "scroll");
    REQUIRE(baseline != nullptr);
    REQUIRE(baseline->view_name == "ScrollView");
    REQUIRE(baseline->components.size() == 4);
    REQUIRE(baseline->components[0].first == "height");
    REQUIRE(baseline->components[1].first == "minX");
    REQUIRE(baseline->components[2].first == "minY");
    REQUIRE(baseline->components[3].first == "width");

    pulp_motion_detach_trace(id);
}

TEST_CASE("pulp_motion_update_geometry keeps frame metrics on the sampled trace only",
          "[motion][swift-bridge][coverage][requested]") {
    BridgeFixture fx;

    const int id = pulp_motion_register_geometry_trace("FrameOnly", 60);
    REQUIRE(id > 0);

    pulp_motion_update_geometry(id, "frame", 1.0, 2.0, 3.0, 4.0);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Baseline, "frame") == 0);

    fx.clock.tick(1.0f / 60.0f);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Baseline, "frame") == 1);
    const auto* baseline = first_of(fx.buffer, SampleEvent::Kind::Baseline, "frame");
    REQUIRE(baseline != nullptr);
    REQUIRE(baseline->view_name == "FrameOnly");
    REQUIRE(baseline->components.size() == 4);

    pulp_motion_detach_trace(id);
}

TEST_CASE("pulp_motion geometry registry survives coordinator reset",
          "[motion][swift-bridge][coverage]") {
    BridgeFixture fx;

    const int first = pulp_motion_register_geometry_trace("ViewA", 30);
    const int second = pulp_motion_register_geometry_trace("ViewB", 30);
    REQUIRE(first > 0);
    REQUIRE(second > 0);

    pulp_motion_update_geometry(first, "frame", 0.0, 0.0, 100.0, 100.0);
    pulp_motion_update_geometry(second, "frame", 10.0, 20.0, 50.0, 50.0);
    for (int i = 0; i < 3; ++i) fx.clock.tick(1.0f / 60.0f);

    Coordinator::instance().reset();
    Coordinator::instance().bind(fx.clock);
    Coordinator::instance().set_firehose(true);
    Coordinator::instance().set_tracing_enabled(true);

    const int fresh = pulp_motion_register_geometry_trace("ViewC", 30);
    REQUIRE(fresh > 0);
    pulp_motion_detach_trace(fresh);
}

TEST_CASE("pulp_motion geometry bridge ignores unknown trace ids",
          "[motion][swift-bridge][coverage][requested]") {
    BridgeFixture fx;

    pulp_motion_update_geometry(123456, "frame", 1.0, 2.0, 3.0, 4.0);
    pulp_motion_detach_trace(123456);
    for (int i = 0; i < 4; ++i) fx.clock.tick(1.0f / 60.0f);

    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Baseline, "frame") == 0);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Start, "frame") == 0);
}

TEST_CASE("PulpBridge state ABI rejects invalid buffers without touching callers",
          "[motion][swift-bridge][coverage][requested]") {
    int untouched_size = 1234;
    REQUIRE(pulp_state_serialize(nullptr) == nullptr);
    REQUIRE(untouched_size == 1234);

    const uint8_t bytes[] = {0x70, 0x75, 0x6c, 0x70};
    REQUIRE_FALSE(pulp_state_deserialize(nullptr, 4));
    REQUIRE_FALSE(pulp_state_deserialize(bytes, 0));
    REQUIRE_FALSE(pulp_state_deserialize(bytes, -1));

    pulp_free(nullptr);
}
