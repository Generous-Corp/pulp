// Meter/scalar host→view source — the paint-safe lock-free channel a Meter
// self-updates from on the FrameClock. Covers the source primitives and the
// subscription lifecycle across the construction orders real hosts use
// (build-then-clock, clock-then-bind, graft-under-clock, detach).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>

#include <pulp/canvas/canvas.hpp>

#include "harness/rt_allocation_probe.hpp"
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/value_source.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

using namespace pulp::view;

namespace {
MeterFrame stereo(float rms, float peak) {
    MeterFrame f;
    f.channels = 2;
    f.rms[0] = f.rms[1] = rms;
    f.peak[0] = f.peak[1] = peak;
    return f;
}
}  // namespace

TEST_CASE("MeterSource publishes the latest frame", "[view][meter-source]") {
    MeterSource src;
    src.publish(stereo(0.3f, 0.6f));
    const MeterFrame got = src.read();
    CHECK(got.channels == 2);
    CHECK(got.rms[0] == Catch::Approx(0.3f));
    CHECK(got.peak[1] == Catch::Approx(0.6f));

    src.publish(stereo(0.1f, 0.9f));  // latest wins
    CHECK(src.read().peak[0] == Catch::Approx(0.9f));
}

TEST_CASE("ScalarSource publishes the latest value", "[view][meter-source]") {
    ScalarSource s;
    CHECK(s.read() == Catch::Approx(0.0f));  // initial
    s.publish(0.42f);
    CHECK(s.read() == Catch::Approx(0.42f));
    s.publish(-1.0f);
    CHECK(s.read() == Catch::Approx(-1.0f));
}

TEST_CASE("a source-bound Meter subscribes when the clock is installed AFTER build",
          "[view][meter-source][lifecycle]") {
    // The order real Pulp hosts use: build the tree + bind the source, THEN
    // install the frame clock on the root. The propagation hook must reach the
    // meter or it silently never updates.
    auto root = std::make_unique<View>();
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));  // on_attached fires now — no clock yet

    auto src = std::make_shared<MeterSource>();
    m->set_source(src, 0);

    FrameClock clock;
    REQUIRE_FALSE(clock.has_active_subscribers());
    root->set_frame_clock(&clock);  // propagates → the meter subscribes
    REQUIRE(clock.has_active_subscribers());

    src->publish(stereo(0.8f, 0.9f));
    clock.tick(0.016f);
    CHECK(m->display_peak() > 0.0f);  // ballistic path moved toward the level
}

TEST_CASE("a source bound AFTER the clock exists still subscribes",
          "[view][meter-source][lifecycle]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));
    REQUIRE_FALSE(clock.has_active_subscribers());  // no source yet
    m->set_source(std::make_shared<MeterSource>(), 0);
    REQUIRE(clock.has_active_subscribers());
}

TEST_CASE("grafting a pre-built source-bound subtree under a clocked root subscribes",
          "[view][meter-source][lifecycle]") {
    // Build container + meter + source entirely offline, then graft into an
    // already-clocked root. add_child must notify the grafted subtree.
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);

    auto container = std::make_unique<View>();
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    container->add_child(std::move(meter));
    m->set_source(std::make_shared<MeterSource>(), 0);  // no clock reachable yet
    REQUIRE_FALSE(clock.has_active_subscribers());

    root->add_child(std::move(container));  // graft under the clock
    REQUIRE(clock.has_active_subscribers());
}

TEST_CASE("unbinding a Meter's source unsubscribes", "[view][meter-source][lifecycle]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));
    m->set_source(std::make_shared<MeterSource>(), 0);
    REQUIRE(clock.has_active_subscribers());
    m->set_source(nullptr);
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("detaching a descendant Meter drops its subscription",
          "[view][meter-source][lifecycle]") {
    // The meter is a DESCENDANT of the removed node, so remove_child never fires
    // on_detached on it — the clock-change notification must still unsubscribe it.
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto container = std::make_unique<View>();
    View* c = container.get();
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    container->add_child(std::move(meter));
    root->add_child(std::move(container));
    m->set_source(std::make_shared<MeterSource>(), 0);
    REQUIRE(clock.has_active_subscribers());

    auto removed = root->remove_child(c);  // detaches container + descendant meter
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("a Meter ignores an out-of-range source channel", "[view][meter-source]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));
    auto src = std::make_shared<MeterSource>();
    m->set_source(src, 5);  // channel 5, frames only carry 2
    src->publish(stereo(0.8f, 0.9f));
    clock.tick(0.016f);
    CHECK(m->display_peak() == Catch::Approx(0.0f));  // untouched
}

TEST_CASE("a destroyed source-bound Meter leaves no dangling FrameClock subscriber",
          "[view][meter-source][lifecycle]") {
    FrameClock clock;
    auto root = std::make_unique<View>();
    root->set_frame_clock(&clock);
    {
        auto meter = std::make_unique<Meter>();
        meter->set_source(std::make_shared<MeterSource>(), 0);
        Meter* m = meter.get();
        root->add_child(std::move(meter));
        REQUIRE(clock.has_active_subscribers());
        root->remove_child(m);  // returns the owner; dropped at end of scope → ~Meter
    }
    // ~Meter must have unsubscribed; ticking must not touch freed memory.
    clock.tick(0.016f);
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("a Meter never reads out of bounds on a malformed channel count",
          "[view][meter-source]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));
    auto src = std::make_shared<MeterSource>();
    m->set_source(src, MeterFrame::kMaxChannels);  // bind a channel at capacity

    MeterFrame bad;             // malformed: claims far more channels than exist
    bad.channels = 999;
    bad.peak[0] = 0.9f;
    bad.rms[0] = 0.8f;
    src->publish(bad);
    clock.tick(0.016f);         // must bound the index by kMaxChannels — no OOB read
    CHECK(m->display_peak() == Catch::Approx(0.0f));  // at-capacity index → ignored
}

TEST_CASE("a live Meter re-points when moved between two FrameClocks",
          "[view][meter-source][lifecycle]") {
    FrameClock clock_a, clock_b;
    auto root_a = std::make_unique<View>();
    root_a->set_frame_clock(&clock_a);
    auto root_b = std::make_unique<View>();
    root_b->set_frame_clock(&clock_b);

    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root_a->add_child(std::move(meter));
    m->set_source(std::make_shared<MeterSource>(), 0);
    REQUIRE(clock_a.has_active_subscribers());
    REQUIRE_FALSE(clock_b.has_active_subscribers());

    auto moved = root_a->remove_child(m);   // detaches from A → unsubscribes from A
    REQUIRE_FALSE(clock_a.has_active_subscribers());
    root_b->add_child(std::move(moved));    // graft under B → subscribes to B
    REQUIRE(clock_b.has_active_subscribers());
}

TEST_CASE("clearing the root clock unsubscribes a live Meter before teardown",
          "[view][meter-source][lifecycle]") {
    FrameClock clock;
    auto root = std::make_unique<View>();
    root->set_frame_clock(&clock);
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));
    m->set_source(std::make_shared<MeterSource>(), 0);
    REQUIRE(clock.has_active_subscribers());

    // The host clears the clock before destroying it (Pulp's GPU hosts do this).
    // The meter must drop its subscription while the clock is still alive.
    root->set_frame_clock(nullptr);
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("TripleBuffer<MeterFrame> never tears a frame under a 2-thread hammer",
          "[view][meter-source][rt]") {
    MeterSource src;
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};

    std::thread writer([&] {
        for (int k = 1; k < 200000 && !stop.load(); ++k) {
            const float v = static_cast<float>(k % 1000) / 1000.0f;
            src.publish(stereo(v, v));  // every field equals v — a coherent frame
        }
        stop.store(true);
    });

    while (!stop.load()) {
        const MeterFrame f = src.read();
        // A coherent frame has all fields equal; a torn read would mix two writes.
        if (f.channels == 2 &&
            (f.rms[0] != f.rms[1] || f.rms[0] != f.peak[0] || f.peak[0] != f.peak[1])) {
            torn.fetch_add(1);
        }
    }
    writer.join();
    CHECK(torn.load() == 0);
}

// ── Idle gate ────────────────────────────────────────────────────────────────
// A source-bound Meter is a FrameClock subscriber, so it runs every vsync for as
// long as it is bound — including through silence, when the ballistics have
// already settled and the next frame would be pixel-identical. Repainting anyway
// costs a composite per meter per vsync forever, and on the plug-in-view-host
// path each of those is a FULL-surface repaint of the whole editor. So a still
// frame must request nothing at all.

namespace {

// A WindowHost that counts repaint requests. request_repaint(Rect) routes
// through WindowHost::mark_dirty(rect) → repaint() when no RenderLoop is
// attached, so this counts exactly the repaints a Meter asks for.
class RepaintCountingHost : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override { ++repaints; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
    int repaints = 0;
};

}  // namespace

TEST_CASE("a silent source-bound Meter settles to zero repaints per second",
          "[view][meter-source][idle-gate]") {
    RepaintCountingHost host;
    FrameClock clock;
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 200, 100});
    root->set_window_host(&host);

    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    m->set_bounds({10, 10, 20, 80});
    root->add_child(std::move(meter));

    auto src = std::make_shared<MeterSource>();
    m->set_source(src, 0);
    root->set_frame_clock(&clock);
    REQUIRE(clock.has_active_subscribers());

    // Silence from the very first frame: the ballistics never leave zero, so no
    // frame ever differs from the last and nothing is ever invalidated.
    src->publish(stereo(0.0f, 0.0f));
    for (int i = 0; i < 120; ++i) clock.tick(1.0f / 60.0f);  // 2 seconds @ 60 Hz
    CHECK(host.repaints == 0);
    CHECK(m->display_peak() == Catch::Approx(0.0f));

    // A real level arrives: the very next frame invalidates.
    src->publish(stereo(0.5f, 0.8f));
    clock.tick(1.0f / 60.0f);
    CHECK(host.repaints >= 1);
    CHECK(m->display_peak() > 0.0f);

    // Let the ballistics decay all the way back down, then confirm the meter
    // goes quiet again rather than repainting forever.
    src->publish(stereo(0.0f, 0.0f));
    for (int i = 0; i < 600; ++i) clock.tick(1.0f / 60.0f);  // 10 s of silence
    const int settled = host.repaints;
    for (int i = 0; i < 120; ++i) clock.tick(1.0f / 60.0f);
    CHECK(host.repaints == settled);   // 0 additional paints while silent

    root->set_frame_clock(nullptr);    // drop the subscription before teardown
}

// ── The generic View seam ───────────────────────────────────────────────────
// A plugin UI that paints its own meters is not a `Meter` — it is a plain View
// (a DesignFrameView subclass, typically) that binds a source and reads the
// snapshot from paint(). These cover that shape directly.

namespace {

// The end-to-end shape a native plugin UI uses: bind a source, read the cached
// frame from paint(). Reads BOTH channels of the frame, since one view painting
// several meters (a compressor's gain-reduction meter plus its output strip) is
// the case a single per-view channel cannot serve.
class MeterPanel : public View {
public:
    void paint(pulp::canvas::Canvas&) override {
        const MeterFrame& f = meter_frame();
        ++paints;
        painted_channels = f.channels;
        painted_peak0 = f.channels > 0 ? f.peak[0] : -1.0f;
        painted_rms1 = f.channels > 1 ? f.rms[1] : -1.0f;
        painted_scalar = scalar_value();
    }
    int paints = 0;
    int painted_channels = -1;
    float painted_peak0 = -1.0f;
    float painted_rms1 = -1.0f;
    float painted_scalar = -1.0f;
};

}  // namespace

TEST_CASE("a View subclass reads a published meter frame from paint()",
          "[view][meter-source]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto panel = std::make_unique<MeterPanel>();
    MeterPanel* p = panel.get();
    root->add_child(std::move(panel));

    auto src = std::make_shared<MeterSource>();
    p->set_meter_source(src, 0);
    REQUIRE(p->has_meter_source());
    REQUIRE(clock.has_active_subscribers());

    pulp::canvas::RecordingCanvas canvas;

    // Nothing published yet: paint sees a zeroed frame, never stale garbage.
    p->paint(canvas);
    CHECK(p->painted_channels == 0);

    // The host publishes on its own thread; the view sees it after a tick.
    src->publish(stereo(0.25f, 0.75f));
    CHECK(p->meter_frame().channels == 0);  // snapshot happens on the clock, not on publish
    clock.tick(0.016f);
    p->paint(canvas);
    CHECK(p->painted_channels == 2);
    CHECK(p->painted_peak0 == Catch::Approx(0.75f));
    CHECK(p->painted_rms1 == Catch::Approx(0.25f));

    // Latest-wins across ticks.
    src->publish(stereo(0.1f, 0.2f));
    clock.tick(0.016f);
    p->paint(canvas);
    CHECK(p->painted_peak0 == Catch::Approx(0.2f));

    // Unbinding drops the snapshot: a view that kept painting the last reading
    // would show a meter frozen at a level the plugin no longer produces.
    p->set_meter_source(nullptr);
    CHECK_FALSE(p->has_meter_source());
    CHECK_FALSE(clock.has_active_subscribers());
    CHECK(p->meter_frame().channels == 0);
    p->paint(canvas);
    CHECK(p->painted_channels == 0);

    root->set_frame_clock(nullptr);
}

TEST_CASE("a View subclass reads a published scalar from paint()",
          "[view][meter-source]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto panel = std::make_unique<MeterPanel>();
    MeterPanel* p = panel.get();
    root->add_child(std::move(panel));

    auto s = std::make_shared<ScalarSource>();
    p->set_scalar_source(s);
    REQUIRE(p->has_scalar_source());
    REQUIRE(clock.has_active_subscribers());

    pulp::canvas::RecordingCanvas canvas;
    s->publish(0.6f);
    clock.tick(0.016f);
    p->paint(canvas);
    CHECK(p->painted_scalar == Catch::Approx(0.6f));

    s->publish(-0.25f);
    clock.tick(0.016f);
    p->paint(canvas);
    CHECK(p->painted_scalar == Catch::Approx(-0.25f));

    // Unbinding stops the channel and releases the editor's frames.
    p->set_scalar_source(nullptr);
    CHECK_FALSE(p->has_scalar_source());
    CHECK_FALSE(clock.has_active_subscribers());
    CHECK(p->scalar_value() == Catch::Approx(0.0f));

    root->set_frame_clock(nullptr);
}

TEST_CASE("an unbound View reads a zeroed frame and a zero scalar",
          "[view][meter-source]") {
    // The getters must be safe on the overwhelming majority of views, which
    // never bind anything and never allocate a binding.
    MeterPanel p;
    CHECK_FALSE(p.has_meter_source());
    CHECK_FALSE(p.has_scalar_source());
    CHECK(p.meter_frame().channels == 0);
    CHECK(p.scalar_value() == Catch::Approx(0.0f));
    p.set_meter_source(nullptr);  // unbinding what was never bound is a no-op
    p.set_scalar_source(nullptr);
    CHECK(p.meter_frame().channels == 0);
}

TEST_CASE("a live View binding re-points when moved between two FrameClocks",
          "[view][meter-source][lifecycle]") {
    // The self-heal path, exercised through the generic seam rather than Meter:
    // the same machinery must survive a re-parent for a plain View.
    FrameClock clock_a, clock_b;
    auto root_a = std::make_unique<View>();
    root_a->set_frame_clock(&clock_a);
    auto root_b = std::make_unique<View>();
    root_b->set_frame_clock(&clock_b);

    auto panel = std::make_unique<MeterPanel>();
    MeterPanel* p = panel.get();
    root_a->add_child(std::move(panel));
    auto src = std::make_shared<MeterSource>();
    p->set_meter_source(src, 0);
    REQUIRE(clock_a.has_active_subscribers());

    auto moved = root_a->remove_child(p);
    REQUIRE_FALSE(clock_a.has_active_subscribers());
    root_b->add_child(std::move(moved));
    REQUIRE(clock_b.has_active_subscribers());

    // The new clock — not the old one — now drives the snapshot.
    src->publish(stereo(0.4f, 0.5f));
    clock_a.tick(0.016f);
    CHECK(p->meter_frame().channels == 0);  // A is no longer this view's clock
    clock_b.tick(0.016f);
    CHECK(p->meter_frame().peak[0] == Catch::Approx(0.5f));

    root_b->set_frame_clock(nullptr);
}

TEST_CASE("a destroyed source-bound View leaves no dangling FrameClock subscriber",
          "[view][meter-source][lifecycle]") {
    FrameClock clock;
    auto root = std::make_unique<View>();
    root->set_frame_clock(&clock);
    {
        auto panel = std::make_unique<MeterPanel>();
        MeterPanel* p = panel.get();
        panel->set_meter_source(std::make_shared<MeterSource>(), 0);
        panel->set_scalar_source(std::make_shared<ScalarSource>());
        root->add_child(std::move(panel));
        REQUIRE(clock.has_active_subscribers());
        root->remove_child(p);  // returned owner is discarded → panel destroyed
    }
    CHECK_FALSE(clock.has_active_subscribers());
    clock.tick(0.016f);  // must not call into the destroyed view
}

// ── Paint-safety ────────────────────────────────────────────────────────────
// The whole point of snapshotting on the clock is that paint() reads a plain
// cached member: no lock, no allocation, no blocking. A regression here (say,
// reading the TripleBuffer or building a container in the getter) is invisible
// in behavior tests and only shows up as an audible glitch in a real host, so
// assert it directly with the shared allocation probe.

namespace {
// Escape hatches that stop the optimizer eliding the positive control's
// allocation. A new-expression whose result the compiler can prove unused is
// elidable outright at -O3, so the size comes from a volatile (the compiler
// cannot fold the request away) and the pointer escapes through a volatile sink
// (it cannot prove the storage is dead). Without both, the control silently
// measures nothing and every zero-allocation assertion below becomes vacuous.
volatile std::size_t g_alloc_count = 32;
void* volatile g_alloc_sink = nullptr;
}  // namespace

TEST_CASE("the allocation probe observes a real allocation on this thread",
          "[view][meter-source][rt-safety]") {
    // Positive control for the probe itself. Without this, "0 allocations"
    // could mean "paint is clean" OR "the interposer was never linked" — and
    // the two are indistinguishable from a passing test.
    std::size_t seen = 0;
    {
        pulp::test::RtAllocationProbe probe;
        float* buf = new float[g_alloc_count];
        g_alloc_sink = buf;
        delete[] buf;
        seen = probe.allocation_count();
    }
    CHECK(seen >= 1);
}

TEST_CASE("reading a bound meter and scalar from paint() never allocates",
          "[view][meter-source][rt-safety]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto panel = std::make_unique<MeterPanel>();
    MeterPanel* p = panel.get();
    root->add_child(std::move(panel));

    auto src = std::make_shared<MeterSource>();
    auto scalar = std::make_shared<ScalarSource>();
    p->set_meter_source(src, 0);
    p->set_scalar_source(scalar);

    pulp::canvas::RecordingCanvas canvas;
    src->publish(stereo(0.3f, 0.7f));
    clock.tick(0.016f);
    p->paint(canvas);  // warm any first-call state up OUTSIDE the probe

    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 64; ++i) p->paint(canvas);
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(p->painted_peak0 == Catch::Approx(0.7f));

    // An UNBOUND view takes the null-binding branch of the same getters — it
    // must not allocate a binding (or a static frame) just to read a zero.
    MeterPanel unbound;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 64; ++i) unbound.paint(canvas);
        CHECK(probe.allocation_count() == 0);
    }

    root->set_frame_clock(nullptr);
}

TEST_CASE("snapshotting a meter frame on the clock never allocates",
          "[view][meter-source][rt-safety]") {
    // The tick side matters too: it runs every frame on the UI thread, and a
    // MeterFrame is fixed-capacity + trivially copyable precisely so the
    // snapshot is a plain copy.
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto panel = std::make_unique<MeterPanel>();
    MeterPanel* p = panel.get();
    root->add_child(std::move(panel));
    p->set_meter_source(std::make_shared<MeterSource>(), 0);
    clock.tick(0.016f);  // first tick outside the probe

    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 64; ++i) clock.tick(0.016f);
        CHECK(probe.allocation_count() == 0);
    }
    root->set_frame_clock(nullptr);
}

// ── DesignFrameView per-element scalars ─────────────────────────────────────
// A DesignFrameView is ONE view painting MANY elements, so a modulation ring
// per macro knob needs a value per element — the view's own single scalar
// cannot carry it. Bindings are keyed by param_key because `elements_` is
// replaced wholesale on a frame swap.

namespace {

const char* kPanelSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">"
    "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#222\"/></svg>";

DesignFrameElement knob_with_key(const char* key, float cx) {
    DesignFrameElement e;
    e.kind = DesignFrameElement::Kind::knob;
    e.cx = cx;
    e.cy = 50.0f;
    e.hit_radius = 10.0f;
    e.param_key = key;
    return e;
}

// The rings case: a subclass painting a live modulated position per element.
class RingPanel : public DesignFrameView {
public:
    RingPanel(std::string svg, std::vector<DesignFrameElement> els)
        : DesignFrameView(std::move(svg), std::move(els)) {}
    // Deliberately does NOT chain to DesignFrameView::paint — this test is about
    // the scalar read, not the SVG raster.
    void paint(pulp::canvas::Canvas&) override {
        for (int i = 0; i < element_count() && i < kMaxRings; ++i)
            painted[i] = element_scalar(i);
    }
    static constexpr int kMaxRings = 4;
    float painted[kMaxRings] = {-1.0f, -1.0f, -1.0f, -1.0f};
};

}  // namespace

TEST_CASE("a DesignFrameView subclass paints a live scalar per element",
          "[view][meter-source][design-frame]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);

    std::vector<DesignFrameElement> els{knob_with_key("macro1", 25.0f),
                                        knob_with_key("macro2", 75.0f)};
    auto panel = std::make_unique<RingPanel>(kPanelSvg, els);
    RingPanel* p = panel.get();
    root->add_child(std::move(panel));

    auto lfo1 = std::make_shared<ScalarSource>();
    auto lfo2 = std::make_shared<ScalarSource>();
    p->set_element_scalar_source("macro1", lfo1);
    p->set_element_scalar_source("macro2", lfo2);
    REQUIRE(p->element_has_scalar_source(0));
    REQUIRE(p->element_has_scalar_source(1));
    REQUIRE(clock.has_active_subscribers());

    pulp::canvas::RecordingCanvas canvas;
    lfo1->publish(0.2f);
    lfo2->publish(0.9f);
    clock.tick(0.016f);
    p->paint(canvas);
    CHECK(p->painted[0] == Catch::Approx(0.2f));  // each ring reads its OWN source
    CHECK(p->painted[1] == Catch::Approx(0.9f));

    // An unknown key binds nothing; an empty key is never a valid identity.
    p->set_element_scalar_source("nope", std::make_shared<ScalarSource>());
    p->set_element_scalar_source("", std::make_shared<ScalarSource>());
    CHECK(p->element_scalar(5) == Catch::Approx(0.0f));   // out of range
    CHECK(p->element_scalar(-1) == Catch::Approx(0.0f));
    CHECK_FALSE(p->element_has_scalar_source(5));

    root->set_frame_clock(nullptr);
}

TEST_CASE("an element scalar binding survives a frame swap and follows its param_key",
          "[view][meter-source][design-frame]") {
    // The reason bindings are keyed by param_key, not index: set_active_frame
    // REPLACES the element list, so "element 0" is a different control per frame.
    // A binding must follow its key across the swap, not silently re-point at
    // whatever now sits at that index.
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);

    std::vector<DesignFrameElement> frame0{knob_with_key("drive", 25.0f),
                                           knob_with_key("mix", 75.0f)};
    auto panel = std::make_unique<RingPanel>(kPanelSvg, frame0);
    RingPanel* p = panel.get();
    root->add_child(std::move(panel));

    // Frame 1 declares the SAME two keys in the OPPOSITE order.
    std::vector<DesignFrameElement> frame1{knob_with_key("mix", 25.0f),
                                           knob_with_key("drive", 75.0f)};
    const int f1 = p->add_frame(kPanelSvg, frame1, -1, -1, -1, -1);

    auto drive_lfo = std::make_shared<ScalarSource>();
    p->set_element_scalar_source("drive", drive_lfo);
    drive_lfo->publish(0.75f);
    clock.tick(0.016f);

    pulp::canvas::RecordingCanvas canvas;
    p->paint(canvas);
    CHECK(p->painted[0] == Catch::Approx(0.75f));  // frame 0: drive is element 0
    CHECK(p->painted[1] == Catch::Approx(0.0f));

    p->set_active_frame(f1);
    clock.tick(0.016f);
    p->paint(canvas);
    CHECK(p->painted[0] == Catch::Approx(0.0f));   // frame 1: mix is element 0
    CHECK(p->painted[1] == Catch::Approx(0.75f));  // drive moved to element 1
    CHECK(p->element_has_scalar_source(1));
    CHECK_FALSE(p->element_has_scalar_source(0));

    root->set_frame_clock(nullptr);
}

TEST_CASE("unbinding an element scalar releases its subscription",
          "[view][meter-source][design-frame][lifecycle]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    std::vector<DesignFrameElement> els{knob_with_key("macro1", 25.0f)};
    auto panel = std::make_unique<RingPanel>(kPanelSvg, els);
    RingPanel* p = panel.get();
    root->add_child(std::move(panel));

    p->set_element_scalar_source("macro1", std::make_shared<ScalarSource>());
    REQUIRE(clock.has_active_subscribers());
    p->set_element_scalar_source("macro1", nullptr);
    CHECK_FALSE(clock.has_active_subscribers());
    CHECK_FALSE(p->element_has_scalar_source(0));
    CHECK(p->element_scalar(0) == Catch::Approx(0.0f));
}

TEST_CASE("reading an element scalar from paint() never allocates",
          "[view][meter-source][design-frame][rt-safety]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    std::vector<DesignFrameElement> els{knob_with_key("macro1", 25.0f),
                                        knob_with_key("macro2", 75.0f)};
    auto panel = std::make_unique<RingPanel>(kPanelSvg, els);
    RingPanel* p = panel.get();
    root->add_child(std::move(panel));
    p->set_element_scalar_source("macro1", std::make_shared<ScalarSource>());
    p->set_element_scalar_source("macro2", std::make_shared<ScalarSource>());

    pulp::canvas::RecordingCanvas canvas;
    clock.tick(0.016f);
    p->paint(canvas);  // warm up outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 64; ++i) p->paint(canvas);
        CHECK(probe.allocation_count() == 0);
    }
    root->set_frame_clock(nullptr);
}
