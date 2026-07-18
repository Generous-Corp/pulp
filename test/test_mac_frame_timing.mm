/// macOS frame-timing seam — the answers that come from CoreVideo / AppKit state
/// and that no portable C++ test can therefore reach:
///   - the pump's nominal (first-frame / wake) interval seed,
///   - whether the CPU plugin-view host drives a render link at all,
///   - what that link COSTS while the editor is static (it runs inside a DAW),
///   - and that -[PulpView prepareForTeardown] leaves no pointer to the freed
///     host behind for the animation timer to find.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/host_frame_pump.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include "../core/view/platform/mac/window_host_mac_internal.hpp"
#include "../core/view/platform/mac/window_host_mac_view.h"

#include <memory>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::view::mac_frame_timing::display_link_nominal_dt;
using pulp::view::mac_frame_timing::MacDisplayLinkDriver;
using pulp::view::mac_frame_timing::plugin_view_wants_render_link;

namespace {

/// Output callback for driver-lifecycle tests: a link opened by these tests is
/// never resumed, so this only has to be a valid target for the driver to route.
CVReturn counting_link_callback(CVDisplayLinkRef, const CVTimeStamp*,
                                const CVTimeStamp*, CVOptionFlags, CVOptionFlags*,
                                void*) {
    return kCVReturnSuccess;
}

/// Does the main display report a definite nominal refresh period? Only there is
/// a never-started link's nominal_dt() meaningful: display_link_nominal_dt falls
/// back to the MEASURED period when nominal is indefinite, and that stays 0 until
/// the link has run. Probes through its own link so it reads the same CoreVideo
/// answer the driver would, without needing the driver's private handle.
bool main_display_has_definite_nominal_period() {
    CVDisplayLinkRef probe = nullptr;
    if (CVDisplayLinkCreateWithCGDisplay(CGMainDisplayID(), &probe) != kCVReturnSuccess ||
        probe == nullptr) {
        return false;
    }
    const CVTime nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(probe);
    const bool definite = (nominal.flags & kCVTimeIsIndefinite) == 0 && nominal.timeScale != 0;
    CVDisplayLinkRelease(probe);
    return definite;
}


/// Run the main run loop for `seconds`, which is what drains the display link's
/// dispatch_async(dispatch_get_main_queue()) blocks.
void pump_main_runloop(double seconds) {
    [[NSRunLoop currentRunLoop]
        runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

/// Counts every frame the host DISPATCHES to the main thread, without making the
/// tree look alive. An activity probe is fired by `begin_host_frame` on every
/// dispatched tick (`FrameClock::pump_activity`) and is deliberately NOT render
/// liveness (`has_active_subscribers()` stays false), so subscribing one cannot
/// itself hold the gate open — it is the one honest counter for "did this vsync
/// reach the main thread?".
struct FrameCounter {
    int frames = 0;
    std::vector<float> dts;  ///< the dt of every dispatched frame, in order

    int subscribe(pulp::view::FrameClock& clock) {
        return clock.subscribe_activity([this](float dt) {
            ++frames;
            dts.push_back(dt);
        });
    }
};

}  // namespace

TEST_CASE("the pump's nominal dt is seeded from a link that has never run",
          "[view][mac][frame-pump][timing]") {
    // Every host seeds HostFramePump::set_nominal_dt right after creating (and
    // starting) its display link. The seed must therefore be readable from a
    // link that has not produced a single frame yet — which is exactly what
    // CVDisplayLinkGetActualOutputVideoRefreshPeriod cannot do: it reports a
    // MEASURED period and returns 0 until the link has computed one. Seeding from
    // it left the pump on its 1/60 default forever, so the "first/wake frame is
    // one real frame of THIS display" contract (1/120 on ProMotion) never held.
    CVDisplayLinkRef link = nullptr;
    // NOT ...CreateWithActiveCGDisplays: that one fails (-6661) in a process with
    // no window-server display list, which is what a headless test runner is.
    if (CVDisplayLinkCreateWithCGDisplay(CGMainDisplayID(), &link) != kCVReturnSuccess ||
        !link) {
        SUCCEED("no display attached to this process — nothing to seed from");
        return;
    }

    const double measured = CVDisplayLinkGetActualOutputVideoRefreshPeriod(link);
    INFO("CVDisplayLinkGetActualOutputVideoRefreshPeriod (never started) = " << measured);
    if (measured > 0.0)
        WARN("the measured period was valid before the link ran — unusual; the "
             "nominal period is still the correct seed");

    const float dt = display_link_nominal_dt(link);
    INFO("seeded nominal dt = " << dt);
    // A real refresh interval: between 240 Hz and 24 Hz. Zero (the old,
    // measured-period answer) means the host silently keeps a hardcoded 1/60.
    REQUIRE(dt > 0.0f);
    REQUIRE(dt >= 1.0f / 240.0f);
    REQUIRE(dt <= 1.0f / 24.0f);

    CVDisplayLinkRelease(link);
}

TEST_CASE("a null link leaves the pump's default in place",
          "[view][mac][frame-pump][timing]") {
    REQUIRE(display_link_nominal_dt(nullptr) == 0.0f);
}

TEST_CASE("a closed display-link driver answers every verb without a handle",
          "[view][mac][frame-pump][timing]") {
    // The hosts call these verbs on a driver that may never have opened — a
    // headless process has no display list, so open() fails and every later
    // verb has to be a no-op rather than a null-handle CoreVideo call. This is
    // the contract that lets a host write its start/stop sequence straight
    // through without re-checking the handle at each step.
    MacDisplayLinkDriver driver;
    REQUIRE_FALSE(driver.is_open());
    REQUIRE(driver.nominal_dt() == 0.0f);
    driver.bind_to_display(CGMainDisplayID());
    driver.resume();
    driver.stop();
    driver.stop();  // idempotent: every host stops explicitly, then again on destruction
    REQUIRE_FALSE(driver.is_open());
}

TEST_CASE("an open display-link driver refuses to replace its own handle",
          "[view][mac][frame-pump][timing]") {
    // A second open() must leave the first link alone and report false. The
    // hosts rely on it: the standalone GPU window host starts its link from
    // run_event_loop() AND from show(), and a plugin editor re-attached to a
    // window starts one per attach. Replacing the handle would leak the running
    // link and leave its callback firing at a host that thinks it owns one link.
    MacDisplayLinkDriver driver;
    if (!driver.open(&counting_link_callback, nullptr)) {
        SUCCEED("no display list in this process — no link to open");
        return;
    }
    REQUIRE(driver.is_open());
    REQUIRE_FALSE(driver.open(&counting_link_callback, nullptr));
    REQUIRE(driver.is_open());

    // Bound to a display, the link can seed a pump before it has ever ticked --
    // but only where the display reports a definite nominal period. A
    // variable-refresh-rate panel, or the virtual display a Mac attaches when
    // driven headless over screen sharing, legitimately reports
    // kCVTimeIsIndefinite; there the measured period is the only answer and it
    // stays 0 until the link has actually run. Asserting unconditionally would
    // fail on those hosts for a property this case is not about.
    driver.bind_to_display(CGMainDisplayID());
    if (main_display_has_definite_nominal_period()) {
        REQUIRE(driver.nominal_dt() > 0.0f);
    }

    driver.stop();
    REQUIRE_FALSE(driver.is_open());
    REQUIRE(driver.nominal_dt() == 0.0f);
    // Closing is not retiring: a re-attached editor opens the same driver again.
    REQUIRE(driver.open(&counting_link_callback, nullptr));
    REQUIRE(driver.is_open());
}

TEST_CASE("the CPU plugin-view host drives frames for a NATIVE editor too",
          "[view][mac][frame-pump][plugin-view]") {
    // MacPluginViewHost::update_render_link() used to start its CVDisplayLink
    // only when a scripted idle callback was installed. The only callers of
    // PluginViewHost::set_idle_callback are make_scripted_idle_pump (clap_entry,
    // vst3_plug_view, au_v2_cocoa_view, au_view_controller_mac) — all guarded on
    // a JS bridge. So a native C++ editor in a CPU host got NO frame source: no
    // FrameClock tick, no widget or CSS animation, no caret blink. Being in a
    // window is the whole condition; an idle callback is one more thing to pump,
    // not the reason to pump.
    REQUIRE(plugin_view_wants_render_link(/*has_view=*/true, /*in_window=*/true));
    // Not attached to a window (or no view at all): no vsync source to run.
    REQUIRE_FALSE(plugin_view_wants_render_link(/*has_view=*/true, /*in_window=*/false));
    REQUIRE_FALSE(plugin_view_wants_render_link(/*has_view=*/false, /*in_window=*/true));
}

TEST_CASE("a STATIC CPU plugin editor dispatches no frames to the main thread",
          "[view][mac][frame-pump][plugin-view][gate]") {
    // The CPU plugin-view host now runs a CVDisplayLink for EVERY editor in a
    // window, not just a scripted one — that is what makes a native C++ editor
    // animate at all. But the link fires at the display's refresh rate forever,
    // and this code runs inside somebody's DAW. So the callback must answer
    // "is this vsync worth a frame?" ON THE LINK THREAD (should_dispatch_host_frame),
    // exactly like the two GPU hosts. Without that gate a static native editor
    // hops to the main thread and walks the entire view tree 60-120 times a
    // second, forever, to conclude that nothing changed.
    @autoreleasepool {
        [NSApplication sharedApplication];

        pulp::view::View root;
        auto fader = std::make_unique<pulp::view::Fader>();
        pulp::view::Fader* f = fader.get();
        root.add_child(std::move(fader));

        auto host = pulp::view::PluginViewHost::create(
            root, pulp::view::PluginViewHost::Size{400, 300});
        REQUIRE(host != nullptr);
        pulp::view::FrameClock* clock = root.frame_clock();
        REQUIRE(clock != nullptr);

        FrameCounter counter;
        counter.subscribe(*clock);

        // Put the editor in a window — the whole condition for the link to run
        // (plugin_view_wants_render_link). Never ordered front: a DAW-embedded
        // editor is a subview, and we want no window-server side effects.
        NSWindow* window = [[[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 400, 300)
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO] autorelease];
        [window setContentView:(NSView*)host->native_handle()];

        // Let the mount frames (the host starts dirty) drain.
        pump_main_runloop(0.20);
        const int mounted = counter.frames;
        if (mounted == 0) {
            SUCCEED("no CVDisplayLink in this process (headless) — nothing to gate");
            return;
        }

        // ── The finding: 200 ms of vsyncs over a completely static tree. ──
        // Gated: not one of them reaches the main thread. Ungated: ~12 at 60 Hz,
        // ~24 on ProMotion, each carrying a full needs_continuous_frames() walk.
        // (200 ms is deliberately UNDER the pump's 250 ms wake threshold, so the
        // resume below has to come from note_frame_skipped, not from the gap.)
        pump_main_runloop(0.20);
        INFO("frames dispatched while idle: " << (counter.frames - mounted));
        REQUIRE(counter.frames == mounted);

        // ── …and it still wakes. ──
        // A real hover arrives as an NSView event, which marks the view dirty
        // (-setNeedsDisplay:) exactly as View::request_repaint does; that is the
        // signal the link thread reads to re-open the gate.
        f->on_mouse_enter();
        host->repaint();
        pump_main_runloop(0.10);
        REQUIRE(counter.frames > mounted);

        // The first frame after the idle is a RESUME — one nominal frame, not the
        // 200 ms the UI spent doing nothing. Feeding the gap through would finish
        // the Fader's 80 ms hover fade before it was ever drawn.
        const float wake_dt = counter.dts.at(static_cast<size_t>(mounted));
        INFO("dt of the first frame after the idle = " << wake_dt);
        REQUIRE(wake_dt > 0.0f);
        REQUIRE(wake_dt < 0.05f);
        // The fade is running, not finished (hover_scale ends at 1.3).
        CHECK(f->hover_scale() > 1.0f);

        [window setContentView:nil];
    }
}

// Name deliberately avoids a leading '-' and square brackets: Catch2 reads a
// leading dash as a CLI flag and brackets as tags, so ctest cannot invoke a
// case named after an ObjC selector ("Unrecognised token: -[PulpView").
TEST_CASE("PulpView prepareForTeardown drops every pointer into the freed host",
          "[view][mac][frame-pump][teardown]") {
    // The animation timer block dereferences BOTH the clock and the pump
    // (`*self.frameClock, *self.framePump`). prepareForTeardown cleared the clock
    // and the root but left framePump pointing into the host that is about to be
    // freed — safe only for as long as nobody reorders the `self.frameClock &&
    // self.framePump` short-circuit that happens to test the cleared one first.
    // The teardown contract is "no pointer into the host survives", not "one
    // surviving pointer is currently unreachable".
    @autoreleasepool {
        [NSApplication sharedApplication];
        PulpView* view = [[[PulpView alloc]
            initWithFrame:NSMakeRect(0, 0, 100, 100)] autorelease];

        pulp::view::FrameClock clock;
        pulp::view::View root;
        pulp::view::HostFramePump pump;
        view.rootView = &root;
        view.frameClock = &clock;
        view.framePump = &pump;

        [view prepareForTeardown];

        REQUIRE(view.rootView == nullptr);
        REQUIRE(view.frameClock == nullptr);
        REQUIRE(view.framePump == nullptr);
    }
}
