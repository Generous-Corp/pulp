// PointerCoalescer — one motion dispatch per presented frame.
//
// These cases pin the CONTRACT, not an implementation: a burst of motion
// inside one frame collapses to exactly one dispatch carrying the newest
// position; button transitions are never merged, never reordered, and never
// swallowed; and the merged path survives so a consumer that cannot
// reconstruct skipped motion still can.
//
// The ordering cases matter more than the counting ones. Spectr's editor
// authority hard-rejects a paint that arrives without a paint_start
// (editor_authority.cpp: "paint without paint_start"), so a coalescer that ate
// a transition breaks editing loudly rather than subtly — but only if the
// ordering is actually preserved, which is what these assert.

#include <catch2/catch_test_macros.hpp>

#include "pulp/view/pointer_coalescer.hpp"

using pulp::view::MouseButton;
using pulp::view::MousePhase;
using pulp::view::Point;
using pulp::view::PointerCoalescer;
using pulp::view::PointerSample;

namespace {

PointerSample motion(float x, float y,
                     MousePhase phase = MousePhase::drag,
                     int pointer_id = 0) {
    PointerSample s;
    s.position = Point{x, y};
    s.phase = phase;
    s.pointer.pointer_id = pointer_id;
    return s;
}

PointerSample transition(float x, float y, MousePhase phase) {
    PointerSample s;
    s.position = Point{x, y};
    s.phase = phase;
    return s;
}

}  // namespace

TEST_CASE("A burst of motion in one frame collapses to a single dispatch",
          "[view][pointer][coalesce]") {
    PointerCoalescer c;
    for (int i = 1; i <= 20; ++i) {
        REQUIRE(c.submit(motion(static_cast<float>(i), 0.0f)).empty());
    }
    REQUIRE(c.has_pending());

    const auto flushed = c.flush_frame();
    REQUIRE(flushed.size() == 1);
    REQUIRE(flushed[0].position.x == 20.0f);   // newest position survives
    REQUIRE(c.last_merged_count() == 19);      // the other 19 never dispatch
    REQUIRE_FALSE(c.has_pending());
}

TEST_CASE("Flushing with nothing pending dispatches nothing",
          "[view][pointer][coalesce]") {
    PointerCoalescer c;
    REQUIRE(c.flush_frame().empty());
    REQUIRE(c.flush_frame().empty());
    REQUIRE(c.total_merged() == 0);
}

TEST_CASE("Each presented frame gets its own motion sample",
          "[view][pointer][coalesce]") {
    // Coalescing must not mean "one dispatch per gesture" — a drag spanning
    // three frames must still move three times.
    PointerCoalescer c;
    c.submit(motion(1, 0));
    c.submit(motion(2, 0));
    const auto f1 = c.flush_frame();
    c.submit(motion(3, 0));
    c.submit(motion(4, 0));
    const auto f2 = c.flush_frame();

    REQUIRE(f1.size() == 1);
    REQUIRE(f2.size() == 1);
    REQUIRE(f1[0].position.x == 2.0f);
    REQUIRE(f2[0].position.x == 4.0f);
}

TEST_CASE("A button transition flushes held motion before itself",
          "[view][pointer][coalesce]") {
    // The ordering guarantee: a release must never reach a handler before the
    // motion that preceded it.
    PointerCoalescer c;
    c.submit(motion(5, 5));
    c.submit(motion(6, 6));

    const auto out = c.submit(transition(7, 7, MousePhase::release));
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].phase == MousePhase::drag);      // held motion, first
    REQUIRE(out[0].position.x == 6.0f);
    REQUIRE(out[1].phase == MousePhase::release);   // then the transition
    REQUIRE(out[1].position.x == 7.0f);
    REQUIRE_FALSE(c.has_pending());
}

TEST_CASE("A transition dispatches immediately rather than waiting for a frame",
          "[view][pointer][coalesce]") {
    // Deferring a press to the next presented frame would delay a click by a
    // whole frame — 370ms on the build that motivated this.
    PointerCoalescer c;
    const auto out = c.submit(transition(1, 1, MousePhase::press));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].phase == MousePhase::press);
    REQUIRE(c.flush_frame().empty());   // nothing left over for the frame
}

TEST_CASE("A press-drag-release gesture keeps every transition and its order",
          "[view][pointer][coalesce]") {
    PointerCoalescer c;
    std::vector<MousePhase> seen;
    auto run = [&](const std::vector<PointerSample>& v) {
        for (const auto& s : v) seen.push_back(s.phase);
    };

    run(c.submit(transition(0, 0, MousePhase::press)));
    for (int i = 1; i <= 50; ++i) run(c.submit(motion(static_cast<float>(i), 0)));
    run(c.flush_frame());
    for (int i = 51; i <= 100; ++i) run(c.submit(motion(static_cast<float>(i), 0)));
    run(c.submit(transition(100, 0, MousePhase::release)));

    REQUIRE(seen.size() == 4);
    REQUIRE(seen[0] == MousePhase::press);
    REQUIRE(seen[1] == MousePhase::drag);     // frame 1's coalesced motion
    REQUIRE(seen[2] == MousePhase::drag);     // flushed by the release
    REQUIRE(seen[3] == MousePhase::release);
    REQUIRE(c.total_merged() == 98);          // 100 motions -> 2 dispatches
}

TEST_CASE("The merged path is preserved for consumers that need it",
          "[view][pointer][coalesce]") {
    // A consumer that stamps one value per sample would draw a dotted line if
    // it only ever saw the final point, so the intermediate positions have to
    // survive the merge.
    PointerCoalescer c;
    c.submit(motion(1, 1));
    c.submit(motion(2, 4));
    c.submit(motion(3, 9));

    REQUIRE(c.pending_path().size() == 3);
    REQUIRE(c.pending_path().front().x == 1.0f);
    REQUIRE(c.pending_path().back().y == 9.0f);

    c.flush_frame();
    REQUIRE(c.pending_path().empty());
}

TEST_CASE("Hover and drag motion are not merged into each other",
          "[view][pointer][coalesce]") {
    // Losing the drag/hover distinction would turn a button-held move into a
    // hover, which is a different gesture entirely.
    PointerCoalescer c;
    c.submit(motion(1, 0, MousePhase::hover));
    const auto out = c.submit(motion(2, 0, MousePhase::drag));

    REQUIRE(out.size() == 1);
    REQUIRE(out[0].phase == MousePhase::hover);
    REQUIRE(out[0].position.x == 1.0f);

    const auto rest = c.flush_frame();
    REQUIRE(rest.size() == 1);
    REQUIRE(rest[0].phase == MousePhase::drag);
}

TEST_CASE("Motion from a second pointer is not merged into the first",
          "[view][pointer][coalesce]") {
    // Two simultaneous touches share this coalescer; merging them would
    // fabricate a path that crosses between fingers.
    PointerCoalescer c;
    c.submit(motion(10, 10, MousePhase::drag, /*pointer_id=*/0));
    const auto out = c.submit(motion(90, 90, MousePhase::drag, /*pointer_id=*/1));

    REQUIRE(out.size() == 1);
    REQUIRE(out[0].pointer.pointer_id == 0);
    REQUIRE(out[0].position.x == 10.0f);

    const auto rest = c.flush_frame();
    REQUIRE(rest.size() == 1);
    REQUIRE(rest[0].pointer.pointer_id == 1);
}

TEST_CASE("An automatic-phase sample is never merged",
          "[view][pointer][coalesce]") {
    // `automatic` means "infer from is_down" and carries no promise about
    // whether it is a press, so merging it could reorder a transition.
    PointerCoalescer c;
    c.submit(motion(1, 0));
    const auto out = c.submit(transition(2, 0, MousePhase::automatic));
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].phase == MousePhase::drag);
    REQUIRE(out[1].phase == MousePhase::automatic);
}

TEST_CASE("Reset drops held motion without dispatching it",
          "[view][pointer][coalesce]") {
    // A host tearing a window down mid-gesture must be able to abandon held
    // motion whose target may already be gone.
    PointerCoalescer c;
    c.submit(motion(1, 0));
    c.submit(motion(2, 0));
    REQUIRE(c.has_pending());

    c.reset();
    REQUIRE_FALSE(c.has_pending());
    REQUIRE(c.flush_frame().empty());
}
