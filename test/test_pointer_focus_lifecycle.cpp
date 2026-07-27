// test_pointer_focus_lifecycle.cpp — context-menu routing, button identity, and
// focus transfer.
//
// Split out of test_pointer_dispatch.cpp (WAH-7), which had grown to ~1,370
// lines covering focus, coordinate mapping, delivery/capture/reentrancy, and
// gestures in one file. Those are four different subjects with four different
// reasons to change; a failure in one told you almost nothing about where to
// look.
//
// The fixtures below stay LOCAL to this file rather than moving to a shared
// helper. They are used only here, and a shared test-helper header is the
// thing a split like this most easily degrades into.

// Right-click routing and root→local coordinate conversion.
//
// These were previously inlined in the macOS hosts, so a regression could only
// be caught by clicking a real NSView. The plugin host in particular had no
// right-button path at all, which left every in-DAW context menu dead.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/gesture.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <string>
#include <vector>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

// A view that records where a context-menu click landed, in its own local space.
class ContextSpy : public View {
public:
    ContextSpy() {
        on_context_menu = [this](Point p) {
            ++hits;
            last = p;
        };
    }
    int hits = 0;
    Point last{};
};

class FocusCallbackSpy : public View {
public:
    void on_focus_changed(bool gained) override {
        View::on_focus_changed(gained);
        if (callback) callback(gained);
    }

    std::function<void(bool)> callback;
};

}  // namespace

TEST_CASE("dispatch_context_menu routes a right-click to the view under it", "[view][input]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto child = std::make_unique<ContextSpy>();
    ContextSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    SECTION("inside the child: handler fires with child-local coordinates") {
        REQUIRE(dispatch_context_menu(root, {130, 70}));
        REQUIRE(spy->hits == 1);
        CHECK_THAT(spy->last.x, WithinAbs(30.0, 1e-4));  // 130 - 100
        CHECK_THAT(spy->last.y, WithinAbs(20.0, 1e-4));  // 70  - 50
    }

    SECTION("outside the child: no handler on the root, so nothing is consumed") {
        REQUIRE_FALSE(dispatch_context_menu(root, {10, 10}));
        REQUIRE(spy->hits == 0);
    }
}

TEST_CASE("dispatch_context_menu reports not-consumed without a handler", "[view][input]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<View>();
    child->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    // No on_context_menu anywhere: the host must fall through to its own menu.
    REQUIRE_FALSE(dispatch_context_menu(root, {50, 50}));
}

TEST_CASE("focus transfer stops when a callback unmounts the requested target",
          "[view][input][focus]") {
    View::focused_input_ = nullptr;
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto previous = std::make_unique<FocusCallbackSpy>();
    auto* previous_ptr = previous.get();
    previous->set_focusable(true);
    root.add_child(std::move(previous));

    auto requested = std::make_unique<FocusCallbackSpy>();
    auto* requested_ptr = requested.get();
    requested->set_focusable(true);
    root.add_child(std::move(requested));

    previous_ptr->claim_input_focus();
    std::unique_ptr<View> detached;
    previous_ptr->callback = [&](bool gained) {
        if (!gained) detached = root.remove_child(requested_ptr);
    };

    REQUIRE_FALSE(transfer_input_focus(root, requested_ptr));
    REQUIRE(detached.get() == requested_ptr);
    REQUIRE(View::focused_input_ == nullptr);
}

TEST_CASE("focus transfer keeps a live non-focusable pointer target",
          "[view][input][focus]") {
    View::focused_input_ = nullptr;
    View root;
    auto child = std::make_unique<View>();
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    REQUIRE(transfer_input_focus(root, child_ptr));
    REQUIRE(View::focused_input_ == nullptr);
}

TEST_CASE("focus transfer rejects a target unmounted before focus starts",
          "[view][input][focus]") {
    View::focused_input_ = nullptr;
    View root;
    auto child = std::make_unique<View>();
    auto* stale_identity = child.get();
    root.add_child(std::move(child));
    auto detached = root.remove_child(stale_identity);

    REQUIRE_FALSE(transfer_input_focus(root, stale_identity));
    REQUIRE(detached.get() == stale_identity);
    REQUIRE(View::focused_input_ == nullptr);
}

TEST_CASE("focus transfer does not claim a target that unmounts while gaining focus",
          "[view][input][focus]") {
    View::focused_input_ = nullptr;
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto requested = std::make_unique<FocusCallbackSpy>();
    auto* requested_ptr = requested.get();
    requested->set_focusable(true);
    root.add_child(std::move(requested));

    std::unique_ptr<View> detached;
    requested_ptr->callback = [&](bool gained) {
        if (gained) detached = root.remove_child(requested_ptr);
    };

    REQUIRE_FALSE(transfer_input_focus(root, requested_ptr));
    REQUIRE(detached.get() == requested_ptr);
    REQUIRE(View::focused_input_ == nullptr);
}

TEST_CASE("focus transfer reads and mutates only the requested root slot",
          "[view][input][focus][multi-root]") {
    View root_a;
    View root_b;
    auto a = std::make_unique<FocusCallbackSpy>();
    auto b = std::make_unique<FocusCallbackSpy>();
    auto* a_ptr = a.get();
    auto* b_ptr = b.get();
    a->set_focusable(true);
    b->set_focusable(true);
    root_a.add_child(std::move(a));
    root_b.add_child(std::move(b));

    a_ptr->claim_input_focus();
    b_ptr->claim_input_focus();  // process-global shim now points at B
    REQUIRE(root_a.interaction().focused_input == a_ptr);
    REQUIRE(root_b.interaction().focused_input == b_ptr);
    REQUIRE(focused_input_under_root(root_a) == a_ptr);

    transfer_input_focus(root_a, nullptr);
    CHECK(root_a.interaction().focused_input == nullptr);
    CHECK(root_b.interaction().focused_input == b_ptr);
    CHECK(focused_input_under_root(root_b) == b_ptr);
}

TEST_CASE("explicit focus target wins over focus selected by a blur callback",
          "[view][input][focus][reentrancy]") {
    View root;
    auto previous = std::make_unique<FocusCallbackSpy>();
    auto requested = std::make_unique<FocusCallbackSpy>();
    auto replacement = std::make_unique<FocusCallbackSpy>();
    auto* previous_ptr = previous.get();
    auto* requested_ptr = requested.get();
    auto* replacement_ptr = replacement.get();
    previous->set_focusable(true);
    requested->set_focusable(true);
    replacement->set_focusable(true);
    root.add_child(std::move(previous));
    root.add_child(std::move(requested));
    root.add_child(std::move(replacement));
    previous_ptr->claim_input_focus();
    previous_ptr->callback = [&](bool gained) {
        if (!gained) {
            replacement_ptr->on_focus_changed(true);
            replacement_ptr->claim_input_focus();
        }
    };

    REQUIRE(transfer_input_focus(root, requested_ptr));
    CHECK(root.interaction().focused_input == requested_ptr);
    CHECK(focused_input_under_root(root) == requested_ptr);
    CHECK(requested_ptr->has_focus());
    CHECK_FALSE(replacement_ptr->has_focus());
}

TEST_CASE("explicit blur clears focus selected by a blur callback",
          "[view][input][focus][reentrancy]") {
    View root;
    auto previous = std::make_unique<FocusCallbackSpy>();
    auto replacement = std::make_unique<FocusCallbackSpy>();
    auto* previous_ptr = previous.get();
    auto* replacement_ptr = replacement.get();
    previous->set_focusable(true);
    replacement->set_focusable(true);
    root.add_child(std::move(previous));
    root.add_child(std::move(replacement));
    previous_ptr->on_focus_changed(true);
    previous_ptr->claim_input_focus();
    previous_ptr->callback = [&](bool gained) {
        if (!gained) {
            replacement_ptr->on_focus_changed(true);
            replacement_ptr->claim_input_focus();
        }
    };

    REQUIRE_FALSE(transfer_input_focus(root, nullptr));
    CHECK(root.interaction().focused_input == nullptr);
    CHECK(focused_input_under_root(root) == nullptr);
    CHECK_FALSE(previous_ptr->has_focus());
    CHECK_FALSE(replacement_ptr->has_focus());
}

TEST_CASE("explicit blur terminates a cyclic focus callback chain",
          "[view][input][focus][reentrancy]") {
    View root;
    auto a = std::make_unique<FocusCallbackSpy>();
    auto b = std::make_unique<FocusCallbackSpy>();
    auto* a_ptr = a.get();
    auto* b_ptr = b.get();
    a->set_focusable(true);
    b->set_focusable(true);
    root.add_child(std::move(a));
    root.add_child(std::move(b));
    a_ptr->on_focus_changed(true);
    a_ptr->claim_input_focus();
    a_ptr->callback = [&](bool gained) {
        if (!gained) {
            b_ptr->on_focus_changed(true);
            b_ptr->claim_input_focus();
        }
    };
    b_ptr->callback = [&](bool gained) {
        if (!gained) {
            a_ptr->on_focus_changed(true);
            a_ptr->claim_input_focus();
        }
    };

    REQUIRE_FALSE(transfer_input_focus(root, nullptr));
    CHECK(root.interaction().focused_input == nullptr);
    CHECK_FALSE(a_ptr->has_focus());
    CHECK_FALSE(b_ptr->has_focus());
}
