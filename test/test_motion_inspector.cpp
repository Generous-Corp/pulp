/// @file test_motion_inspector.cpp
/// Catch2 integration tests for the inspector-side Motion.* domain.
/// Drives MotionInspector::handle() with synthesized requests against
/// a real View tree + FrameClock and asserts request/response shapes,
/// trace registration, and event broadcasting through a captured
/// InspectorServer broadcast sink.

#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/inspector_server.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <choc/text/choc_JSON.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using Catch::Approx;
using pulp::inspect::InspectorMessage;
using pulp::inspect::InspectorServer;
using pulp::inspect::MotionInspector;
using pulp::inspect::make_request;
using pulp::view::FrameClock;
using pulp::view::Rect;
using pulp::view::View;
using pulp::view::motion::Coordinator;

namespace {

/// Test scaffold: real View tree (parent + named child), real
/// FrameClock, a coordinator bound to it. The MotionInspector uses a
/// caller-supplied capture hook in place of a real InspectorServer
/// because TCP isn't allowed in unit tests.
struct Scaffold {
    Scaffold() {
        Coordinator::instance().reset();
        Coordinator::instance().bind(clock);

        parent.set_bounds({ 10.f, 20.f, 400.f, 300.f });
        auto child = std::make_unique<View>();
        child->set_id("card");
        child->set_bounds({ 5.f, 7.f, 100.f, 50.f });
        child_ptr = child.get();
        parent.add_child(std::move(child));

        // Server isn't actually started — we capture via a coordinator
        // sink that mimics what MotionInspector::broadcast_event sends.
        // For the integration test we use a real MotionInspector with
        // server = nullptr so no broadcast happens, and we observe
        // raw SampleEvents via a buffer sink.
        coord_sink_id = Coordinator::instance().add_sink(
            [this](const pulp::view::motion::SampleEvent& e) {
                std::lock_guard<std::mutex> lk(mtx);
                events.push_back(e);
            });
    }

    ~Scaffold() {
        if (coord_sink_id) Coordinator::instance().remove_sink(coord_sink_id);
        Coordinator::instance().reset();
    }

    FrameClock clock;
    View parent;
    View* child_ptr = nullptr;
    int coord_sink_id = 0;
    std::mutex mtx;
    std::vector<pulp::view::motion::SampleEvent> events;
};

/// Parse a response body that should contain a JSON object.
choc::value::Value parse_obj(const std::string& s) {
    return choc::json::parse(s);
}

}  // namespace

// ── Motion.startTrace basic happy path ────────────────────────────────

TEST_CASE("Motion.startTrace registers a geometry trace by node id", "[motion-inspector]") {
    Scaffold s;
    MotionInspector mi(s.parent, /*server=*/nullptr);

    const std::string params = R"({
        "view_name": "CardTrace",
        "fps": 60,
        "metrics": [
            {
                "kind": "geometry",
                "name": "frame",
                "node_id": "card",
                "properties": ["minX", "minY", "width", "height"],
                "space": "window",
                "source": "layout"
            }
        ]
    })";

    auto resp = mi.handle(make_request(7, "Motion.startTrace", params));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(resp.id == 7);
    auto body = parse_obj(resp.params_json);
    REQUIRE(body.hasObjectMember("trace_id"));
    REQUIRE(body["trace_id"].getInt64() >= 1);
    REQUIRE(mi.active_trace_count() == 1);
    REQUIRE(Coordinator::instance().active_trace_count() == 1);
    REQUIRE(Coordinator::instance().tracing_enabled());
}

// ── Tick produces motion events through the coordinator sink ──────────

TEST_CASE("Ticking the clock samples the registered geometry trace", "[motion-inspector]") {
    Scaffold s;
    MotionInspector mi(s.parent, /*server=*/nullptr);

    const std::string params = R"({
        "view_name": "CardTrace",
        "fps": 60,
        "metrics": [
            {
                "kind": "geometry",
                "name": "frame",
                "node_id": "card",
                "properties": ["minX", "minY", "width", "height"]
            }
        ]
    })";
    REQUIRE_FALSE(mi.handle(make_request(1, "Motion.startTrace", params)).is_error);

    // Tick 1: baseline.
    s.clock.tick(1.0f / 60.0f);
    // Mutate the child's bounds, tick to produce Start + Sample.
    s.child_ptr->set_bounds({ 5.f, 27.f, 100.f, 50.f });
    s.clock.tick(1.0f / 60.0f);
    // Stable: produces End on next tick.
    s.clock.tick(1.0f / 60.0f);

    std::size_t baseline_n = 0, start_n = 0, sample_n = 0, end_n = 0;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        for (const auto& e : s.events) {
            using K = pulp::view::motion::SampleEvent::Kind;
            if (e.view_name != "CardTrace" || e.metric_name != "frame") continue;
            if (e.kind == K::Baseline) ++baseline_n;
            if (e.kind == K::Start)    ++start_n;
            if (e.kind == K::Sample)   ++sample_n;
            if (e.kind == K::End)      ++end_n;
        }
    }
    REQUIRE(baseline_n == 1);
    REQUIRE(start_n == 1);
    REQUIRE(sample_n == 1);
    REQUIRE(end_n == 1);
}

// ── Stop trace removes the registration ───────────────────────────────

TEST_CASE("Motion.stopTrace removes the trace", "[motion-inspector]") {
    Scaffold s;
    MotionInspector mi(s.parent, /*server=*/nullptr);

    const std::string params = R"({
        "view_name": "X", "fps": 60,
        "metrics": [{ "kind": "geometry", "name": "f", "node_id": "card" }]
    })";
    auto resp = mi.handle(make_request(1, "Motion.startTrace", params));
    REQUIRE_FALSE(resp.is_error);
    const auto trace_id = parse_obj(resp.params_json)["trace_id"].getInt64();
    REQUIRE(mi.active_trace_count() == 1);

    std::ostringstream stop_params;
    stop_params << "{\"trace_id\":" << trace_id << "}";
    auto stop_resp = mi.handle(make_request(2, "Motion.stopTrace", stop_params.str()));
    REQUIRE_FALSE(stop_resp.is_error);
    REQUIRE(parse_obj(stop_resp.params_json)["removed"].getBool() == true);
    REQUIRE(mi.active_trace_count() == 0);
    REQUIRE(Coordinator::instance().active_trace_count() == 0);
}

// ── Unknown node id is a clean error ──────────────────────────────────

TEST_CASE("Motion.startTrace errors on unknown node id", "[motion-inspector]") {
    Scaffold s;
    MotionInspector mi(s.parent, /*server=*/nullptr);

    const std::string params = R"({
        "view_name": "X", "fps": 60,
        "metrics": [{ "kind": "geometry", "name": "f", "node_id": "no-such" }]
    })";
    auto resp = mi.handle(make_request(99, "Motion.startTrace", params));
    REQUIRE(resp.is_error);
    REQUIRE(mi.active_trace_count() == 0);
}

// ── Missing metrics array is an error ─────────────────────────────────

TEST_CASE("Motion.startTrace errors when metrics array missing", "[motion-inspector]") {
    Scaffold s;
    MotionInspector mi(s.parent, /*server=*/nullptr);
    auto resp = mi.handle(make_request(1, "Motion.startTrace",
                                       R"({"view_name":"X","fps":30})"));
    REQUIRE(resp.is_error);
}

// ── Snapshot returns coordinator state ────────────────────────────────

TEST_CASE("Motion.snapshot returns coordinator state", "[motion-inspector]") {
    Scaffold s;
    MotionInspector mi(s.parent, /*server=*/nullptr);

    const std::string start_params = R"({
        "view_name": "X", "fps": 60,
        "metrics": [{ "kind": "geometry", "name": "f", "node_id": "card" }]
    })";
    REQUIRE_FALSE(mi.handle(make_request(1, "Motion.startTrace", start_params)).is_error);

    auto snap = mi.handle(make_request(2, "Motion.snapshot"));
    REQUIRE_FALSE(snap.is_error);
    auto body = parse_obj(snap.params_json);
    REQUIRE(body["tracing_enabled"].getBool() == true);
    REQUIRE(body["active_traces"].getInt64() == 1);
    REQUIRE(body["inspector_traces"].getInt64() == 1);
}

// ── List traces returns registered ids ────────────────────────────────

TEST_CASE("Motion.listTraces returns active inspector trace ids", "[motion-inspector]") {
    Scaffold s;
    MotionInspector mi(s.parent, /*server=*/nullptr);

    const std::string p = R"({
        "view_name": "X", "fps": 60,
        "metrics": [{ "kind": "geometry", "name": "f", "node_id": "card" }]
    })";
    REQUIRE_FALSE(mi.handle(make_request(1, "Motion.startTrace", p)).is_error);
    REQUIRE_FALSE(mi.handle(make_request(2, "Motion.startTrace", p)).is_error);

    auto resp = mi.handle(make_request(3, "Motion.listTraces"));
    REQUIRE_FALSE(resp.is_error);
    auto body = parse_obj(resp.params_json);
    REQUIRE(body["trace_ids"].isArray());
    REQUIRE(body["trace_ids"].size() == 2);
}

// ── Unknown method is a clean error ───────────────────────────────────

TEST_CASE("Unknown Motion.* method returns an error", "[motion-inspector]") {
    Scaffold s;
    MotionInspector mi(s.parent, /*server=*/nullptr);
    auto resp = mi.handle(make_request(1, "Motion.nope"));
    REQUIRE(resp.is_error);
}

// ── Bug-sweep regression — scroll-geometry inspector route ────────────

TEST_CASE("Motion.startTrace registers a scroll-geometry trace by node id",
          "[motion-inspector][bug-sweep]") {
    Coordinator::instance().reset();
    FrameClock clock;
    Coordinator::instance().bind(clock);

    pulp::view::View parent;
    parent.set_bounds({0.f, 0.f, 400.f, 300.f});
    auto scroll = std::make_unique<pulp::view::ScrollView>();
    scroll->set_id("list");
    scroll->set_bounds({0.f, 0.f, 200.f, 200.f});
    scroll->set_content_size({800.f, 1600.f});
    parent.add_child(std::move(scroll));

    MotionInspector mi(parent, /*server=*/nullptr);

    // Default props (empty array) — exercises the empty-array branch.
    const std::string params = R"({
        "view_name": "Panel", "fps": 60,
        "metrics": [{ "kind": "scroll-geometry", "name": "scroll",
                      "node_id": "list" }]
    })";
    auto resp = mi.handle(make_request(1, "Motion.startTrace", params));
    REQUIRE_FALSE(resp.is_error);

    // Explicit camelCase property names — exercises parse_scroll_property.
    const std::string params2 = R"({
        "view_name": "Panel", "fps": 60,
        "metrics": [{ "kind": "scrollGeometry", "name": "scroll2",
                      "node_id": "list",
                      "properties": ["contentOffsetY", "visibleRectHeight"] }]
    })";
    auto resp2 = mi.handle(make_request(2, "Motion.startTrace", params2));
    REQUIRE_FALSE(resp2.is_error);

    // Pointing at a non-ScrollView surfaces a clean error rather than
    // dynamic_cast-then-crash.
    auto card = std::make_unique<pulp::view::View>();
    card->set_id("card");
    parent.add_child(std::move(card));
    const std::string params3 = R"({
        "view_name": "X", "fps": 60,
        "metrics": [{ "kind": "scroll-geometry", "name": "s", "node_id": "card" }]
    })";
    auto resp3 = mi.handle(make_request(3, "Motion.startTrace", params3));
    REQUIRE(resp3.is_error);
    REQUIRE(resp3.params_json.find("not a ScrollView") != std::string::npos);

    Coordinator::instance().reset();
}
