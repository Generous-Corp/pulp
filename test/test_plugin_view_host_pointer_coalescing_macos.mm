// Per-frame pointer coalescing through the macOS PLUGIN view host.
//
// The coalescer itself has its own unit suite (test_pointer_coalescer.cpp).
// This one exists because that suite passed the whole time the plug-in path
// was uncoalesced: the component worked and nothing drove it. Every assertion
// here therefore goes through the host's REAL channels — a real
// PluginViewHost, its real NSView, real NSEvents delivered to -mouseDown: /
// -mouseDragged: / -mouseUp:, and the same per-frame flush the host's display
// link calls — so a regression that unwires the host from the component fails
// here even though the component is still perfect.
//
// The one thing simulated is the frame boundary. Timing ownership sits with
// the caller by design (see pointer_coalescer.hpp), so "present a frame" is
// PluginViewHost::flush_pointer_input_for_frame() rather than a real vsync;
// waiting on a live CVDisplayLink would make these tests time-dependent
// without testing anything extra.

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/host_pointer_input.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace pulp::view;

namespace {

/// A View that records every pointer callback it receives, in order, so a test
/// can assert on the SHAPE of the stream (how many, which phase, where) rather
/// than on a single final value — the merged position surviving is not by
/// itself proof that the intermediate ones were merged rather than delivered.
///
/// These are the legacy virtuals every widget overrides, which is exactly the
/// channel `deliver_mouse_drag` drives, so this records what a real knob or
/// band-paint surface would see.
class Recorder : public View {
public:
    struct Event {
        enum class Kind { down, drag, up } kind;
        pulp::view::Point position;
    };
    std::vector<Event> events;

    std::size_t count(Event::Kind kind) const {
        std::size_t n = 0;
        for (const auto& e : events)
            if (e.kind == kind) ++n;
        return n;
    }

    void on_mouse_down(pulp::view::Point p) override {
        events.push_back({Event::Kind::down, p});
    }
    void on_mouse_drag(pulp::view::Point p) override {
        events.push_back({Event::Kind::drag, p});
    }
    void on_mouse_up(pulp::view::Point p) override {
        events.push_back({Event::Kind::up, p});
    }
};

/// A host NSView attached into a real window, which is what makes the host
/// start its frame driver (-viewDidMoveToWindow → start link). Kept as a
/// fixture because "in a window" is a precondition of the behavior under test,
/// not an incidental setup step.
struct HostedEditor {
    Recorder root;
    Recorder& rec = root;
    std::unique_ptr<PluginViewHost> host;
    NSWindow* window = nil;
    NSView* view = nil;

    /// `use_gpu` picks WHICH of the two macOS plug-in views is under test.
    /// They are separate Objective-C classes with separate ivars and separate
    /// start/stop paths, so wiring one and not the other is a live possibility
    /// — and the GPU view is the one a scripted editor actually runs on, i.e.
    /// the path the reported problem is on.
    explicit HostedEditor(bool use_gpu = false) {
        root.set_bounds({0, 0, 320, 200});
        PluginViewHost::Options opts;
        opts.size = {320u, 200u};
        opts.use_gpu = use_gpu;
        host = PluginViewHost::create(root, opts);
        if (!host) return;
        if (use_gpu && !host->is_gpu_backed()) { host.reset(); return; }
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 320, 200)
                                             styleMask:NSWindowStyleMaskBorderless
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        host->attach_to_parent((__bridge void*)window.contentView);
        view = (__bridge NSView*)host->native_handle();
    }

    bool usable() const { return host != nullptr && view != nil; }

    NSEvent* event(NSEventType type, pulp::view::Point p, int click_count) const {
        // The view is not flipped, and -localPoint: does the flip; feed window
        // coordinates the way AppKit would.
        return [NSEvent mouseEventWithType:type
                                  location:NSMakePoint(p.x, window.contentView.bounds.size.height - p.y)
                             modifierFlags:0
                                 timestamp:0
                              windowNumber:window.windowNumber
                                   context:nil
                               eventNumber:0
                                clickCount:click_count
                                  pressure:1.0f];
    }

    void down(pulp::view::Point p) { [view mouseDown:event(NSEventTypeLeftMouseDown, p, 1)]; }
    void drag(pulp::view::Point p) { [view mouseDragged:event(NSEventTypeLeftMouseDragged, p, 1)]; }
    void up(pulp::view::Point p) { [view mouseUp:event(NSEventTypeLeftMouseUp, p, 1)]; }

    /// Present one frame — the display link's per-tick flush.
    void present_frame() { host->flush_pointer_input_for_frame(); }
};

}  // namespace

TEST_CASE("hosted plugin editor declares a pointer flush driver once in a window",
          "[view][pointer][coalescing][macos]") {
    @autoreleasepool {
        HostedEditor editor;
        if (!editor.usable()) {
            WARN("no PluginViewHost on this platform build");
            return;
        }

        // THE regression this file exists for. The opt-in and the frame driver
        // are two different lines of code, and the failure when they disagree
        // is silent: the flush runs, finds nothing held because nothing was
        // ever held, and every sample took the immediate path. Nothing logs,
        // nothing throws, drags simply cost what they cost before. If this
        // assertion is the only one that fails, the host is driving frames
        // without telling its view — coalescing is off.
        REQUIRE(editor.host->pointer_coalescing_active());
    }
}

TEST_CASE("a burst of drag motion inside one frame becomes one dispatch",
          "[view][pointer][coalescing][macos]") {
    @autoreleasepool {
        HostedEditor editor;
        if (!editor.usable()) {
            WARN("no PluginViewHost on this platform build");
            return;
        }
        REQUIRE(editor.host->pointer_coalescing_active());

        editor.down({10, 10});
        REQUIRE(editor.rec.count(Recorder::Event::Kind::down) == 1);

        // Eight OS motion events without an intervening presented frame — an
        // ordinary hand-drag rate against an editor painting slower than the
        // mouse moves.
        for (int i = 1; i <= 8; ++i)
            editor.drag({10.0f + static_cast<float>(i) * 5.0f, 10.0f});

        // Held, not dropped: nothing has been delivered yet...
        CHECK(editor.rec.count(Recorder::Event::Kind::drag) == 0);

        // ...and the frame releases exactly one, carrying the NEWEST position.
        editor.present_frame();
        REQUIRE(editor.rec.count(Recorder::Event::Kind::drag) == 1);
        CHECK(editor.rec.events.back().position.x == 50.0f);

        // A frame with nothing held delivers nothing (no phantom repeat).
        editor.present_frame();
        CHECK(editor.rec.count(Recorder::Event::Kind::drag) == 1);

        // One dispatch PER FRAME, not one per gesture: a second burst releases
        // a second sample.
        editor.drag({80, 40});
        editor.drag({90, 45});
        editor.present_frame();
        REQUIRE(editor.rec.count(Recorder::Event::Kind::drag) == 2);
        CHECK(editor.rec.events.back().position.x == 90.0f);
    }
}

TEST_CASE("a discrete tap is never deferred to a frame boundary",
          "[view][pointer][coalescing][macos]") {
    // The regression this guards against is NOT the one coalescing was built to
    // fix, which is why it needs its own case. Coalescing trades latency for
    // volume on MOTION; if a press or release ever entered the same queue, every
    // tap would wait for the next presented frame — 60-80ms at the 12-17 fps a
    // loaded editor actually runs at, felt as a sluggish dropdown or a
    // late-registering click. The drag measurements are blind to it.
    //
    // So: press and release, with NO motion and NO frame in between, must both
    // have been delivered before this test ever presents a frame.
    @autoreleasepool {
        HostedEditor editor;
        if (!editor.usable()) {
            WARN("no PluginViewHost on this platform build");
            return;
        }
        REQUIRE(editor.host->pointer_coalescing_active());  // worst case for deferral

        editor.down({40, 40});
        REQUIRE(editor.rec.count(Recorder::Event::Kind::down) == 1);  // immediate

        editor.up({40, 40});
        REQUIRE(editor.rec.count(Recorder::Event::Kind::up) == 1);    // immediate

        // Both landed with zero frames presented, and in order.
        REQUIRE(editor.rec.events.size() == 2);
        CHECK(editor.rec.events[0].kind == Recorder::Event::Kind::down);
        CHECK(editor.rec.events[1].kind == Recorder::Event::Kind::up);

        // And the coalescer was never handed anything: a tap carries no motion,
        // so it cannot even have taken the merge path.
        editor.present_frame();
        CHECK(editor.rec.events.size() == 2);  // no phantom late delivery
    }
}

TEST_CASE("the host's own frame driver releases held motion with no manual flush",
          "[view][pointer][coalescing][macos]") {
    // The tests around this one call flush_pointer_input_for_frame() themselves,
    // which proves the coalescing contract but NOT that anything in the shipped
    // host ever calls it. That gap is precisely the shape of the bug being
    // fixed — a component that works, wired to a driver that never drives it —
    // so this case touches no flush at all: it posts motion, runs the run loop,
    // and requires the host's real CVDisplayLink to have delivered it.
    @autoreleasepool {
        HostedEditor editor;
        if (!editor.usable()) {
            WARN("no PluginViewHost on this platform build");
            return;
        }
        REQUIRE(editor.host->pointer_coalescing_active());

        editor.down({10, 10});
        for (int i = 1; i <= 5; ++i)
            editor.drag({10.0f + static_cast<float>(i) * 4.0f, 10.0f});
        REQUIRE(editor.rec.count(Recorder::Event::Kind::drag) == 0);  // held

        // Bounded wait rather than a fixed sleep: pass as soon as a frame has
        // been presented, so this costs one vsync in the normal case and only
        // spends the budget when something is actually wrong.
        const NSTimeInterval deadline = [NSDate timeIntervalSinceReferenceDate] + 2.0;
        while (editor.rec.count(Recorder::Event::Kind::drag) == 0 &&
               [NSDate timeIntervalSinceReferenceDate] < deadline) {
            [[NSRunLoop currentRunLoop]
                runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }

        // Delivered by the display link, and merged: five motion events, one
        // dispatch, newest position.
        REQUIRE(editor.rec.count(Recorder::Event::Kind::drag) == 1);
        CHECK(editor.rec.events.back().position.x == 30.0f);
    }
}

TEST_CASE("a release flushes held motion and never lands ahead of it",
          "[view][pointer][coalescing][macos]") {
    @autoreleasepool {
        HostedEditor editor;
        if (!editor.usable()) {
            WARN("no PluginViewHost on this platform build");
            return;
        }
        REQUIRE(editor.host->pointer_coalescing_active());

        editor.down({10, 10});
        editor.drag({20, 10});
        editor.drag({30, 10});
        // No frame in between: the release arrives while motion is still held.
        editor.up({30, 10});

        // The terminal must not be deferred to the next frame (a click delayed
        // by a whole frame trades a drag problem for a click problem), must not
        // swallow the motion, and must not overtake it. A consumer that brackets
        // a gesture — Spectr's editor authority rejects a paint whose
        // paint_start never arrived — fails loudly on any of those.
        const auto& ev = editor.rec.events;
        REQUIRE(ev.size() == 3);
        CHECK(ev[0].kind == Recorder::Event::Kind::down);
        CHECK(ev[1].kind == Recorder::Event::Kind::drag);
        CHECK(ev[1].position.x == 30.0f);  // merged, newest position survives
        CHECK(ev[2].kind == Recorder::Event::Kind::up);

        // And the gesture really is over — a later frame has nothing left.
        editor.present_frame();
        CHECK(editor.rec.events.size() == 3);
    }
}

TEST_CASE("withdrawing the flush driver releases held motion instead of stranding it",
          "[view][pointer][coalescing][macos]") {
    @autoreleasepool {
        HostedEditor editor;
        if (!editor.usable()) {
            WARN("no PluginViewHost on this platform build");
            return;
        }
        REQUIRE(editor.host->pointer_coalescing_active());

        editor.down({10, 10});
        editor.drag({40, 10});
        CHECK(editor.rec.count(Recorder::Event::Kind::drag) == 0);  // held

        // Detaching stops the frame driver mid-drag. Nothing will flush after
        // this, so the withdrawal itself must deliver what is held — otherwise
        // the last movement before a detach is lost, and any later motion would
        // be held forever by a driver that no longer exists.
        editor.host->detach();
        CHECK_FALSE(editor.host->pointer_coalescing_active());
        REQUIRE(editor.rec.count(Recorder::Event::Kind::drag) == 1);
        CHECK(editor.rec.events.back().position.x == 40.0f);

        // With no driver, motion goes straight through, one dispatch per event
        // — the pre-coalescing behavior, which is the correct fail-safe.
        editor.drag({50, 10});
        editor.drag({60, 10});
        CHECK(editor.rec.count(Recorder::Event::Kind::drag) == 3);
    }
}

TEST_CASE("the GPU plugin view coalesces on the same contract",
          "[view][pointer][coalescing][macos][gpu]") {
    // PulpGpuPluginView is a DIFFERENT class from PulpPluginView with its own
    // ivars, its own dirty funnel (request_repaint rather than
    // -setNeedsDisplay:) and its own display-link start/stop. Everything above
    // could pass with this one entirely unwired, which matters because a
    // scripted/GPU editor is exactly where the slow drag was reported.
    @autoreleasepool {
        HostedEditor editor(/*use_gpu=*/true);
        if (!editor.usable()) {
            SUCCEED("no GPU-backed plugin host in this process — skipped");
            return;
        }
        REQUIRE(editor.host->is_gpu_backed());
        REQUIRE(editor.host->pointer_coalescing_active());

        editor.down({10, 10});
        for (int i = 1; i <= 6; ++i)
            editor.drag({10.0f + static_cast<float>(i) * 6.0f, 10.0f});
        CHECK(editor.rec.count(Recorder::Event::Kind::drag) == 0);  // held

        editor.present_frame();
        REQUIRE(editor.rec.count(Recorder::Event::Kind::drag) == 1);
        CHECK(editor.rec.events.back().position.x == 46.0f);

        // Terminal ordering holds here too.
        editor.drag({100, 20});
        editor.up({100, 20});
        const auto& ev = editor.rec.events;
        REQUIRE(ev.size() == 4);
        CHECK(ev[2].kind == Recorder::Event::Kind::drag);
        CHECK(ev[2].position.x == 100.0f);
        CHECK(ev[3].kind == Recorder::Event::Kind::up);
    }
}

// ── The detector itself ─────────────────────────────────────────────────────
//
// The host tests above assert the wiring is right. These assert that when it
// is wrong, something SAYS SO — which is the property that would have caught
// the standalone's hidden-frame-timer bug at the time instead of months later.

TEST_CASE("driving frames without declaring a driver is reported, not silent",
          "[view][pointer][coalescing]") {
    HostPointerInput input;
    std::vector<PointerSample> delivered;
    auto deliver = [&](const PointerSample& s) { delivered.push_back(s); };

    PointerSample motion;
    motion.phase = MousePhase::drag;
    motion.position = {5, 5};

    // A host that starts a frame driver but never declares it: samples take the
    // immediate path (correct, just uncoalesced) while frames tick past.
    CHECK_FALSE(input.submit(motion, deliver));
    CHECK(delivered.size() == 1);
    CHECK_FALSE(input.undriven_motion_detected());

    input.flush_for_frame(deliver);  // the frame tick that reveals it
    CHECK(input.undriven_motion_detected());
}

TEST_CASE("a correctly declared driver never trips the detector",
          "[view][pointer][coalescing]") {
    HostPointerInput input;
    std::vector<PointerSample> delivered;
    auto deliver = [&](const PointerSample& s) { delivered.push_back(s); };

    input.begin_flush_driver();
    PointerSample motion;
    motion.phase = MousePhase::drag;
    for (int i = 0; i < 5; ++i) {
        motion.position = {static_cast<float>(i), 0};
        input.submit(motion, deliver);
    }
    CHECK(delivered.empty());
    input.flush_for_frame(deliver);
    CHECK(delivered.size() == 1);
    CHECK_FALSE(input.undriven_motion_detected());
}

TEST_CASE("the coalescing stat line has a reachable negative",
          "[view][pointer][coalescing]") {
    // A diagnostic counter is only worth reading if its two halves CAN
    // disagree. If "received" and "merged" were derived from one another the
    // line would always look healthy and would prove only that the code ran —
    // a check with no reachable negative, whose agreement carries no
    // information. So drive the SAME input through both states and require the
    // reported ratio to come out different.
    auto drive = [](bool with_driver) {
        HostPointerInput input;
        auto sink = [](const PointerSample&) {};
        if (with_driver) input.begin_flush_driver();
        PointerSample motion;
        motion.phase = MousePhase::drag;
        for (int frame = 0; frame < 4; ++frame) {
            for (int i = 0; i < 6; ++i) {
                motion.position = {static_cast<float>(i), 0};
                input.submit(motion, sink);
            }
            if (with_driver) input.flush_for_frame(sink);
        }
        char line[256];
        const bool saw = input.format_stats(line, sizeof(line));
        return std::tuple<bool, std::size_t, std::size_t, std::string>{
            saw, input.total_motion_samples(), input.total_merged(), std::string(line)};
    };

    const auto [saw_on, received_on, merged_on, line_on] = drive(true);
    const auto [saw_off, received_off, merged_off, line_off] = drive(false);

    // Both arms received identical input, so "received" alone cannot tell them
    // apart — which is exactly why it is not the only number reported.
    CHECK(saw_on);
    CHECK(saw_off);
    CHECK(received_on == received_off);
    CHECK(received_on == 24u);

    // THE NEGATIVE. With no flush driver nothing can merge, so the ratio is
    // exactly 1.0; with one, 24 samples collapse to 4 frames' worth.
    CHECK(merged_off == 0u);
    CHECK(merged_on == 20u);
    CHECK(merged_on != merged_off);
    CHECK(line_on != line_off);

    INFO("driver active: " << line_on);
    INFO("no driver:     " << line_off);
    CHECK(line_off.find("driver=NONE") != std::string::npos);
    CHECK(line_on.find("driver=active") != std::string::npos);
}

TEST_CASE("the stat line refuses to look like a successful measurement when no motion arrived",
          "[view][pointer][coalescing]") {
    // The failure this exists for: a synthetic drag that lands on the wrong
    // window produces no input at all, and a counter that printed zeros would
    // read as a clean run. It must say so instead.
    HostPointerInput input;
    input.begin_flush_driver();
    char line[256];
    CHECK_FALSE(input.format_stats(line, sizeof(line)));
    CHECK(std::string(line).find("no pointer motion") != std::string::npos);
}

TEST_CASE("motion from before a driver existed is not reported as a bug",
          "[view][pointer][coalescing]") {
    // A view can legitimately receive input before it joins a window and
    // starts a link. Counting that history would make the detector cry wolf on
    // every editor open, and a detector that cries wolf gets ignored — which
    // costs more than not having one.
    HostPointerInput input;
    std::vector<PointerSample> delivered;
    auto deliver = [&](const PointerSample& s) { delivered.push_back(s); };

    PointerSample motion;
    motion.phase = MousePhase::drag;
    input.submit(motion, deliver);
    CHECK(delivered.size() == 1);

    input.begin_flush_driver();
    input.flush_for_frame(deliver);
    CHECK_FALSE(input.undriven_motion_detected());
}

#endif  // TARGET_OS_OSX
