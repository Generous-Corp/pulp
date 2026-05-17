/// @file test_motion_android_bridge.cpp
/// Catch2 unit tests for the Android JNI bridge C ABI in
/// `core/platform/src/android/jni_motion.cpp`. Mirrors
/// `test_motion_swift_bridge.cpp` — exercises the publish channel
/// shim, the ambient-provenance slot, and the geometry-trace
/// register/update/detach lifecycle by calling the C symbols
/// directly. The `Java_com_pulp_motion_PulpMotionNative_*` JNI
/// exports compile only under `__ANDROID__` (gated inside
/// jni_motion.cpp via `PULP_MOTION_HAVE_JNI`) so they do not affect
/// the host-side test target.

#include <pulp/platform/android/motion_bridge.h>
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
/// SampleEvent (including `TraceStarted`). The destructor clears
/// ambient provenance so leftover state cannot leak across tests.
class AndroidBridgeFixture {
public:
    AndroidBridgeFixture() {
        Coordinator::instance().reset();
        Coordinator::instance().bind(clock);
        Coordinator::instance().set_firehose(true);
        Coordinator::instance().set_tracing_enabled(true);
        sink_id = Coordinator::instance().add_sink(
            [this](const SampleEvent& e) { buffer.push_back(e); });
    }
    ~AndroidBridgeFixture() {
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

TEST_CASE("Android pulp_motion shim is a no-op when tracing is disabled",
          "[motion][android-bridge]") {
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

TEST_CASE("Android pulp_motion_publish_value routes to the Coordinator",
          "[motion][android-bridge]") {
    AndroidBridgeFixture fx;
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

TEST_CASE("Android pulp_motion_publish_components emits sorted multi-component events",
          "[motion][android-bridge]") {
    AndroidBridgeFixture fx;

    // Keys deliberately out of alphabetical order — the coordinator
    // sorts inside publish_components so the emitted event lines stay
    // stable regardless of caller key order.
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

TEST_CASE("Android pulp_motion_publish_components tolerates empty / null inputs",
          "[motion][android-bridge]") {
    AndroidBridgeFixture fx;

    // count == 0: nothing emitted, no crash.
    const char* keys[1] = {"x"};
    const double vals[1] = {1.0};
    pulp_motion_publish_components("Card", "p", keys, vals, 0, 0.001, 3);

    // null keys / values: short-circuits.
    pulp_motion_publish_components("Card", "p", nullptr, vals, 1, 0.001, 3);
    pulp_motion_publish_components("Card", "p", keys, nullptr, 1, 0.001, 3);

    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Baseline, "p") == 0);
}

// ── Ambient provenance ────────────────────────────────────────────────

TEST_CASE("Android pulp_motion_set_ambient_provenance stamps subsequent publishes",
          "[motion][android-bridge]") {
    AndroidBridgeFixture fx;

    pulp_motion_set_ambient_provenance("android", "CardView",
                                       "MotionProbe.kt", 99);
    pulp_motion_publish_value("Card", "opacity", 0.0, 0.001, 3);
    pulp_motion_publish_value("Card", "opacity", 1.0, 0.001, 3);

    const auto* baseline = first_of(fx.buffer, SampleEvent::Kind::Baseline, "opacity");
    REQUIRE(baseline != nullptr);
    REQUIRE(baseline->provenance.source_kind == "android");
    REQUIRE(baseline->provenance.source_id == "CardView");
    REQUIRE(baseline->provenance.source_file == "MotionProbe.kt");
    REQUIRE(baseline->provenance.source_line == 99);

    pulp_motion_clear_ambient_provenance();
    pulp_motion_publish_value("Card2", "opacity", 0.0, 0.001, 3);
    pulp_motion_publish_value("Card2", "opacity", 1.0, 0.001, 3);

    const SampleEvent* card2_baseline = nullptr;
    for (const auto& e : fx.buffer) {
        if (e.kind == SampleEvent::Kind::Baseline && e.view_name == "Card2") {
            card2_baseline = &e;
            break;
        }
    }
    REQUIRE(card2_baseline != nullptr);
    REQUIRE(card2_baseline->provenance.source_kind.empty());
}

// ── Geometry trace register / update / detach ─────────────────────────

TEST_CASE("Android pulp_motion_register_geometry_trace round-trips through FrameClock",
          "[motion][android-bridge]") {
    AndroidBridgeFixture fx;

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

    // TraceStarted should carry the auto-stamped provenance — the
    // Android bridge uses source_kind="android" (the Swift bridge
    // uses "swiftui"). This is the only externally-visible difference
    // between the two facades.
    const auto* started = first_of(fx.buffer, SampleEvent::Kind::TraceStarted);
    REQUIRE(started != nullptr);
    REQUIRE(started->provenance.source_kind == "android");
    REQUIRE(started->provenance.source_id == "CardView");

    // Detach: subsequent updates must not emit a new burst.
    const std::size_t pre_detach = fx.buffer.size();
    pulp_motion_detach_trace(id);
    pulp_motion_update_geometry(id, "frame", 999.0, 999.0, 999.0, 999.0);
    for (int i = 0; i < 10; ++i) fx.clock.tick(1.0f / 60.0f);

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

TEST_CASE("Android pulp_motion_update_geometry with a non-default metric also emits",
          "[motion][android-bridge]") {
    AndroidBridgeFixture fx;

    const int id = pulp_motion_register_geometry_trace("ScrollView", 60);
    REQUIRE(id > 0);

    // The Android bridge routes non-"frame" metrics through
    // publish_components on the "android" view name (Swift uses
    // "swiftui"); make sure we still get a Baseline for the metric.
    pulp_motion_update_geometry(id, "scroll", 0.0, 0.0, 320.0, 480.0);
    pulp_motion_update_geometry(id, "scroll", 0.0, 100.0, 320.0, 480.0);
    pulp_motion_update_geometry(id, "scroll", 0.0, 200.0, 320.0, 480.0);

    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Baseline, "scroll") >= 1);

    // The publish call uses view "android" (matches the source_kind
    // stamp); the registered trace itself sticks with the original
    // view name "ScrollView" for its own "frame" metric.
    const auto* scroll_baseline =
        first_of(fx.buffer, SampleEvent::Kind::Baseline, "scroll");
    REQUIRE(scroll_baseline != nullptr);
    REQUIRE(scroll_baseline->view_name == "android");

    pulp_motion_detach_trace(id);
}

// ── Coordinator::reset deadlock regression (Path G + Path H share this) ───
//
// If the TraceHandle ever moves back inside the shared_ptr the sampler
// captures, `Coordinator::reset()` will deadlock when destructing the
// spec set — the lambda's destruction releases the shared_ptr, which
// releases the TraceHandle, which calls Coordinator::detach() and
// re-takes the Coordinator's mutex while it is still held by reset().
// This test pins the fix.

TEST_CASE("Android bridge: Coordinator::reset() does not deadlock with registered geometry",
          "[motion][android-bridge]") {
    AndroidBridgeFixture fx;

    // Register two traces; pump some geometry into each so the
    // sampler lambdas hold non-trivial state.
    const int a = pulp_motion_register_geometry_trace("ViewA", 30);
    const int b = pulp_motion_register_geometry_trace("ViewB", 30);
    REQUIRE(a > 0);
    REQUIRE(b > 0);

    pulp_motion_update_geometry(a, "frame",  0.0, 0.0, 100.0, 100.0);
    pulp_motion_update_geometry(b, "frame", 10.0, 20.0, 50.0, 50.0);
    for (int i = 0; i < 3; ++i) fx.clock.tick(1.0f / 60.0f);

    // Reset the Coordinator WITHOUT first detaching. If the bridge's
    // registry holds the TraceHandle inside the shared_ptr the
    // sampler captures, this call will hang forever. The test
    // succeeds by virtue of returning — Catch2 imposes a per-test
    // timeout via CTest but the deadlock signature is "hang", not
    // "assert".
    Coordinator::instance().reset();

    // After reset, a fresh tracing session must still work — proves
    // we cleaned up cleanly rather than leaving the singleton in a
    // half-locked state.
    Coordinator::instance().bind(fx.clock);
    Coordinator::instance().set_firehose(true);
    Coordinator::instance().set_tracing_enabled(true);
    REQUIRE(pulp_motion_tracing_enabled());

    const int c = pulp_motion_register_geometry_trace("ViewC", 30);
    REQUIRE(c > 0);
    pulp_motion_detach_trace(c);
}
