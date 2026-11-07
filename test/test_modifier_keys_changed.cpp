// test_modifier_keys_changed.cpp — modifier-key change notifications.
//
// Pressing or releasing Shift (or any modifier) with no other key reaches the
// focused view and every view a pointer press is captured by, so a control can
// switch to fine adjustment after its drag has already begun.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/view.hpp>

#include <memory>
#include <vector>

using namespace pulp::view;

namespace {

class ModifierSpy : public View {
public:
    void on_modifier_keys_changed(Modifiers m) override { seen.push_back(m); }
    std::vector<Modifiers> seen;
};

struct Fixture {
    View root;
    ModifierSpy* knob = nullptr;
    ModifierSpy* field = nullptr;
    ModifierSpy* bystander = nullptr;

    Fixture() {
        root.set_bounds({0, 0, 400, 200});
        auto add = [&](float x) {
            auto v = std::make_unique<ModifierSpy>();
            auto* raw = v.get();
            raw->set_bounds({x, 10, 80, 80});
            raw->set_focusable(true);
            root.add_child(std::move(v));
            return raw;
        };
        knob = add(10);
        field = add(110);
        bystander = add(210);
    }
};

}  // namespace

TEST_CASE("a modifier change reaches the view a drag is captured by",
          "[view][input][modifiers]") {
    Fixture f;

    REQUIRE(deliver_mouse_down(f.root, f.knob, {30, 30}, 0));
    deliver_mouse_drag(f.root, f.knob, {30, 40}, 0);

    CHECK(f.root.simulate_modifier_keys(kModShift));
    REQUIRE(f.knob->seen.size() == 1);
    CHECK(f.knob->seen[0] == kModShift);

    // Release Shift: the full set held is now empty.
    CHECK(f.root.simulate_modifier_keys(kModNone));
    REQUIRE(f.knob->seen.size() == 2);
    CHECK(f.knob->seen[1] == kModNone);

    CHECK(f.field->seen.empty());
    CHECK(f.bystander->seen.empty());
}

TEST_CASE("a modifier change reaches the focused view",
          "[view][input][modifiers]") {
    Fixture f;
    REQUIRE(transfer_input_focus(f.root, f.field));

    CHECK(f.root.simulate_modifier_keys(kModShift | kModAlt));
    REQUIRE(f.field->seen.size() == 1);
    CHECK(f.field->seen[0] == (kModShift | kModAlt));
    CHECK(f.knob->seen.empty());
}

TEST_CASE("a view that is both focused and captured is told once",
          "[view][input][modifiers]") {
    Fixture f;
    REQUIRE(transfer_input_focus(f.root, f.knob));
    REQUIRE(deliver_mouse_down(f.root, f.knob, {30, 30}, 0));

    f.root.simulate_modifier_keys(kModShift);
    CHECK(f.knob->seen.size() == 1);
}

TEST_CASE("after the release no view is captured and nothing is delivered",
          "[view][input][modifiers]") {
    Fixture f;
    REQUIRE(deliver_mouse_down(f.root, f.knob, {30, 30}, 0));
    deliver_mouse_up(f.root, f.knob, {30, 30}, 0, 1, MouseUpHost{});

    CHECK_FALSE(f.root.simulate_modifier_keys(kModShift));
    CHECK(f.knob->seen.empty());
}

TEST_CASE("a cancelled press also ends the capture",
          "[view][input][modifiers]") {
    Fixture f;
    REQUIRE(deliver_mouse_down(f.root, f.knob, {30, 30}, 0));
    deliver_mouse_cancel(f.root, f.knob, {30, 30}, 0, 1, MouseButton::left,
                         PointerAttributes{});
    CHECK_FALSE(f.root.simulate_modifier_keys(kModShift));
}

TEST_CASE("a disabled view is not told about modifier changes",
          "[view][input][modifiers][disabled]") {
    Fixture f;
    REQUIRE(transfer_input_focus(f.root, f.field));
    f.field->set_enabled(false);
    CHECK_FALSE(f.root.simulate_modifier_keys(kModShift));
    CHECK(f.field->seen.empty());
}

TEST_CASE("each touch pointer keeps its own captured view",
          "[view][input][modifiers]") {
    Fixture f;
    PointerAttributes a, b;
    a.type = b.type = PointerType::touch;
    a.pointer_id = 1;
    b.pointer_id = 2;
    REQUIRE(deliver_mouse_down(f.root, f.knob, {30, 30}, 0, 1, true,
                               MouseButton::left, MouseDownHost{}, a));
    REQUIRE(deliver_mouse_down(f.root, f.bystander, {230, 30}, 0, 1, true,
                               MouseButton::left, MouseDownHost{}, b));

    f.root.simulate_modifier_keys(kModShift);
    CHECK(f.knob->seen.size() == 1);
    CHECK(f.bystander->seen.size() == 1);

    deliver_mouse_up(f.root, f.knob, {30, 30}, 0, 1, MouseUpHost{},
                     MouseButton::left, a);
    f.root.simulate_modifier_keys(kModNone);
    CHECK(f.knob->seen.size() == 1);
    CHECK(f.bystander->seen.size() == 2);
}

TEST_CASE("a buttonless move ends a press whose release was lost",
          "[view][input][modifiers]") {
    Fixture f;
    REQUIRE(deliver_mouse_down(f.root, f.knob, {30, 30}, 0));
    deliver_hover_move(f.root, {300, 150});  // button is plainly up
    CHECK_FALSE(f.root.simulate_modifier_keys(kModShift));
    CHECK(f.knob->seen.empty());
}

TEST_CASE("a press whose target left the tree is still released",
          "[view][input][modifiers]") {
    Fixture f;
    REQUIRE(deliver_mouse_down(f.root, f.knob, {30, 30}, 0));
    auto removed = f.root.remove_child(f.knob);
    deliver_mouse_up(f.root, removed.get(), {30, 30}, 0, 1, MouseUpHost{});
    CHECK_FALSE(f.root.simulate_modifier_keys(kModShift));
}
