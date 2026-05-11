// pulp #1708 — global input-focus slot, auto-cleared on destruction.
//
// Pins the `View::focused_input_` mechanism that the macOS PulpView's
// keyDown / text-input dispatch consults instead of its own raw
// `_focusedView` pointer. Without auto-clear on destruction, a React
// unmount of the focused widget (e.g., closing a Settings modal)
// leaves a dangling pointer that the next keypress dereferences via
// dynamic_cast<TextEditor*> inside libc++abi — segfault.
//
// Contract under test:
//   1. focused_input_ defaults to nullptr.
//   2. claim_input_focus() / release_input_focus() toggle the slot
//      and don't null a third party's hold.
//   3. View destructor releases the slot if it currently holds it.

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

TEST_CASE("View::focused_input_ defaults to nullptr [issue-1708]",
          "[view][focus]") {
    FocusGuard g;
    REQUIRE(View::focused_input_ == nullptr);
}

TEST_CASE("View::claim_input_focus sets focused_input_ to this [issue-1708]",
          "[view][focus]") {
    FocusGuard g;
    TestView v;
    v.claim_input_focus();
    REQUIRE(View::focused_input_ == &v);
}

TEST_CASE("View::release_input_focus clears only if this holds it [issue-1708]",
          "[view][focus]") {
    FocusGuard g;
    TestView a;
    TestView b;

    a.claim_input_focus();
    REQUIRE(View::focused_input_ == &a);

    // Releasing a non-holder must NOT null a third party's slot.
    b.release_input_focus();
    REQUIRE(View::focused_input_ == &a);

    a.release_input_focus();
    REQUIRE(View::focused_input_ == nullptr);
}

// THE crash regression: destroying the focused widget must clear the
// global slot so the platform host's next text-input dispatch reads
// nullptr instead of a freed View*.
TEST_CASE("~View() clears focused_input_ if this holds it [issue-1708]",
          "[view][focus]") {
    FocusGuard g;
    {
        TestView v;
        v.claim_input_focus();
        REQUIRE(View::focused_input_ == &v);
    }  // v destructed here
    REQUIRE(View::focused_input_ == nullptr);
}

TEST_CASE("~View() does not clear focused_input_ if a sibling holds it [issue-1708]",
          "[view][focus]") {
    FocusGuard g;
    TestView holder;
    holder.claim_input_focus();
    REQUIRE(View::focused_input_ == &holder);
    {
        TestView sibling;
        // sibling never claimed focus
    }
    REQUIRE(View::focused_input_ == &holder);
    holder.release_input_focus();
}
