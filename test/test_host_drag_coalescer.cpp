// HostDragCoalescer — the host-side half of per-frame drag coalescing.
//
// These cases pin the four rules a host must not re-derive, and they exist
// because the macOS PLUG-IN editor host shipped for months without any of
// them: `PointerCoalescer` and the per-presented-frame flush landed in the
// standalone window host, and the plug-in view — a different ObjC class — kept
// dispatching every raw AppKit drag event synchronously. That is the surface a
// musician actually drags inside a DAW, so it was the one that mattered most.
//
// The counting cases are the measurement the fix is judged by:
// `raw_samples` vs `delivered_samples` and `merged_samples` are the same
// quantities the standalone host reports as trace counters, so a host wired to
// this type reports them whether or not a trace session is running.
//
// The ordering cases matter more. A coalescer that ate or reordered a
// transition breaks a gesture's bracket — a press with no matching release
// leaves a DAW holding an automation touch — so those are asserted directly
// rather than inferred from a frame rate.
//
// The relative-delta case is the one a naive port gets wrong: coalescing keeps
// the newest POSITION, which is already the whole displacement, but a relative
// movement delta is per-event and must be SUMMED. Keeping only the survivor's
// delta shortens every relative drag in proportion to how many samples merged,
// which reads as "the knob feels sluggish" rather than as dropped input.

#include <catch2/catch_test_macros.hpp>

#include "pulp/view/host_drag_coalescer.hpp"

#include <vector>

using pulp::view::HostDragCoalescer;
using pulp::view::MouseButton;
using pulp::view::MousePhase;
using pulp::view::Point;
using pulp::view::PointerSample;

namespace {

PointerSample drag(float x, float y) {
    PointerSample s;
    s.position = Point{x, y};
    s.phase = MousePhase::drag;
    s.button = MouseButton::left;
    return s;
}

PointerSample drag_with_delta(float x, float y, float dx, float dy) {
    PointerSample s = drag(x, y);
    s.pointer.movement_x = dx;
    s.pointer.movement_y = dy;
    s.pointer.has_movement_delta = true;
    return s;
}

PointerSample transition(float x, float y, MousePhase phase) {
    PointerSample s;
    s.position = Point{x, y};
    s.phase = phase;
    s.button = MouseButton::left;
    return s;
}

/// Records what actually reached the view tree.
struct Recorder {
    std::vector<PointerSample> seen;
    HostDragCoalescer::Deliver deliver() {
        return [this](const PointerSample& s) { seen.push_back(s); };
    }
};

}  // namespace

TEST_CASE("A frame's worth of raw drag events reaches the tree once",
          "[view][pointer][coalesce][plugin-host]") {
    HostDragCoalescer c;
    Recorder rec;
    c.set_frame_driver_running(true, rec.deliver());

    // 60 raw AppKit samples inside one presented frame — an ordinary hand-drag
    // rate against an editor that is presenting slowly.
    for (int i = 1; i <= 60; ++i)
        c.submit(drag(static_cast<float>(i), 0.0f), rec.deliver());

    REQUIRE(rec.seen.empty());          // nothing dispatched yet
    REQUIRE(c.stats().raw_samples == 60);

    REQUIRE(c.flush_frame(rec.deliver()) == 1);
    REQUIRE(rec.seen.size() == 1);
    REQUIRE(rec.seen[0].position.x == 60.0f);   // the newest position survives

    const auto& s = c.stats();
    REQUIRE(s.raw_samples == 60);
    REQUIRE(s.delivered_samples == 1);
    REQUIRE(s.merged_samples == 59);
    REQUIRE(s.raw_samples - s.delivered_samples == s.merged_samples);
    REQUIRE(s.flushes == 1);
}

// The CONTROL for the case above. The same drive through the same instrument
// with coalescing OFF must deliver every sample and merge none: a zero merged
// count has to mean "coalescing did not happen", not "the recorder was never
// wired up". Without this, a broken fixture and a working fix read identically.
TEST_CASE("Control: the same drive with no frame driver delivers every sample",
          "[view][pointer][coalesce][plugin-host]") {
    HostDragCoalescer c;
    Recorder rec;
    // No set_frame_driver_running — the fail-safe path, which is exactly what
    // the plug-in host did on every raw AppKit event before it coalesced.
    REQUIRE_FALSE(c.frame_driver_running());

    for (int i = 1; i <= 60; ++i)
        c.submit(drag(static_cast<float>(i), 0.0f), rec.deliver());

    REQUIRE(rec.seen.size() == 60);
    REQUIRE(c.stats().raw_samples == 60);
    REQUIRE(c.stats().delivered_samples == 60);
    REQUIRE(c.stats().merged_samples == 0);
    REQUIRE_FALSE(c.has_pending());
}

TEST_CASE("The host is told to arm exactly one repaint per held run",
          "[view][pointer][coalesce][plugin-host]") {
    HostDragCoalescer c;
    Recorder rec;
    c.set_frame_driver_running(true, rec.deliver());

    int armed = 0;
    for (int i = 0; i < 40; ++i)
        if (c.submit(drag(static_cast<float>(i), 0.0f), rec.deliver()).arm_repaint)
            ++armed;
    REQUIRE(armed == 1);   // the idle -> pending edge, and only that

    c.flush_frame(rec.deliver());

    // A new run after the flush arms again — otherwise a frame driver that
    // gated on the dirty flag would never dispatch, and held motion would
    // strand forever.
    REQUIRE(c.submit(drag(99.0f, 0.0f), rec.deliver()).arm_repaint);
    REQUIRE_FALSE(c.submit(drag(100.0f, 0.0f), rec.deliver()).arm_repaint);
}

TEST_CASE("A release flushes held motion ahead of itself, never swallowing it",
          "[view][pointer][coalesce][plugin-host]") {
    HostDragCoalescer c;
    Recorder rec;
    c.set_frame_driver_running(true, rec.deliver());

    c.submit(drag(10.0f, 0.0f), rec.deliver());
    c.submit(drag(20.0f, 0.0f), rec.deliver());
    REQUIRE(rec.seen.empty());

    c.submit(transition(25.0f, 0.0f, MousePhase::release), rec.deliver());

    REQUIRE(rec.seen.size() == 2);
    REQUIRE(rec.seen[0].phase == MousePhase::drag);
    REQUIRE(rec.seen[0].position.x == 20.0f);
    REQUIRE(rec.seen[1].phase == MousePhase::release);
}

TEST_CASE("Merged relative movement deltas are summed, not replaced",
          "[view][pointer][coalesce][plugin-host]") {
    HostDragCoalescer c;
    Recorder rec;
    c.set_frame_driver_running(true, rec.deliver());

    // Ten one-pixel hops. A widget that integrates movement_x (a knob in
    // relative-mouse mode) must see the full travel, not the last hop.
    for (int i = 1; i <= 10; ++i)
        c.submit(drag_with_delta(static_cast<float>(i), static_cast<float>(2 * i),
                                 1.0f, 2.0f),
                 rec.deliver());

    REQUIRE(c.flush_frame(rec.deliver()) == 1);
    REQUIRE(rec.seen.size() == 1);
    REQUIRE(rec.seen[0].pointer.has_movement_delta);
    REQUIRE(rec.seen[0].pointer.movement_x == 10.0f);
    REQUIRE(rec.seen[0].pointer.movement_y == 20.0f);

    // And the accumulator is consumed, not carried into the next run.
    rec.seen.clear();
    c.submit(drag_with_delta(11.0f, 22.0f, 1.0f, 2.0f), rec.deliver());
    c.flush_frame(rec.deliver());
    REQUIRE(rec.seen.size() == 1);
    REQUIRE(rec.seen[0].pointer.movement_x == 1.0f);
    REQUIRE(rec.seen[0].pointer.movement_y == 2.0f);
}

TEST_CASE("Leaving the frame driver flushes; a later sample dispatches at once",
          "[view][pointer][coalesce][plugin-host]") {
    HostDragCoalescer c;
    Recorder rec;
    c.set_frame_driver_running(true, rec.deliver());
    c.submit(drag(5.0f, 0.0f), rec.deliver());
    REQUIRE(rec.seen.empty());

    c.set_frame_driver_running(false, rec.deliver());
    REQUIRE(rec.seen.size() == 1);            // held motion is not stranded
    REQUIRE(rec.seen[0].position.x == 5.0f);

    c.submit(drag(6.0f, 0.0f), rec.deliver());
    REQUIRE(rec.seen.size() == 2);            // fail-safe, delivered immediately
}

TEST_CASE("Teardown drops held motion instead of delivering it",
          "[view][pointer][coalesce][plugin-host]") {
    HostDragCoalescer c;
    Recorder rec;
    c.set_frame_driver_running(true, rec.deliver());
    c.submit(drag_with_delta(5.0f, 0.0f, 5.0f, 0.0f), rec.deliver());
    REQUIRE(c.has_pending());

    // The drag target may already be unmounted, so a teardown must not
    // dispatch into it.
    c.discard();
    REQUIRE_FALSE(c.has_pending());
    REQUIRE(c.flush_frame(rec.deliver()) == 0);
    REQUIRE(rec.seen.empty());

    // The dropped sample's delta went with it — it must not resurface on the
    // next gesture as a phantom jump.
    c.submit(drag_with_delta(1.0f, 0.0f, 1.0f, 0.0f), rec.deliver());
    c.flush_frame(rec.deliver());
    REQUIRE(rec.seen.size() == 1);
    REQUIRE(rec.seen[0].pointer.movement_x == 1.0f);
}
