/// @file test_motion.cpp
/// Catch2 unit tests for pulp::view::motion (Phase 0: coordinator,
/// FrameClock binding, emission semantics, accumulator-gated FPS,
/// Start/Sample/End burst framing, deltas, sinks, geometry walker).

#include <pulp/view/motion.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <memory>
#include <string>
#include <vector>

using Catch::Approx;
using pulp::view::FrameClock;
using pulp::view::Rect;
using pulp::view::View;
using namespace pulp::view::motion;

namespace {

/// RAII fixture: resets the singleton coordinator, enables tracing,
/// binds a fresh FrameClock, and installs a buffer sink.
class Fixture {
public:
    Fixture() {
        Coordinator::instance().reset();
        Coordinator::instance().bind(clock);
        Coordinator::instance().set_tracing_enabled(true);
        sink_id = Coordinator::instance().add_sink(make_buffer_sink(&buffer));
    }
    ~Fixture() { Coordinator::instance().reset(); }

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

}  // namespace

// ── Binding ──────────────────────────────────────────────────────────

TEST_CASE("Coordinator binds and unbinds a FrameClock", "[motion]") {
    Fixture fx;
    REQUIRE(Coordinator::instance().is_bound());
    Coordinator::instance().unbind();
    REQUIRE_FALSE(Coordinator::instance().is_bound());
}

TEST_CASE("Coordinator off by default after reset", "[motion]") {
    Coordinator::instance().reset();
    REQUIRE_FALSE(Coordinator::instance().tracing_enabled());
    REQUIRE_FALSE(Coordinator::instance().is_bound());
    REQUIRE(Coordinator::instance().active_trace_count() == 0);
}

// ── Emission semantics ───────────────────────────────────────────────

TEST_CASE("Baseline emitted on first sample", "[motion]") {
    Fixture fx;
    double v = 1.0;
    auto handle = Coordinator::instance()
        .trace("Test", { /*fps=*/60 })
        .value("opacity", [&]{ return v; })
        .attach();

    fx.clock.tick(1.0f / 60.0f);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Baseline, "opacity") == 1);
    REQUIRE(fx.buffer.front().components.size() == 1);
    REQUIRE(fx.buffer.front().components.front().second == Approx(1.0));
}

TEST_CASE("No emission when value is stable", "[motion]") {
    Fixture fx;
    double v = 0.5;
    auto handle = Coordinator::instance()
        .trace("Test", { 60 })
        .value("opacity", [&]{ return v; })
        .attach();

    fx.clock.tick(1.0f / 60.0f);  // baseline
    const std::size_t after_baseline = fx.buffer.size();
    for (int i = 0; i < 10; ++i) fx.clock.tick(1.0f / 60.0f);
    REQUIRE(fx.buffer.size() == after_baseline);
}

TEST_CASE("Burst: Baseline -> Start -> Samples -> End with delta", "[motion]") {
    Fixture fx;
    double v = 0.0;
    auto handle = Coordinator::instance()
        .trace("Card", { 60 })
        .value("opacity", [&]{ return v; })
        .attach();

    fx.clock.tick(1.0f / 60.0f);              // baseline (v=0)
    v = 0.25; fx.clock.tick(1.0f / 60.0f);    // Start + Sample
    v = 0.50; fx.clock.tick(1.0f / 60.0f);    // Sample
    v = 0.75; fx.clock.tick(1.0f / 60.0f);    // Sample
    fx.clock.tick(1.0f / 60.0f);              // End (stable)

    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Baseline) == 1);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Start) == 1);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::Sample) == 3);
    REQUIRE(count_kind(fx.buffer, SampleEvent::Kind::End) == 1);

    const SampleEvent* end = nullptr;
    for (const auto& e : fx.buffer) {
        if (e.kind == SampleEvent::Kind::End) { end = &e; break; }
    }
    REQUIRE(end != nullptr);
    REQUIRE(end->deltas.size() == 1);
    REQUIRE(end->deltas.front().first == "value");
    REQUIRE(end->deltas.front().second == Approx(0.75));
}

TEST_CASE("Epsilon threshold suppresses jitter", "[motion]") {
    Fixture fx;
    double v = 1.0;
    auto handle = Coordinator::instance()
        .trace("Test", { 60 })
        .value("x", [&]{ return v; }, /*precision=*/3, /*epsilon=*/0.01)
        .attach();

    fx.clock.tick(1.0f / 60.0f);             // baseline
    const std::size_t base = fx.buffer.size();
    v = 1.005;
    for (int i = 0; i < 5; ++i) fx.clock.tick(1.0f / 60.0f);
    REQUIRE(fx.buffer.size() == base);       // below epsilon: silent
    v = 1.05;
    fx.clock.tick(1.0f / 60.0f);
    REQUIRE(fx.buffer.size() > base);        // above epsilon: emits
}

TEST_CASE("Monotonic FrameClock timestamps stamped on events", "[motion]") {
    Fixture fx;
    double v = 0.0;
    auto handle = Coordinator::instance()
        .trace("Test", { 60 })
        .value("x", [&]{ return v; })
        .attach();

    fx.clock.tick(1.0f / 60.0f);
    v = 1.0;
    fx.clock.tick(1.0f / 60.0f);
    v = 2.0;
    fx.clock.tick(1.0f / 60.0f);

    REQUIRE(fx.buffer.size() >= 3);
    double prev_t = -1.0;
    std::uint64_t prev_f = 0;
    for (const auto& e : fx.buffer) {
        REQUIRE(e.t_seconds >= prev_t);
        REQUIRE(e.frame >= prev_f);
        prev_t = e.t_seconds;
        prev_f = e.frame;
    }
}

// ── FPS gating ───────────────────────────────────────────────────────

TEST_CASE("FPS gating samples at requested rate via accumulator", "[motion]") {
    Fixture fx;
    double v = 0.0;
    auto handle = Coordinator::instance()
        .trace("Test", { /*fps=*/30 })
        .value("v", [&]{ v += 1.0; return v; })  // changes every sample
        .attach();

    // 60 ticks at 60 fps = 1.0 s. Expect ~30 sample-ish events
    // (Baseline + Samples), tolerating ±2 for FP drift.
    for (int i = 0; i < 60; ++i) fx.clock.tick(1.0f / 60.0f);

    const auto samples = count_kind(fx.buffer, SampleEvent::Kind::Sample)
                       + count_kind(fx.buffer, SampleEvent::Kind::Baseline);
    REQUIRE(samples >= 28);
    REQUIRE(samples <= 32);
}

// ── Multi-component metrics ──────────────────────────────────────────

TEST_CASE("multi() emits all components together", "[motion]") {
    Fixture fx;
    double x = 0.0, y = 0.0;
    auto handle = Coordinator::instance()
        .trace("Pointer", { 60 })
        .multi("xy", {
            {"x", [&]{ return x; }},
            {"y", [&]{ return y; }},
        })
        .attach();

    fx.clock.tick(1.0f / 60.0f);
    REQUIRE(fx.buffer.size() == 1);
    REQUIRE(fx.buffer.front().components.size() == 2);
    // Components are emitted sorted by name.
    REQUIRE(fx.buffer.front().components[0].first == "x");
    REQUIRE(fx.buffer.front().components[1].first == "y");
}

// ── Multiple sinks / independent traces ──────────────────────────────

TEST_CASE("Multiple traces are independent", "[motion]") {
    Fixture fx;
    double a = 0.0, b = 100.0;
    auto h1 = Coordinator::instance().trace("A", { 60 })
        .value("v", [&]{ return a; }).attach();
    auto h2 = Coordinator::instance().trace("B", { 60 })
        .value("v", [&]{ return b; }).attach();

    fx.clock.tick(1.0f / 60.0f);                  // both emit baseline
    a = 1.0;
    fx.clock.tick(1.0f / 60.0f);                  // A: Start + Sample
    fx.clock.tick(1.0f / 60.0f);                  // A: End

    std::size_t a_baseline = 0, a_start = 0, a_end = 0;
    std::size_t b_baseline = 0, b_start = 0, b_end = 0;
    for (const auto& e : fx.buffer) {
        if (e.view_name == "A") {
            if (e.kind == SampleEvent::Kind::Baseline) ++a_baseline;
            if (e.kind == SampleEvent::Kind::Start) ++a_start;
            if (e.kind == SampleEvent::Kind::End) ++a_end;
        } else if (e.view_name == "B") {
            if (e.kind == SampleEvent::Kind::Baseline) ++b_baseline;
            if (e.kind == SampleEvent::Kind::Start) ++b_start;
            if (e.kind == SampleEvent::Kind::End) ++b_end;
        }
    }
    REQUIRE(a_baseline == 1);
    REQUIRE(a_start == 1);
    REQUIRE(a_end == 1);
    REQUIRE(b_baseline == 1);
    REQUIRE(b_start == 0);
    REQUIRE(b_end == 0);
}

// ── Trace lifetime / sink lifetime ───────────────────────────────────

TEST_CASE("TraceHandle RAII detaches on destruction", "[motion]") {
    Fixture fx;
    {
        auto handle = Coordinator::instance()
            .trace("Ephemeral", { 60 })
            .value("x", []{ return 0.0; })
            .attach();
        REQUIRE(Coordinator::instance().active_trace_count() == 1);
    }
    REQUIRE(Coordinator::instance().active_trace_count() == 0);
}

TEST_CASE("Sink can be removed mid-stream", "[motion]") {
    Fixture fx;
    double v = 0.0;
    auto handle = Coordinator::instance().trace("X", { 60 })
        .value("v", [&]{ return v; }).attach();

    fx.clock.tick(1.0f / 60.0f);                  // baseline
    Coordinator::instance().remove_sink(fx.sink_id);
    const std::size_t base = fx.buffer.size();
    for (int i = 0; i < 5; ++i) {
        v += 1.0;
        fx.clock.tick(1.0f / 60.0f);
    }
    REQUIRE(fx.buffer.size() == base);
}

TEST_CASE("Tracing disabled produces no events", "[motion]") {
    Fixture fx;
    Coordinator::instance().set_tracing_enabled(false);
    double v = 0.0;
    auto handle = Coordinator::instance().trace("X", { 60 })
        .value("v", [&]{ return v; }).attach();
    fx.clock.tick(1.0f / 60.0f);
    REQUIRE(fx.buffer.empty());
}

// ── Geometry walker (Layout source) ──────────────────────────────────

TEST_CASE("geometry() samples view bounds in window space", "[motion]") {
    Fixture fx;
    View parent;
    parent.set_bounds({ 10.f, 20.f, 400.f, 300.f });
    auto child_owner = std::make_unique<View>();
    child_owner->set_bounds({ 5.f, 7.f, 100.f, 50.f });
    View* child = child_owner.get();
    parent.add_child(std::move(child_owner));

    auto handle = Coordinator::instance().trace("Child", { 60 })
        .geometry("frame", *child,
                  { GeometryProperty::MinX, GeometryProperty::MinY,
                    GeometryProperty::Width, GeometryProperty::Height },
                  GeometrySpace::Window, GeometrySource::Layout)
        .attach();

    fx.clock.tick(1.0f / 60.0f);
    REQUIRE(fx.buffer.size() == 1);
    const auto& comps = fx.buffer.front().components;
    REQUIRE(comps.size() == 4);
    // Components are sorted alphabetically (height, minX, minY, width).
    auto find = [&](const std::string& name) -> double {
        for (const auto& [k, v] : comps) if (k == name) return v;
        return 0.0;
    };
    REQUIRE(find("minX") == Approx(15.f));   // parent.x + child.x = 10 + 5
    REQUIRE(find("minY") == Approx(27.f));   // parent.y + child.y = 20 + 7
    REQUIRE(find("width") == Approx(100.f));
    REQUIRE(find("height") == Approx(50.f));
}

TEST_CASE("geometry() ViewLocal returns local-origin frame", "[motion]") {
    Fixture fx;
    View v;
    v.set_bounds({ 50.f, 60.f, 200.f, 100.f });
    auto handle = Coordinator::instance().trace("V", { 60 })
        .geometry("frame", v,
                  { GeometryProperty::MinX, GeometryProperty::MinY,
                    GeometryProperty::Width, GeometryProperty::Height },
                  GeometrySpace::ViewLocal, GeometrySource::Layout)
        .attach();

    fx.clock.tick(1.0f / 60.0f);
    REQUIRE(fx.buffer.size() == 1);
    auto find = [&](const std::string& name) -> double {
        for (const auto& [k, val] : fx.buffer.front().components)
            if (k == name) return val;
        return 0.0;
    };
    REQUIRE(find("minX") == Approx(0.f));
    REQUIRE(find("minY") == Approx(0.f));
    REQUIRE(find("width") == Approx(200.f));
    REQUIRE(find("height") == Approx(100.f));
}

// ── Line formatting ──────────────────────────────────────────────────

TEST_CASE("format_line produces canonical PulpMotion output", "[motion]") {
    SampleEvent e;
    e.kind = SampleEvent::Kind::Sample;
    e.view_name = "Card";
    e.metric_name = "frame";
    e.precision = 2;
    // Components must already be sorted (the coordinator does this).
    e.components = {
        {"height", 80.0}, {"minX", 12.0}, {"minY", 34.0}, {"width", 120.0},
    };
    const auto s = format_line(e);
    REQUIRE(s == "[PulpMotion][Card][frame] height=80.00 minX=12.00 minY=34.00 width=120.00");
}

TEST_CASE("format_line Start/End markers include frame + time", "[motion]") {
    SampleEvent start;
    start.kind = SampleEvent::Kind::Start;
    start.view_name = "X";
    start.metric_name = "y";
    start.frame = 42;
    start.t_seconds = 1.5;
    REQUIRE(format_line(start) == "[PulpMotion][X][y] -- Start frame=42 t=1.500000 --");

    SampleEvent end;
    end.kind = SampleEvent::Kind::End;
    end.view_name = "X";
    end.metric_name = "y";
    end.frame = 50;
    end.t_seconds = 2.0;
    end.precision = 2;
    end.deltas = { {"x", 10.0}, {"y", -5.0} };
    REQUIRE(format_line(end) ==
            "[PulpMotion][X][y] -- End frame=50 t=2.000000 -- xDelta=10.00 yDelta=-5.00");
}

// ── Emitted-event counter ────────────────────────────────────────────

// ── Presentation geometry walker (Phase 2) ───────────────────────────

namespace {
double find_component(const std::vector<std::pair<std::string, double>>& comps,
                      const std::string& name) {
    for (const auto& [k, v] : comps) if (k == name) return v;
    return 0.0;
}
}  // namespace

TEST_CASE("Presentation: uniform scale expands AABB around transform origin",
          "[motion][presentation]") {
    Fixture fx;
    View v;
    v.set_bounds({ 100.f, 100.f, 100.f, 50.f });
    v.set_transform_origin(0.5f, 0.5f);
    v.set_scale(2.0f);

    auto handle = Coordinator::instance().trace("V", { 60 })
        .geometry("frame", v,
                  { GeometryProperty::MinX, GeometryProperty::MinY,
                    GeometryProperty::Width, GeometryProperty::Height },
                  GeometrySpace::Window, GeometrySource::Presentation)
        .attach();
    fx.clock.tick(1.0f / 60.0f);
    REQUIRE(fx.buffer.size() == 1);
    const auto& comps = fx.buffer.front().components;
    // Scale 2 around the center (150, 125) → AABB is twice as wide/tall,
    // centered on the same point. So minX = 150 - 100 = 50, minY = 125 - 50 = 75.
    REQUIRE(find_component(comps, "minX")   == Approx(50.f).margin(0.01));
    REQUIRE(find_component(comps, "minY")   == Approx(75.f).margin(0.01));
    REQUIRE(find_component(comps, "width")  == Approx(200.f).margin(0.01));
    REQUIRE(find_component(comps, "height") == Approx(100.f).margin(0.01));
}

TEST_CASE("Layout source ignores paint-time scale", "[motion][presentation]") {
    Fixture fx;
    View v;
    v.set_bounds({ 100.f, 100.f, 100.f, 50.f });
    v.set_transform_origin(0.5f, 0.5f);
    v.set_scale(2.0f);

    auto handle = Coordinator::instance().trace("V", { 60 })
        .geometry("frame", v,
                  { GeometryProperty::MinX, GeometryProperty::MinY,
                    GeometryProperty::Width, GeometryProperty::Height },
                  GeometrySpace::Window, GeometrySource::Layout)
        .attach();
    fx.clock.tick(1.0f / 60.0f);
    const auto& comps = fx.buffer.front().components;
    REQUIRE(find_component(comps, "minX")   == Approx(100.f));
    REQUIRE(find_component(comps, "minY")   == Approx(100.f));
    REQUIRE(find_component(comps, "width")  == Approx(100.f));
    REQUIRE(find_component(comps, "height") == Approx(50.f));
}

TEST_CASE("Presentation: translate shifts the AABB", "[motion][presentation]") {
    Fixture fx;
    View v;
    v.set_bounds({ 50.f, 60.f, 100.f, 100.f });
    v.set_translate(10.f, 20.f);

    auto handle = Coordinator::instance().trace("V", { 60 })
        .geometry("frame", v,
                  { GeometryProperty::MinX, GeometryProperty::MinY,
                    GeometryProperty::Width, GeometryProperty::Height },
                  GeometrySpace::Window, GeometrySource::Presentation)
        .attach();
    fx.clock.tick(1.0f / 60.0f);
    const auto& comps = fx.buffer.front().components;
    REQUIRE(find_component(comps, "minX") == Approx(60.f).margin(0.01));
    REQUIRE(find_component(comps, "minY") == Approx(80.f).margin(0.01));
    REQUIRE(find_component(comps, "width")  == Approx(100.f));
    REQUIRE(find_component(comps, "height") == Approx(100.f));
}

TEST_CASE("Presentation: 90deg rotation swaps width/height in AABB",
          "[motion][presentation]") {
    Fixture fx;
    View v;
    v.set_bounds({ 0.f, 0.f, 100.f, 50.f });
    v.set_transform_origin(0.5f, 0.5f);
    v.set_rotation(90.f);

    auto handle = Coordinator::instance().trace("V", { 60 })
        .geometry("frame", v,
                  { GeometryProperty::Width, GeometryProperty::Height },
                  GeometrySpace::Window, GeometrySource::Presentation)
        .attach();
    fx.clock.tick(1.0f / 60.0f);
    const auto& comps = fx.buffer.front().components;
    // Width and height swap (with a small float tolerance).
    REQUIRE(find_component(comps, "width")  == Approx(50.f).margin(0.5));
    REQUIRE(find_component(comps, "height") == Approx(100.f).margin(0.5));
}

TEST_CASE("Presentation: 2D affine matrix (translate) applies",
          "[motion][presentation]") {
    Fixture fx;
    View v;
    v.set_bounds({ 0.f, 0.f, 100.f, 100.f });
    // Pure translate via 2D matrix: identity scale, e=10, f=20.
    v.set_transform_matrix(1.f, 0.f, 0.f, 1.f, 10.f, 20.f);

    auto handle = Coordinator::instance().trace("V", { 60 })
        .geometry("frame", v,
                  { GeometryProperty::MinX, GeometryProperty::MinY,
                    GeometryProperty::Width, GeometryProperty::Height },
                  GeometrySpace::Window, GeometrySource::Presentation)
        .attach();
    fx.clock.tick(1.0f / 60.0f);
    const auto& comps = fx.buffer.front().components;
    REQUIRE(find_component(comps, "minX") == Approx(10.f).margin(0.01));
    REQUIRE(find_component(comps, "minY") == Approx(20.f).margin(0.01));
    REQUIRE(find_component(comps, "width")  == Approx(100.f).margin(0.01));
    REQUIRE(find_component(comps, "height") == Approx(100.f).margin(0.01));
}

TEST_CASE("Presentation: parent scale propagates to child position",
          "[motion][presentation]") {
    Fixture fx;
    View parent;
    parent.set_bounds({ 0.f, 0.f, 200.f, 200.f });
    parent.set_transform_origin(0.0f, 0.0f);
    parent.set_scale(2.0f);

    auto child = std::make_unique<View>();
    child->set_bounds({ 10.f, 20.f, 30.f, 40.f });
    View* child_ptr = child.get();
    parent.add_child(std::move(child));

    auto handle = Coordinator::instance().trace("Child", { 60 })
        .geometry("frame", *child_ptr,
                  { GeometryProperty::MinX, GeometryProperty::MinY,
                    GeometryProperty::Width, GeometryProperty::Height },
                  GeometrySpace::Window, GeometrySource::Presentation)
        .attach();
    fx.clock.tick(1.0f / 60.0f);
    const auto& comps = fx.buffer.front().components;
    // Parent scaled 2x from (0,0): child (10,20) → (20,40), size 30x40 → 60x80.
    REQUIRE(find_component(comps, "minX")   == Approx(20.f).margin(0.01));
    REQUIRE(find_component(comps, "minY")   == Approx(40.f).margin(0.01));
    REQUIRE(find_component(comps, "width")  == Approx(60.f).margin(0.01));
    REQUIRE(find_component(comps, "height") == Approx(80.f).margin(0.01));
}

TEST_CASE("Presentation walker handles ScrollView ancestor offset",
          "[motion][presentation]") {
    Fixture fx;
    pulp::view::ScrollView scroll;
    scroll.set_bounds({ 10.f, 20.f, 200.f, 100.f });
    scroll.set_content_size({ 0.f, 600.f });

    auto item = std::make_unique<View>();
    item->set_bounds({ 0.f, 50.f, 100.f, 24.f });
    View* item_ptr = item.get();
    scroll.add_child(std::move(item));

    scroll.set_scroll(0.f, 20.f);
    // set_scroll animates by default; force the smoothed value to settle.
    scroll.advance_animations(10.0f);

    auto handle = Coordinator::instance().trace("Item", { 60 })
        .geometry("frame", *item_ptr,
                  { GeometryProperty::MinX, GeometryProperty::MinY,
                    GeometryProperty::Width, GeometryProperty::Height },
                  GeometrySpace::Window, GeometrySource::Presentation)
        .attach();
    fx.clock.tick(1.0f / 60.0f);
    const auto& comps = fx.buffer.front().components;
    // ScrollView contributes translate(10, 20) - translate(0, 20) = (10, 0)
    // before painting children. Item at (0, 50) → presentation (10, 50).
    REQUIRE(find_component(comps, "minX") == Approx(10.f).margin(0.5));
    REQUIRE(find_component(comps, "minY") == Approx(50.f).margin(0.5));
}

// ── Publish channel (Phase 3) ────────────────────────────────────────

TEST_CASE("publish_value is a no-op when firehose is off", "[motion][publish]") {
    Fixture fx;
    REQUIRE(Coordinator::instance().firehose() == false);
    publish_value("Card", "opacity", 0.5);
    publish_value("Card", "opacity", 0.6);
    REQUIRE(fx.buffer.empty());
}

TEST_CASE("publish_value with firehose emits a full Baseline/Start/Sample/End burst",
          "[motion][publish]") {
    Fixture fx;
    Coordinator::instance().set_firehose(true);

    publish_value("Card", "opacity", 0.5);   // Baseline
    publish_value("Card", "opacity", 0.6);   // Start + Sample
    publish_value("Card", "opacity", 0.6);   // stable → End
    publish_value("Card", "opacity", 0.6);   // still stable, no event

    std::size_t baseline = 0, start = 0, sample = 0, end = 0;
    for (const auto& e : fx.buffer) {
        if (e.kind == SampleEvent::Kind::Baseline) ++baseline;
        if (e.kind == SampleEvent::Kind::Start)    ++start;
        if (e.kind == SampleEvent::Kind::Sample)   ++sample;
        if (e.kind == SampleEvent::Kind::End)      ++end;
    }
    REQUIRE(baseline == 1);
    REQUIRE(start == 1);
    REQUIRE(sample == 1);
    REQUIRE(end == 1);
}

TEST_CASE("publish_value End burst fires on first stable publish",
          "[motion][publish]") {
    Fixture fx;
    Coordinator::instance().set_firehose(true);

    publish_value("X", "y", 0.0);   // Baseline
    publish_value("X", "y", 1.0);   // Start + Sample
    publish_value("X", "y", 1.0);   // stable → End

    std::size_t start = 0, end = 0;
    for (const auto& e : fx.buffer) {
        if (e.kind == SampleEvent::Kind::Start) ++start;
        if (e.kind == SampleEvent::Kind::End)   ++end;
    }
    REQUIRE(start == 1);
    REQUIRE(end == 1);
}

TEST_CASE("publish_components routes multiple keys independently",
          "[motion][publish]") {
    Fixture fx;
    Coordinator::instance().set_firehose(true);

    publish_components("Card", "frame", { {"x", 0.0}, {"y", 0.0} });
    publish_components("Toast", "alpha", { {"value", 1.0} });
    publish_components("Card", "frame", { {"x", 5.0}, {"y", 0.0} });

    std::size_t card_evts = 0, toast_evts = 0;
    for (const auto& e : fx.buffer) {
        if (e.view_name == "Card")  ++card_evts;
        if (e.view_name == "Toast") ++toast_evts;
    }
    REQUIRE(card_evts >= 3);  // Baseline + Start + Sample
    REQUIRE(toast_evts == 1); // Baseline only
}

TEST_CASE("publish_value respects tracing_enabled gate",
          "[motion][publish]") {
    Fixture fx;
    Coordinator::instance().set_firehose(true);
    Coordinator::instance().set_tracing_enabled(false);
    publish_value("Card", "opacity", 0.5);
    REQUIRE(fx.buffer.empty());
}

TEST_CASE("publish_value respects epsilon threshold",
          "[motion][publish]") {
    Fixture fx;
    Coordinator::instance().set_firehose(true);

    publish_value("X", "y", 1.0, {3, /*epsilon=*/0.05});  // Baseline
    publish_value("X", "y", 1.01);  // below epsilon → no Start/Sample
    publish_value("X", "y", 1.02);  // still below

    std::size_t change_events = 0;
    for (const auto& e : fx.buffer) {
        if (e.kind == SampleEvent::Kind::Start ||
            e.kind == SampleEvent::Kind::Sample) ++change_events;
    }
    REQUIRE(change_events == 0);

    publish_value("X", "y", 1.10);  // crosses epsilon → Start + Sample
    change_events = 0;
    for (const auto& e : fx.buffer) {
        if (e.kind == SampleEvent::Kind::Start ||
            e.kind == SampleEvent::Kind::Sample) ++change_events;
    }
    REQUIRE(change_events == 2);
}

TEST_CASE("Emitted event counter advances", "[motion]") {
    Fixture fx;
    double v = 0.0;
    auto handle = Coordinator::instance().trace("X", { 60 })
        .value("v", [&]{ return v; }).attach();
    REQUIRE(Coordinator::instance().emitted_event_count() == 0);
    fx.clock.tick(1.0f / 60.0f);
    REQUIRE(Coordinator::instance().emitted_event_count() >= 1);
}
