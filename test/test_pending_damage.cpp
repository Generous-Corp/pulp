// test_pending_damage.cpp — PendingDamage's consume/restore contract (WAH-8).
//
// PendingDamage is now the ONE damage abstraction behind both WindowHost and
// PluginViewHost. WindowHost used to carry its own `dirty_full_` /
// `have_dirty_bounds_` / `dirty_bounds_` trio plus a hand-rolled copy of the
// union logic, which is how the two hosts drifted apart on what a bounded mark
// even means.
//
// take()/restore() exist because "read three accessors, paint, then call
// clear()" is a shape that silently clears state the frame never painted —
// most importantly when the frame did not reach the screen at all.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/pending_damage.hpp>

using pulp::view::PendingDamage;
using pulp::view::Rect;

TEST_CASE("a fresh accumulator is full", "[pending-damage][wah-8]") {
    // The first frame of any surface has no previous content to preserve.
    PendingDamage d;
    REQUIRE(d.is_full());
    REQUIRE_FALSE(d.has_bounds());
}

TEST_CASE("full is sticky within a frame", "[pending-damage][wah-8]") {
    PendingDamage d;
    d.mark(Rect{10, 10, 20, 20});
    // Damage may only ever SHRINK a repaint. A bounded mark after a full one
    // must not narrow it, or a host that honours bounds would skip real work.
    REQUIRE(d.is_full());
}

TEST_CASE("a degenerate rect escalates to full", "[pending-damage][wah-8]") {
    PendingDamage d;
    d.clear();
    d.mark(Rect{10, 10, 0, 50});
    REQUIRE(d.is_full());
}

TEST_CASE("bounded marks union into a bounding box", "[pending-damage][wah-8]") {
    PendingDamage d;
    d.clear();
    d.mark(Rect{10, 10, 10, 10});
    d.mark(Rect{50, 60, 10, 10});

    REQUIRE_FALSE(d.is_full());
    REQUIRE(d.has_bounds());
    REQUIRE(d.bounds().x == 10.0f);
    REQUIRE(d.bounds().y == 10.0f);
    REQUIRE(d.bounds().x + d.bounds().width == 60.0f);
    REQUIRE(d.bounds().y + d.bounds().height == 70.0f);
}

TEST_CASE("take() reads and clears in one step", "[pending-damage][wah-8]") {
    PendingDamage d;
    d.clear();
    d.mark(Rect{5, 6, 7, 8});

    const auto snap = d.take();

    REQUIRE(snap.is_bounded());
    REQUIRE(snap.bounds().x == 5.0f);
    REQUIRE(snap.bounds().width == 7.0f);
    // Cleared, so a mark arriving after this point belongs to the NEXT frame.
    REQUIRE_FALSE(d.is_full());
    REQUIRE_FALSE(d.has_bounds());
}

TEST_CASE("take() of a full frame reports full", "[pending-damage][wah-8]") {
    PendingDamage d;  // starts full
    const auto snap = d.take();
    REQUIRE(snap.is_full());
    REQUIRE_FALSE(snap.is_bounded());
    REQUIRE_FALSE(d.is_full());
}

TEST_CASE("is_bounded() is false without bounds", "[pending-damage][wah-8]") {
    PendingDamage d;
    d.clear();
    const auto snap = d.take();
    REQUIRE_FALSE(snap.is_full());
    REQUIRE_FALSE(snap.has_bounds());
    REQUIRE_FALSE(snap.is_bounded());
}

TEST_CASE("restore() puts a bounded snapshot back", "[pending-damage][wah-8]") {
    PendingDamage d;
    d.clear();
    d.mark(Rect{5, 6, 7, 8});
    const auto snap = d.take();

    d.restore(snap);

    REQUIRE_FALSE(d.is_full());
    REQUIRE(d.has_bounds());
    REQUIRE(d.bounds().x == 5.0f);
    REQUIRE(d.bounds().width == 7.0f);
}

TEST_CASE("restore() of a full snapshot leaves the repaint full",
          "[pending-damage][wah-8]") {
    PendingDamage d;
    const auto snap = d.take();  // full
    d.clear();
    d.mark(Rect{1, 1, 2, 2});

    d.restore(snap);

    // Sticky-full still holds across a restore: the failed frame was going to
    // repaint everything, so the retry must too.
    REQUIRE(d.is_full());
}

TEST_CASE("restore() unions with damage marked while the frame was in flight",
          "[pending-damage][wah-8]") {
    PendingDamage d;
    d.clear();
    d.mark(Rect{0, 0, 10, 10});
    const auto snap = d.take();
    // A repaint request arriving mid-frame — the common case under a drag.
    d.mark(Rect{100, 100, 10, 10});

    d.restore(snap);

    // Overwriting instead of unioning would drop the newer region entirely.
    REQUIRE_FALSE(d.is_full());
    REQUIRE(d.bounds().x == 0.0f);
    REQUIRE(d.bounds().y == 0.0f);
    REQUIRE(d.bounds().x + d.bounds().width == 110.0f);
    REQUIRE(d.bounds().y + d.bounds().height == 110.0f);
}

TEST_CASE("restore() of an empty snapshot is a no-op",
          "[pending-damage][wah-8]") {
    PendingDamage d;
    d.clear();
    const auto snap = d.take();  // nothing pending
    d.mark(Rect{3, 3, 4, 4});

    d.restore(snap);

    REQUIRE_FALSE(d.is_full());
    REQUIRE(d.bounds().x == 3.0f);
}

TEST_CASE("take/restore round-trips repeatedly without drift",
          "[pending-damage][wah-8]") {
    // Models a host retrying a frame that keeps failing: the damage must stay
    // exactly as wide as it was, never silently shrinking to nothing.
    PendingDamage d;
    d.clear();
    d.mark(Rect{20, 30, 40, 50});

    for (int i = 0; i < 5; ++i) {
        const auto snap = d.take();
        REQUIRE(snap.is_bounded());
        d.restore(snap);
    }

    REQUIRE(d.bounds().x == 20.0f);
    REQUIRE(d.bounds().y == 30.0f);
    REQUIRE(d.bounds().width == 40.0f);
    REQUIRE(d.bounds().height == 50.0f);
}
