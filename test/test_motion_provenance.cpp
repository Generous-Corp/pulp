/// @file test_motion_provenance.cpp
/// Provenance adapters: confirm that each animation surface
/// (Tween, AnimatorSet, CSS TransitionSpec, JS rAF via ambient slot)
/// stamps an identifiable `motion::Provenance` envelope on the events
/// it publishes. An offline reader of the fixture can answer
/// "where does this animation live?" without grepping source.

#include <pulp/view/animation.hpp>
#include <pulp/view/animator_set.hpp>
#include <pulp/view/css_animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/script_engine.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace pulp::view;
using namespace pulp::view::motion;

namespace {

/// Wire a buffer sink to capture every emitted SampleEvent under the
/// firehose for the duration of a scope. RAII so reset always happens.
class MotionCapture {
public:
    MotionCapture() {
        Coordinator::instance().reset();
        Coordinator::instance().bind(clock_);
        Coordinator::instance().set_tracing_enabled(true);
        Coordinator::instance().set_firehose(true);
        sink_id_ = Coordinator::instance().add_sink(make_buffer_sink(&events_));
    }

    ~MotionCapture() {
        Coordinator::instance().reset();
    }

    void tick(float dt = 1.0f / 60.0f) { clock_.tick(dt); }
    FrameClock& clock() { return clock_; }
    std::vector<SampleEvent>& events() { return events_; }

private:
    FrameClock clock_;
    std::vector<SampleEvent> events_;
    int sink_id_ = 0;
};

/// Find the first event whose provenance source_kind matches `kind`.
const SampleEvent* find_event_by_kind(const std::vector<SampleEvent>& events,
                                      const std::string& kind) {
    for (const auto& e : events) {
        if (e.provenance.source_kind == kind) return &e;
    }
    return nullptr;
}

}  // namespace

// ── Tween + PULP_MOTION_TWEEN macro ─────────────────────────────────────

TEST_CASE("Tween::set_motion_provenance stamps publishes",
          "[motion][provenance][tween]") {
    MotionCapture cap;

    Tween t(0.0f, 1.0f, 0.1f, easing::linear);
    t.set_motion_provenance("tween", "GainKnob.opacity hover",
                            "/path/to/knob.cpp", 42);

    // Advance + publish a few ticks so a Baseline / Start / Sample fires.
    for (int i = 0; i < 12; ++i) {
        t.advance(1.0f / 60.0f);
        t.publish("Gain Knob", "opacity");
        cap.tick();
    }

    REQUIRE_FALSE(cap.events().empty());
    for (const auto& e : cap.events()) {
        REQUIRE(e.provenance.source_kind == "tween");
        REQUIRE(e.provenance.source_id == "GainKnob.opacity hover");
        REQUIRE(e.provenance.source_file == "/path/to/knob.cpp");
        REQUIRE(e.provenance.source_line == 42);
    }
}

TEST_CASE("PULP_MOTION_TWEEN macro auto-fills source_file/source_line",
          "[motion][provenance][tween][macro]") {
    MotionCapture cap;

    // Note the line number of the next statement — it'll be captured by
    // the macro and surface on every emitted event.
    auto t = PULP_MOTION_TWEEN("hover-glow", /*from*/0.0f, /*to*/1.0f,
                               /*duration*/0.1f, easing::linear);
    const int construction_line = __LINE__ - 2;  // line of the macro call

    for (int i = 0; i < 12; ++i) {
        t.advance(1.0f / 60.0f);
        t.publish("Card", "opacity");
        cap.tick();
    }

    REQUIRE_FALSE(cap.events().empty());
    const auto& first = cap.events().front();
    REQUIRE(first.provenance.source_kind == "tween");
    REQUIRE(first.provenance.source_id == "hover-glow");
    // `source_file` should end with this test file's name and the line
    // should point at (or very near) the macro invocation.
    REQUIRE(first.provenance.source_file.find("test_motion_provenance.cpp")
            != std::string::npos);
    // Allow ±2 since the macro lambda expansion can shift the line by
    // one when source_location is taken inside a default argument.
    REQUIRE(first.provenance.source_line >= construction_line - 1);
    REQUIRE(first.provenance.source_line <= construction_line + 2);
}

// ── AnimatorSetBuilder::name() ──────────────────────────────────────────

TEST_CASE("AnimatorSetBuilder::name() propagates as motion provenance",
          "[motion][provenance][animator-set]") {
    MotionCapture cap;

    float captured = 0.0f;
    auto runner = AnimatorSetBuilder()
                      .name("knob-glow")
                      .then(0.0f, 1.0f, 0.1f,
                            [&](float v) { captured = v; }, easing::linear)
                      .build_runner();

    // Advance until done, publishing each step's current scalar.
    for (int i = 0; i < 20; ++i) {
        runner.advance(1.0f / 60.0f);
        runner.publish("Card", "opacity", static_cast<double>(captured));
        cap.tick();
        if (runner.finished()) break;
    }

    REQUIRE_FALSE(cap.events().empty());
    const SampleEvent* e = find_event_by_kind(cap.events(), "animator-set");
    REQUIRE(e != nullptr);
    REQUIRE(e->provenance.source_id == "knob-glow");
}

// ── CSS TransitionSpec source attribution ───────────────────────────────

TEST_CASE("parse_transition_shorthand_with_provenance carries file/line",
          "[motion][provenance][css-transition]") {
    auto specs = parse_transition_shorthand_with_provenance(
        "opacity 200ms ease, transform 300ms ease-in 100ms",
        "/styles/card.css", 17);
    REQUIRE(specs.size() == 2);
    for (const auto& s : specs) {
        REQUIRE(s.source_file == "/styles/card.css");
        REQUIRE(s.source_line == 17);
    }
    REQUIRE(specs[0].property_name == "opacity");
    REQUIRE(specs[1].property_name == "transform");
}

TEST_CASE("CssAnimation publish stamps css-transition provenance",
          "[motion][provenance][css-transition]") {
    MotionCapture cap;

    auto specs = parse_transition_shorthand_with_provenance(
        "opacity 100ms linear", "/styles/card.css", 42);
    REQUIRE(specs.size() == 1);

    CssAnimation anim;
    anim.spec = specs[0];
    anim.property = specs[0].property;
    anim.start_value = 0.0f;
    anim.end_value = 1.0f;

    for (int i = 0; i < 12 && anim.active; ++i) {
        anim.tick(1.0f / 60.0f);
        anim.publish("Card", "opacity");
        cap.tick();
    }

    REQUIRE_FALSE(cap.events().empty());
    const SampleEvent* e = find_event_by_kind(cap.events(), "css-transition");
    REQUIRE(e != nullptr);
    REQUIRE(e->provenance.source_id == "opacity");
    REQUIRE(e->provenance.source_file == "/styles/card.css");
    REQUIRE(e->provenance.source_line == 42);
}

// ── Ambient provenance (rAF / design-import substrate) ──────────────────

TEST_CASE("Ambient provenance fills in when PublishOptions::provenance empty",
          "[motion][provenance][ambient]") {
    MotionCapture cap;

    Provenance p;
    p.source_kind = "rAF";
    p.source_id = "my-script.js:7";
    set_ambient_provenance(p);

    publish_value("Card", "opacity", 0.0);
    cap.tick();
    publish_value("Card", "opacity", 0.5);
    cap.tick();
    publish_value("Card", "opacity", 1.0);
    cap.tick();

    clear_ambient_provenance();

    REQUIRE_FALSE(cap.events().empty());
    for (const auto& e : cap.events()) {
        REQUIRE(e.provenance.source_kind == "rAF");
        REQUIRE(e.provenance.source_id == "my-script.js:7");
    }
}

TEST_CASE("Design-import provenance round-trips through publish channel",
          "[motion][provenance][design-import]") {
    MotionCapture cap;

    // Simulate a generated JS bundle calling motion.set_provenance(...)
    // by setting the ambient slot, then making publish calls — exactly
    // what `motion.publishValue` will do under the hood once the JS
    // bridge surface is exposed.
    Provenance p;
    p.source_kind = "design-import";
    p.source_id = "figma:Card/Hover";
    set_ambient_provenance(p);

    publish_value("Card", "opacity", 0.0);
    cap.tick();
    publish_value("Card", "opacity", 0.5);
    cap.tick();
    publish_value("Card", "opacity", 1.0);
    cap.tick();

    clear_ambient_provenance();

    // The current_ambient_provenance() round-trip itself.
    REQUIRE(current_ambient_provenance().source_kind.empty());

    REQUIRE_FALSE(cap.events().empty());
    const SampleEvent* e = find_event_by_kind(cap.events(), "design-import");
    REQUIRE(e != nullptr);
    REQUIRE(e->provenance.source_id == "figma:Card/Hover");
}

// ── WidgetBridge::load_script(code, script_id) + JS motion bindings ───

TEST_CASE("WidgetBridge load_script(code, script_id) tags rAF publishes",
          "[motion][provenance][widget-bridge][raf]") {
    MotionCapture cap;

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);

    REQUIRE(bridge.active_script_id().empty());

    // Script schedules an rAF that publishes a value. The bridge should
    // stamp the emitted event with source_kind="rAF" and source_id
    // "<script_id>:<callback_id>".
    bridge.load_script(
        "var __raf_published__ = false;"
        "requestAnimationFrame(function() {"
        "  motion.publishValue('Card', 'opacity', 0.5);"
        "  __raf_published__ = true;"
        "});",
        "my-script.js");

    REQUIRE(bridge.active_script_id() == "my-script.js");

    // Drain pending rAF callbacks. load_script's flush_frames invocation
    // already does this — but we also issue another flush so the test is
    // robust regardless of internal ordering.
    bridge.service_frame_callbacks();

    REQUIRE_FALSE(cap.events().empty());
    const SampleEvent* e = find_event_by_kind(cap.events(), "rAF");
    REQUIRE(e != nullptr);
    // source_id format is "<script_id>:<callback_id>" — we don't pin the
    // exact callback_id (it depends on JS-side allocator state), only
    // that it starts with the script id and a colon.
    REQUIRE(e->provenance.source_id.rfind("my-script.js:", 0) == 0);
}

TEST_CASE("motion.setProvenance + motion.publishValue round-trip from JS",
          "[motion][provenance][widget-bridge][js][design-import]") {
    MotionCapture cap;

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Simulate a design-import emitted bundle that tags its animation
    // with the vendor + node id before publishing.
    bridge.load_script(
        "motion.setProvenance('design-import', 'figma:Card/Hover');"
        "motion.publishValue('Card', 'opacity', 0.0);"
        "motion.publishValue('Card', 'opacity', 0.5);"
        "motion.publishValue('Card', 'opacity', 1.0);"
        "motion.clearProvenance();");

    cap.tick();

    REQUIRE_FALSE(cap.events().empty());
    const SampleEvent* e = find_event_by_kind(cap.events(), "design-import");
    REQUIRE(e != nullptr);
    REQUIRE(e->provenance.source_id == "figma:Card/Hover");

    // motion.clearProvenance must have actually cleared the ambient slot
    // so a follow-up publish doesn't inherit the stale envelope.
    REQUIRE(current_ambient_provenance().source_kind.empty());
}

// ── Backwards compat: publishes without provenance still work unchanged ─

TEST_CASE("Publishes without provenance still emit (backwards compatible)",
          "[motion][provenance][backcompat]") {
    MotionCapture cap;

    PublishOptions po{};  // empty provenance
    publish_value("Card", "opacity", 0.0, po);
    cap.tick();
    publish_value("Card", "opacity", 1.0, po);
    cap.tick();

    REQUIRE_FALSE(cap.events().empty());
    for (const auto& e : cap.events()) {
        REQUIRE_FALSE(e.provenance.is_set());
    }
}
