// Phase 8 fidelity follow-up — verify the design-system widgets are actually
// WIRED (not just painted): driving each via its input handlers must change
// its value/state and fire its callback. Catches a widget that looks right but
// doesn't respond (the "knobs don't move" failure mode).

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/buttons.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;

TEST_CASE("Knob moves on drag", "[design-system][interaction]") {
    Knob k; k.set_bounds({0, 0, 80, 80}); k.set_value(0.5f);
    bool fired = false; k.on_change = [&](float) { fired = true; };
    k.on_mouse_down({40, 40});
    k.on_mouse_drag({40, 4});   // drag upward
    k.on_mouse_up({40, 4});
    REQUIRE(fired);
    REQUIRE(k.value() != 0.5f);
}

TEST_CASE("Fader moves on drag", "[design-system][interaction]") {
    Fader f; f.set_bounds({0, 0, 26, 160}); f.set_value(0.5f);
    bool fired = false; f.on_change = [&](float) { fired = true; };
    f.on_mouse_down({13, 80});
    f.on_mouse_drag({13, 8});
    f.on_mouse_up({13, 8});
    REQUIRE(fired);
    REQUIRE(f.value() != 0.5f);
}

TEST_CASE("RangeSlider moves on drag", "[design-system][interaction]") {
    RangeSlider s; s.set_bounds({0, 0, 220, 18}); s.set_min(0); s.set_max(1); s.set_value(0.2f);
    bool fired = false; s.on_change = [&](float) { fired = true; };
    MouseEvent ev{};
    ev.is_down = true;
    ev.position = {200.0f, 9.0f};   // near the right end
    s.on_mouse_event(ev);
    REQUIRE(fired);
    REQUIRE(s.value() > 0.2f);
}

TEST_CASE("Toggle flips on click", "[design-system][interaction]") {
    Toggle t; t.set_bounds({0, 0, 52, 30}); t.set_on(false);
    bool fired = false; t.on_toggle = [&](bool) { fired = true; };
    t.on_mouse_down({26, 15});
    REQUIRE(fired);
    REQUIRE(t.is_on());
}

TEST_CASE("Checkbox flips on click", "[design-system][interaction]") {
    Checkbox c; c.set_bounds({0, 0, 22, 22}); c.set_checked(false);
    bool fired = false; c.on_change = [&](bool) { fired = true; };
    c.on_mouse_down({11, 11});
    REQUIRE(fired);
    REQUIRE(c.is_checked());
}

TEST_CASE("Stepper increments/decrements on the +/- zones", "[design-system][interaction]") {
    Stepper s; s.set_bounds({0, 0, 140, 36}); s.set_range(-10, 10); s.set_step(1); s.set_value(0);
    int fired = 0; s.on_change = [&](double) { ++fired; };
    s.on_mouse_down({130, 18});   // + zone (x > w - h)
    REQUIRE(s.value() == 1.0);
    s.on_mouse_down({6, 18});     // - zone (x < h)
    REQUIRE(s.value() == 0.0);
    REQUIRE(fired == 2);
}

TEST_CASE("PanControl moves on drag", "[design-system][interaction]") {
    PanControl p; p.set_bounds({0, 0, 200, 18}); p.set_value(0.0f);
    bool fired = false; p.on_change = [&](float) { fired = true; };
    p.on_mouse_down({180, 9});    // right of center
    p.on_mouse_drag({180, 9});
    REQUIRE(fired);
    REQUIRE(p.value() > 0.0f);
}

TEST_CASE("XYPad moves on drag", "[design-system][interaction]") {
    XYPad pad; pad.set_bounds({0, 0, 120, 120});
    bool fired = false; pad.on_change = [&](float, float) { fired = true; };
    pad.on_mouse_down({30, 90});
    pad.on_mouse_drag({90, 30});
    REQUIRE(fired);
}

TEST_CASE("TextButton fires on click", "[design-system][interaction]") {
    TextButton b("Render"); b.set_bounds({0, 0, 110, 36});
    bool clicked = false; b.on_click = [&]() { clicked = true; };
    b.on_mouse_down({55, 18});
    REQUIRE(clicked);
}

TEST_CASE("TabPanel switches active tab on click", "[design-system][interaction]") {
    TabPanel t; t.set_bounds({0, 0, 300, 120});
    t.add_tab("Amp", std::make_unique<View>());
    t.add_tab("Filter", std::make_unique<View>());
    t.add_tab("FX", std::make_unique<View>());
    t.set_active_tab(0);
    MouseEvent ev{};
    ev.is_down = true;
    ev.position = {150.0f, 10.0f};   // middle tab, within the tab bar
    t.on_mouse_event(ev);
    REQUIRE(t.active_tab() == 1);
}
