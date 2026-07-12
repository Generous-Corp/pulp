/// Host frame-timing parity tests.
///
/// These exercise the HOST SEAM — the point where a vsync source's timestamps
/// become the `dt` every animation integrates. The pre-existing FrameClock and
/// motion fixtures drive `clock.tick(1.0f/60.0f)` by hand, so they can only ever
/// prove the clock integrates whatever it is handed; they cannot catch the bug
/// where the host hands it a FAKE constant. That is what these tests cover:
/// injected 60 Hz / 120 Hz / variable-refresh / dropped / coalesced timestamps,
/// the first frame, and wake-after-idle — plus the invariant that ONE identical
/// dt reaches every consumer (FrameClock, activity probes, CSS animations,
/// widget animations).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/host_frame_pump.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <vector>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

/// A view that reports itself as always animating, so `needs_continuous_frames`
/// keeps the host pumping frames without needing a real widget animation.
class AlwaysAnimatingView : public View {
public:
    AlwaysAnimatingView() { set_continuous_repaint(true); }
};

/// Attach a CSS animation whose `elapsed_seconds` records exactly how much time
/// the CSS timeline was advanced by.
void attach_css_animation(View& v, float duration) {
    CssAnimation a;
    a.property = AnimatableProperty::opacity;
    a.spec.duration_seconds = duration;
    a.start_value = 0.0f;
    a.end_value = 1.0f;
    a.active = true;
    v.active_animations().push_back(a);
}

constexpr double k60 = 1.0 / 60.0;
constexpr double k120 = 1.0 / 120.0;

} // namespace

TEST_CASE("HostFramePump measures the real interval at 60Hz and 120Hz",
          "[view][frame-pump][timing]") {
    SECTION("60 Hz") {
        HostFramePump pump;
        double t = 1000.0;
        REQUIRE_THAT(pump.measure(t), WithinAbs(1.0f / 60.0f, 1e-6f));  // first frame: nominal
        float total = 0;
        for (int i = 0; i < 60; ++i) {
            t += k60;
            total += pump.measure(t);
        }
        // 60 frames of a 60 Hz link == 1 second of animation time.
        REQUIRE_THAT(total, WithinAbs(1.0f, 1e-3f));
    }

    SECTION("120 Hz — the ProMotion bug: 120 ticks must still be 1 second") {
        HostFramePump pump;
        double t = 1000.0;
        pump.measure(t);
        float total = 0;
        for (int i = 0; i < 120; ++i) {
            t += k120;
            total += pump.measure(t);
        }
        // With the old hardcoded 1/60 this would total 2.0 — animations ran 2x fast.
        REQUIRE_THAT(total, WithinAbs(1.0f, 1e-3f));
    }
}

TEST_CASE("HostFramePump passes variable-refresh and dropped frames through raw",
          "[view][frame-pump][timing]") {
    HostFramePump pump;
    double t = 0.0;
    pump.measure(t);

    // Variable refresh: the link slews between 120 Hz and 40 Hz.
    const double intervals[] = {k120, k60, 1.0 / 40.0, k120, 1.0 / 48.0};
    double expected_total = 0;
    float measured_total = 0;
    for (double iv : intervals) {
        t += iv;
        expected_total += iv;
        measured_total += pump.measure(t);
    }
    REQUIRE_THAT(measured_total, WithinAbs(static_cast<float>(expected_total), 1e-5f));

    // A dropped frame (two vsyncs' worth) is a SLOW FRAME, not a wake: it must
    // pass through raw so animation time keeps tracking wall time.
    t += 2 * k60;
    REQUIRE_THAT(pump.measure(t), WithinAbs(static_cast<float>(2 * k60), 1e-5f));

    // A 100 ms hitch (6 dropped vsyncs) is still under the wake threshold: raw.
    t += 0.1;
    REQUIRE_THAT(pump.measure(t), WithinAbs(0.1f, 1e-5f));
    REQUIRE(pump.resumes() == 1);  // only the first frame counted as a resume
}

TEST_CASE("HostFramePump never rewinds or invents time on a coalesced callback",
          "[view][frame-pump][timing]") {
    HostFramePump pump;
    pump.measure(100.0);
    // Two callbacks with the same presentation timestamp (coalesced): no time passed.
    REQUIRE_THAT(pump.measure(100.0 + k60), WithinAbs(static_cast<float>(k60), 1e-6f));
    REQUIRE_THAT(pump.measure(100.0 + k60), WithinAbs(0.0f, 1e-9f));
    // A backwards timestamp must clamp to zero, never negative (the shader clock
    // reads FrameClock::time() and must be monotonic).
    REQUIRE_THAT(pump.measure(100.0), WithinAbs(0.0f, 1e-9f));
}

TEST_CASE("HostFramePump treats wake-from-idle as a resume, not a 12-second frame",
          "[view][frame-pump][timing][wake]") {
    HostFramePump pump;
    double t = 5.0;
    pump.measure(t);
    t += k60;
    pump.measure(t);

    SECTION("unannounced idle — the wake threshold catches it") {
        t += 12.0;  // editor idled at 0 fps for 12 s, then a hover woke it
        const float dt = pump.measure(t);
        REQUIRE_THAT(dt, WithinAbs(1.0f / 60.0f, 1e-6f));  // one nominal frame, not 12 s
        REQUIRE(pump.resumes() == 2);
        // And the pump re-bases: the frame after the wake is a normal frame.
        t += k60;
        REQUIRE_THAT(pump.measure(t), WithinAbs(static_cast<float>(k60), 1e-6f));
    }

    SECTION("announced idle — suspend() makes the next frame a resume exactly") {
        pump.suspend();
        REQUIRE(pump.suspended());
        t += 0.001;  // even a tiny gap: the host told us it stopped pumping
        REQUIRE_THAT(pump.measure(t), WithinAbs(1.0f / 60.0f, 1e-6f));
        REQUIRE(pump.resumes() == 2);
    }

    SECTION("nominal dt follows the display's real refresh period") {
        pump.set_nominal_dt(1.0f / 120.0f);
        pump.suspend();
        REQUIRE_THAT(pump.measure(t + 3.0), WithinAbs(1.0f / 120.0f, 1e-6f));
    }
}

TEST_CASE("begin_host_frame delivers ONE dt to every consumer",
          "[view][frame-pump][timing][contract]") {
    View root;
    auto child = std::make_unique<AlwaysAnimatingView>();
    View* child_ptr = child.get();
    root.add_child(std::move(child));
    attach_css_animation(*child_ptr, 10.0f);

    FrameClock clock;
    HostFramePump pump;

    std::vector<float> subscriber_dts;
    std::vector<float> activity_dts;
    clock.subscribe([&](float dt) { subscriber_dts.push_back(dt); return true; });
    clock.subscribe_activity([&](float dt) { activity_dts.push_back(dt); });

    // Drive one second of a 120 Hz display.
    double t = 42.0;
    float returned_total = 0;
    for (int i = 0; i < 121; ++i) {
        auto tick = begin_host_frame(&root, clock, pump, t, /*needs_repaint=*/false);
        REQUIRE(tick.should_render);  // continuous-repaint child keeps the loop alive
        advance_host_frame(&root, clock, tick.dt);
        returned_total += tick.dt;
        t += k120;
    }

    // Every consumer saw the same series: the FrameClock's integrated time, the
    // render subscribers, the activity probes, and the CSS timeline.
    REQUIRE(subscriber_dts.size() == 121);
    REQUIRE(activity_dts.size() == 121);
    REQUIRE(subscriber_dts == activity_dts);
    REQUIRE_THAT(clock.time(), WithinAbs(returned_total, 1e-4f));
    REQUIRE_THAT(child_ptr->active_animations()[0].elapsed_seconds,
                 WithinAbs(returned_total, 1e-4f));

    // …and that series is 1 real second (first frame nominal + 120 × 1/120),
    // NOT the ~2 s a hardcoded 1/60 would have produced on this display.
    REQUIRE_THAT(clock.time(), WithinAbs(1.0f / 60.0f + 1.0f, 2e-3f));
    REQUIRE(clock.frame() == 121);
}

TEST_CASE("begin_host_frame pumps activity probes on frames it does not render",
          "[view][frame-pump][timing][contract]") {
    View root;  // static tree: nothing animating
    FrameClock clock;
    HostFramePump pump;

    std::vector<float> activity_dts;
    clock.subscribe_activity([&](float dt) { activity_dts.push_back(dt); });

    double t = 7.0;
    auto tick = begin_host_frame(&root, clock, pump, t, /*needs_repaint=*/false);
    REQUIRE_FALSE(tick.should_render);   // idle at 0 fps
    REQUIRE(activity_dts.size() == 1);   // probe still ran, with the measured dt
    REQUIRE_THAT(activity_dts[0], WithinAbs(tick.dt, 1e-9f));
    REQUIRE(clock.frame() == 0);         // the render clock did NOT advance

    // The probe flips the tree live (a meter starts moving) — because probes run
    // BEFORE the decision, this very tick renders.
    root.set_continuous_repaint(true);
    t += k60;
    tick = begin_host_frame(&root, clock, pump, t, false);
    REQUIRE(tick.should_render);
    REQUIRE_THAT(tick.dt, WithinAbs(static_cast<float>(k60), 1e-6f));
}

TEST_CASE("wake-from-idle does not teleport an animation that starts on wake",
          "[view][frame-pump][timing][wake]") {
    View root;
    root.set_continuous_repaint(true);
    FrameClock clock;
    HostFramePump pump;

    // Frame 1, then the editor sits idle for 30 s (host stops pumping entirely).
    double t = 0.0;
    auto tick = begin_host_frame(&root, clock, pump, t, false);
    advance_host_frame(&root, clock, tick.dt);

    // A hover starts a 300 ms fade, and the host wakes up 30 s later.
    attach_css_animation(root, 0.3f);
    t += 30.0;
    tick = begin_host_frame(&root, clock, pump, t, true);
    advance_host_frame(&root, clock, tick.dt);

    // The fade must be one frame in — NOT finished. A raw 30 s dt (or any clamp
    // bigger than the animation) would have snapped it straight to its end value.
    REQUIRE(root.active_animations()[0].elapsed_seconds < 0.3f);
    REQUIRE(root.active_animations()[0].active);
    REQUIRE_THAT(root.active_animations()[0].elapsed_seconds,
                 WithinAbs(1.0f / 60.0f, 1e-5f));
    REQUIRE(tick.resumed);
}

TEST_CASE("advance_widget_animations drives widgets and CSS with the host dt",
          "[view][frame-pump][timing]") {
    View root;
    auto knob = std::make_unique<Knob>();
    Knob* knob_ptr = knob.get();
    root.add_child(std::move(knob));
    attach_css_animation(*knob_ptr, 1.0f);

    knob_ptr->on_mouse_enter();  // starts the hover-glow animation
    advance_widget_animations(&root, 0.5f);
    REQUIRE(knob_ptr->hover_glow() > 0.0f);
    REQUIRE_THAT(knob_ptr->active_animations()[0].elapsed_seconds, WithinAbs(0.5f, 1e-5f));
}

// ── The gated host: the vsyncs a real macOS host does NOT dispatch ───────────
//
// Every test above drives the pump on EVERY simulated vsync. The two macOS GPU
// hosts do not: MacGpuWindowHost::display_link_callback and
// MacGpuPluginViewHost::display_link_callback answer "is this vsync worth a hop
// to the main thread?" ON THE LINK THREAD, from needs_repaint / continuous /
// has-idle-callback, and return early when all three are false — so on an idle
// vsync `begin_host_frame` is never called and the pump stops measuring. A test
// that pumps every vsync can therefore never see the bug this harness exists to
// catch: the next real frame measures the WHOLE idle gap and hands it to
// whatever animation just started.
//
// GatedHost mirrors those two callbacks exactly, including render_frame()'s
// "needs_repaint_ = continuous_frames_ = (tree still animating?)" post-paint
// step (window_host_mac.mm), so the gate closes on a static tree the same way.
namespace {

struct GatedHost {
    explicit GatedHost(View* r) : root(r) {}

    View* root;
    FrameClock clock;
    HostFramePump pump;
    bool needs_repaint = true;   // hosts start dirty (first paint)
    bool continuous = false;
    bool has_idle = false;       // true only for scripted/JS editors
    int vsyncs = 0;
    int skipped = 0;
    int rendered = 0;
    float last_dt = -1.0f;

    /// One vsync of the display link.
    void vsync(double t) {
        ++vsyncs;
        // Link thread.
        if (!should_dispatch_host_frame(pump, needs_repaint, continuous, has_idle)) {
            ++skipped;
            return;
        }
        // Main thread.
        const auto tick = begin_host_frame(root, clock, pump, t, needs_repaint);
        if (!tick.should_render) {
            continuous = false;
            return;
        }
        advance_host_frame(root, clock, tick.dt);
        last_dt = tick.dt;
        ++rendered;
        // render_frame(): repaint, then re-arm from the tree's own liveness.
        continuous = needs_continuous_frames(root) || clock.has_active_subscribers();
        needs_repaint = continuous;
    }
};

} // namespace

TEST_CASE("a short idle gap is not fed to an animation that starts on wake",
          "[view][frame-pump][timing][wake][gated-host]") {
    // The exact user-visible regression: a native C++ editor (no scripted idle
    // callback, i.e. every example plugin), static tree, mouse away for 100 ms,
    // then the pointer enters a Fader. Fader::on_mouse_enter starts an 80 ms
    // hover fade (motion.duration.fast). If the host feeds it the 100 ms idle
    // gap, the fade COMPLETES on its first frame and the hover never plays.
    View root;
    auto fader = std::make_unique<Fader>();
    Fader* f = fader.get();
    root.add_child(std::move(fader));

    GatedHost host(&root);
    double t = 1000.0;

    // Frame 1: the initial paint. Tree is static afterwards, so the gate closes.
    host.vsync(t);
    REQUIRE(host.rendered == 1);
    REQUIRE_FALSE(host.needs_repaint);

    // 100 ms of idle vsyncs (6 at 60 Hz). The real host returns from the link
    // callback without touching the pump — that is the whole point.
    for (int i = 0; i < 6; ++i) {
        t += k60;
        host.vsync(t);
    }
    REQUIRE(host.skipped == 6);
    REQUIRE(host.rendered == 1);  // nothing was dispatched: the pump went stale

    // Mouse enters the Fader: an 80 ms hover fade starts and the host marks
    // itself dirty (View::request_repaint → needs_repaint_).
    f->on_mouse_enter();
    host.needs_repaint = true;
    REQUIRE_THAT(f->hover_scale(), WithinAbs(1.0f, 1e-6f));

    t += k60;
    host.vsync(t);
    REQUIRE(host.rendered == 2);

    // The frame after a skipped-vsync idle is a RESUME: one nominal frame, not
    // the 100 ms the UI spent doing nothing. (CHECK, not REQUIRE, so a regression
    // also reports what that dt did to the animation below.)
    CHECK_THAT(host.last_dt, WithinAbs(1.0f / 60.0f, 1e-5f));
    // …so the fade is one frame in, NOT finished. With the raw 100 ms gap it
    // lands exactly on its 1.3 end value and the hover animation is never seen.
    CHECK(f->hover_scale() > 1.0f);
    CHECK(f->hover_scale() < 1.29f);
}

TEST_CASE("a gated host resumes even when the idle gap is under one frame",
          "[view][frame-pump][timing][wake][gated-host]") {
    // Any skipped vsync invalidates the pump's last timestamp — there is no gap
    // small enough to be safely treated as elapsed animation time, because the
    // tree was, by construction, not animating during it.
    View root;
    GatedHost host(&root);
    double t = 0.0;
    host.vsync(t);              // first paint
    t += k60; host.vsync(t);    // idle vsync: skipped
    REQUIRE(host.skipped == 1);

    host.needs_repaint = true;
    t += k60;
    host.vsync(t);
    // Raw would be 2/60 here (two vsyncs since the last measured frame). Only
    // one of them was a real frame; the other was idle.
    REQUIRE_THAT(host.last_dt, WithinAbs(1.0f / 60.0f, 1e-5f));
    REQUIRE(host.pump.resumes() == 2);  // the first frame, and this wake
}

TEST_CASE("a gated host that keeps rendering still measures raw dt",
          "[view][frame-pump][timing][gated-host]") {
    // The skip bookkeeping must not turn a continuously-animating host into a
    // stream of nominal frames: while the gate stays OPEN, every vsync is
    // measured and a slow/dropped frame still passes through raw.
    View root;
    root.set_continuous_repaint(true);  // something is animating: gate stays open
    GatedHost host(&root);
    double t = 0.0;
    host.vsync(t);
    for (int i = 0; i < 10; ++i) {
        t += k120;
        host.vsync(t);
        REQUIRE_THAT(host.last_dt, WithinAbs(static_cast<float>(k120), 1e-5f));
    }
    REQUIRE(host.skipped == 0);
    REQUIRE(host.rendered == 11);

    // A 100 ms hitch WHILE animating is a slow frame, not a wake: raw.
    t += 0.1;
    host.vsync(t);
    REQUIRE_THAT(host.last_dt, WithinAbs(0.1f, 1e-4f));
    REQUIRE(host.pump.resumes() == 1);  // only the first frame
}

TEST_CASE("a scripted host is pumped every vsync and never skips",
          "[view][frame-pump][timing][gated-host]") {
    // A JS editor installs an idle callback (rAF / timers / async results), so
    // the gate is always open even on a static tree. Those vsyncs ARE measured,
    // so no skip is recorded and a wake needs no resume.
    View root;
    GatedHost host(&root);
    host.has_idle = true;
    double t = 0.0;
    host.vsync(t);
    for (int i = 0; i < 6; ++i) {
        t += k60;
        host.vsync(t);
    }
    REQUIRE(host.skipped == 0);
    REQUIRE(host.rendered == 1);  // dispatched every vsync, painted only once
    REQUIRE_FALSE(host.pump.suspended());
    REQUIRE(host.pump.resumes() == 1);
}

// ── FrameClock subscription lifetime across host teardown ────────────────────
//
// Both plugin-view hosts OWN their FrameClock (it is a member), and the view tree
// does NOT — it belongs to the Processor and routinely outlives the editor. So on
// editor close the clock is destroyed first. Any widget that cached a
// `FrameClock*` when it subscribed is then holding a dangling pointer, and its
// destructor unsubscribes through it. The hosts hand the tree its last safe
// moment: `root->set_frame_clock(nullptr)` runs from the host destructor, while
// the clock is still alive. A caching widget MUST use it.
//
// This fired for real: giving the macOS CPU plugin-view host a FrameClock (so a
// native editor animates at all) made a focused TextEditor subscribe for the
// first time in that host, and the CPU plugin-view host tests started taking a
// SIGBUS in ~TextEditor → FrameClock::unsubscribe about a third of the time.

TEST_CASE("a focused TextEditor drops its caret subscription when the host clock goes away",
          "[view][frame-pump][frame-clock][lifetime]") {
    View root;
    auto editor = std::make_unique<TextEditor>();
    TextEditor* te = editor.get();
    root.add_child(std::move(editor));

    auto clock = std::make_unique<FrameClock>();
    root.set_frame_clock(clock.get());

    te->on_focus_changed(true);  // focus starts the caret blink subscription
    REQUIRE(clock->has_active_subscribers());

    // Host teardown, step 1: the host clears the tree's clock while it is alive.
    root.set_frame_clock(nullptr);
    REQUIRE_FALSE(clock->has_active_subscribers());  // the subscription is GONE…
    REQUIRE(root.frame_clock() == nullptr);

    // Host teardown, step 2: the clock is freed. The tree survives it.
    clock.reset();
    // …so ~TextEditor here has nothing to unsubscribe from, and cannot dereference
    // the freed clock. (Without the fix this is the use-after-free; the assertion
    // above is the deterministic proof, the crash is the flaky one.)
}

TEST_CASE("an editing InlineValueEditor drops its caret subscription when the host clock goes away",
          "[view][frame-pump][frame-clock][lifetime]") {
    View root;
    auto ive = std::make_unique<InlineValueEditor>();
    InlineValueEditor* ed = ive.get();
    root.add_child(std::move(ive));

    auto clock = std::make_unique<FrameClock>();
    root.set_frame_clock(clock.get());

    ed->begin_edit();
    REQUIRE(ed->editing());
    REQUIRE(clock->has_active_subscribers());

    root.set_frame_clock(nullptr);
    REQUIRE_FALSE(clock->has_active_subscribers());

    clock.reset();
}

TEST_CASE("a caret subscription is re-homed onto a replacement clock",
          "[view][frame-pump][frame-clock][lifetime]") {
    // Dropping the old subscription must not leave a focused editor with a dead
    // caret: the notification also re-subscribes on whatever clock is now reachable
    // (an editor moved between hosts, or a host that swaps its clock).
    View root;
    auto editor = std::make_unique<TextEditor>();
    TextEditor* te = editor.get();
    root.add_child(std::move(editor));

    FrameClock first;
    FrameClock second;
    root.set_frame_clock(&first);
    te->on_focus_changed(true);
    REQUIRE(first.has_active_subscribers());

    root.set_frame_clock(&second);
    REQUIRE_FALSE(first.has_active_subscribers());
    REQUIRE(second.has_active_subscribers());
}
