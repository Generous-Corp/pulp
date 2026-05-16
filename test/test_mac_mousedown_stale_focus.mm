// pulp #2088 — pin the underlying invariant the mid-mouseDown stale-pointer
// fix in -[PulpView liveFocusedView] relies on.
//
// Background
// ----------
// `-[PulpView mouseDown:]` keeps an Obj-C ivar `_focusedView` that mirrors
// `pulp::view::View::focused_input_`. The static is auto-cleared by ~View()
// (#1708); the ivar is not. PR #1819 ("fix #1818") re-synced ONLY at the top
// of mouseDown — but mouseDown's own work (overlay dispatch, ComboBox
// routing, on_mouse_event → React unmount) can destroy the focused view
// MID-FUNCTION. Subsequent `_focusedView->...` derefs PAC-faulted (#2088).
//
// The complete fix routes every `_focusedView` deref through
// `-[liveFocusedView]`, which re-syncs from the static at each access site.
//
// What this test pins
// -------------------
// The accessor's correctness depends on ONE invariant: when a focused view
// is destroyed, `pulp::view::View::focused_input_` becomes nullptr. If that
// ever regresses, every PulpView callsite that reads through the accessor
// gets a stale pointer and PAC-faults the way #1818/#2088 did.
//
// This test:
//   1. Claim focus on view A → focused_input_ = A.
//   2. Cache the pointer (simulates PulpView's `_focusedView` ivar).
//   3. Destroy A. focused_input_ MUST become nullptr (#1708 contract).
//   4. Cache is now dangling; assert the live static differs — this is
//      exactly the condition `-[liveFocusedView]` checks before re-syncing.
//
// A full mouseDown integration test (click A → destroy A → click B → assert
// no crash) is harder to construct in the existing #2001 harness because
// `simulate_mouse` runs Yoga on the test root and the test view bounds
// don't survive layout in a way that makes hit_test deterministic. Filed as
// a follow-up gap in the harness; manually verified by rebuilding Spectr
// against this fix and confirming first-click no longer crashes.
//
// Tag [issue-2088] for coverage attribution.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/view.hpp>

using pulp::view::View;

namespace {

class TestView : public View {
public:
    void paint(pulp::canvas::Canvas&) override {}
};

struct FocusGuard {
    FocusGuard() { View::focused_input_ = nullptr; }
    ~FocusGuard() { View::focused_input_ = nullptr; }
};

}  // namespace

TEST_CASE("destroyed focused view must auto-clear focused_input_ "
          "(invariant -[PulpView liveFocusedView] depends on) [issue-2088]",
          "[view][focus][mac]") {
    FocusGuard guard;

    // Step 1: focus A through the public API.
    auto a = std::make_unique<TestView>();
    a->claim_input_focus();
    REQUIRE(View::focused_input_ == a.get());

    // Step 2: cache the pointer — simulates the way -[PulpView mouseDown:]
    // stores a parallel `_focusedView` ivar before dispatching nested work.
    auto* cached = View::focused_input_;
    REQUIRE(cached == a.get());

    // Step 3: destroy A. ~View must null `focused_input_` (#1708 contract).
    // If this regresses, every PulpView callsite that re-syncs from the
    // static will keep its stale ivar and crash on the next deref.
    a.reset();
    REQUIRE(View::focused_input_ == nullptr);

    // Step 4: the cached pointer is now dangling. -[PulpView liveFocusedView]
    // detects this exact condition by comparing the ivar to the live static
    // and resetting the ivar to nullptr before any deref. Pin the comparison
    // here so the contract that supports the fix can't silently change.
    REQUIRE(cached != View::focused_input_);
}

TEST_CASE("focus transfer between views keeps focused_input_ in sync "
          "[issue-2088]",
          "[view][focus][mac]") {
    FocusGuard guard;

    auto a = std::make_unique<TestView>();
    auto b = std::make_unique<TestView>();

    a->claim_input_focus();
    REQUIRE(View::focused_input_ == a.get());

    // Transfer focus to B. A's release_input_focus() should also clear the
    // static if A still owns it.
    a->release_input_focus();
    REQUIRE(View::focused_input_ == nullptr);

    b->claim_input_focus();
    REQUIRE(View::focused_input_ == b.get());

    // ~View on either view must clear if the destructed view holds focus.
    b.reset();
    REQUIRE(View::focused_input_ == nullptr);

    // A is still alive but doesn't hold focus — its destruction shouldn't
    // affect a stale slot.
    a.reset();
    REQUIRE(View::focused_input_ == nullptr);
}
