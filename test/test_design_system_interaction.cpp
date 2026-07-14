// Verify the design-system widgets are actually wired, not just painted:
// driving each via its input handlers must change its value/state and fire its
// callback. Catches a widget that looks right but doesn't respond (the "knobs
// don't move" failure mode).

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/buttons.hpp>
#include <pulp/view/context_menu.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/midi_keyboard.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/side_panel.hpp>
#include <pulp/view/table.hpp>
#include <pulp/view/theme_presets.hpp>
#include <pulp/view/tree_view.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>

#include <array>
#include <cmath>
#include <type_traits>

using namespace pulp::view;

// Design-system Figma-name aliases must resolve to their canonical SDK class.
// See docs/reference/design-system-naming.md. These also force the alias
// headers to compile in CI.
static_assert(std::is_same_v<Sidebar, SidePanel>, "Sidebar must alias SidePanel");
static_assert(std::is_same_v<PopupMenu, ContextMenu>, "PopupMenu must alias ContextMenu");
static_assert(std::is_same_v<Table, TableListBox>, "Table must alias TableListBox");

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

TEST_CASE("Knob modulation rings round-trip + render", "[design-system][modulation]") {
    Knob k; k.set_bounds({0, 0, 92, 92}); k.set_value(0.5f);
    // {lo, hi, color}: a unipolar-positive ring and a unipolar-negative one.
    k.set_modulation_rings({{0.0f, 0.5f, Color::hex(0x5E78FF)},
                            {-0.3f, 0.0f, Color::hex(0xF6B847)}});
    REQUIRE(k.modulation_rings().size() == 2);
    REQUIRE(k.modulation_rings()[0].hi == 0.5f);
    REQUIRE(k.modulation_rings()[0].lo == 0.0f);
    REQUIRE(k.modulation_rings()[1].lo == -0.3f);
    REQUIRE(k.modulation_rings()[1].hi == 0.0f);
    // Plain knob (no rings) leaves the set empty — keeps the goldens unchanged.
    Knob plain;
    REQUIRE(plain.modulation_rings().empty());
}

TEST_CASE("Knob modulation range spans [base+lo, base+hi] and clips at limits",
          "[design-system][modulation]") {
    Knob k; k.set_bounds({0, 0, 92, 92}); k.set_value(0.5f);
    k.set_modulation_rings({{-0.3f, 0.3f, Color::hex(0x5E78FF)}});  // bipolar
    auto close = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };

    auto [lo, hi] = k.modulation_range(0);
    REQUIRE(close(lo, 0.2f));   // base+lo = 0.5 − 0.3
    REQUIRE(close(hi, 0.8f));   // base+hi = 0.5 + 0.3

    // Live indicator tracks the source phase across the range.
    k.set_modulation_phase(1.0f);
    REQUIRE(close(k.modulated_value(0), 0.8f));   // toward hi
    k.set_modulation_phase(-1.0f);
    REQUIRE(close(k.modulated_value(0), 0.2f));   // toward lo
    k.set_modulation_phase(0.0f);
    REQUIRE(close(k.modulated_value(0), 0.5f));

    // Near a limit the range clips instead of overshooting.
    k.set_value(0.9f);
    auto [lo2, hi2] = k.modulation_range(0);
    REQUIRE(close(hi2, 1.0f));   // 0.9 + 0.3 clipped to 1.0
    REQUIRE(close(lo2, 0.6f));
}

TEST_CASE("Knob modulation handle drag moves ONLY the grabbed endpoint",
          "[design-system][modulation]") {
    // Regression: a bipolar ring's two ends must move INDEPENDENTLY — dragging
    // the high handle must not shrink/grow the low handle (and vice-versa).
    Knob k; k.set_bounds({0, 0, 92, 92}); k.set_value(0.5f);
    k.set_modulation_rings({{-0.3f, 0.3f, Color::hex(0x5E78FF)}});  // lo=−0.3, hi=+0.3
    float base_before = k.value();
    int fired_ring = -2; float fired_lo = 999.0f, fired_hi = 999.0f;
    k.on_modulation_change = [&](int r, float lo, float hi) {
        fired_ring = r; fired_lo = lo; fired_hi = hi;
    };
    auto close = [](float a, float b) { return std::fabs(a - b) < 0.02f; };

    // Geometry mirrors knob_mod_geom (b=92): handles sit on the mod ring at the
    // value→angle of each endpoint.
    const float cx = 46.0f, cy = 46.0f, full_r = 43.0f;
    const float arc_w = std::max(3.0f, full_r * 0.13f);
    const float ring_r = full_r * 0.64f;
    const float mod_w = std::max(2.0f, full_r * 0.05f);
    const float mod_r = ring_r + arc_w * 0.5f + mod_w * 0.5f + 2.0f;
    const float start = 2.356f, sweep = 7.069f - 2.356f;
    auto angle = [&](float v) { return start + v * sweep; };
    auto pt = [&](float v) {
        return Point{cx + mod_r * std::cos(angle(v)), cy + mod_r * std::sin(angle(v))};
    };

    // Grab the HIGH handle (base+hi = 0.8) and drag it inward to base (0.5).
    k.on_mouse_down(pt(0.8f));
    REQUIRE(k.dragging_modulation());
    k.on_mouse_drag(pt(0.5f));
    REQUIRE(fired_ring == 0);
    REQUIRE(close(k.modulation_rings()[0].hi, 0.0f));    // high end moved to ~base
    REQUIRE(close(k.modulation_rings()[0].lo, -0.3f));   // LOW END UNCHANGED ← the fix
    REQUIRE(k.value() == base_before);                    // base value untouched
    k.on_mouse_up(pt(0.5f));
    REQUIRE_FALSE(k.dragging_modulation());

    // Now grab the LOW handle (base+lo = 0.2) and drag it; only lo changes.
    float hi_now = k.modulation_rings()[0].hi;
    k.on_mouse_down(pt(0.2f));
    REQUIRE(k.dragging_modulation());
    k.on_mouse_drag(pt(0.35f));
    REQUIRE(close(k.modulation_rings()[0].lo, -0.15f));   // low end → 0.35 − 0.5
    REQUIRE(close(k.modulation_rings()[0].hi, hi_now));   // HIGH END UNCHANGED
    k.on_mouse_up(pt(0.35f));

    // A press in the dial center is a value drag, not a handle drag.
    k.on_mouse_down({cx, cy});
    REQUIRE_FALSE(k.dragging_modulation());
}

TEST_CASE("Knob modulation handle clamps at the bottom gap instead of flipping",
          "[design-system][modulation]") {
    // Regression: dragging a handle off the bottom (into the dead-zone gap
    // between the arc's two ends) must clamp to the NEARER end, not wrap across
    // the gap to the opposite end (the handle "teleporting" to 5 o'clock).
    Knob k; k.set_bounds({0, 0, 92, 92}); k.set_value(0.5f);
    k.set_modulation_rings({{-0.3f, 0.3f, Color::hex(0x5E78FF)}});
    auto close = [](float a, float b) { return std::fabs(a - b) < 0.02f; };

    const float cx = 46.0f, cy = 46.0f, full_r = 43.0f;
    const float arc_w = std::max(3.0f, full_r * 0.13f);
    const float ring_r = full_r * 0.64f;
    const float mod_w = std::max(2.0f, full_r * 0.05f);
    const float mod_r = ring_r + arc_w * 0.5f + mod_w * 0.5f + 2.0f;
    const float start = 2.356f, sweep = 7.069f - 2.356f;
    auto angle = [&](float v) { return start + v * sweep; };

    // Grab the LOW handle (base+lo = 0.2) and drag past the parameter start
    // into the bottom gap, clearly on the START side of the gap (just below the
    // start boundary at ~7:30, short of the 6 o'clock midpoint).
    const float two_pi = 6.2831853f;
    float gap_a = (start + two_pi) - 0.25f;       // inside the gap, near the start
    Point gap_pt{cx + mod_r * std::cos(gap_a), cy + mod_r * std::sin(gap_a)};
    k.on_mouse_down({cx + mod_r * std::cos(angle(0.2f)), cy + mod_r * std::sin(angle(0.2f))});
    REQUIRE(k.dragging_modulation());
    k.on_mouse_drag(gap_pt);
    // Clamps at the start (value 0 → offset −0.5); does NOT flip to +0.5.
    REQUIRE(close(k.modulation_rings()[0].lo, -0.5f));
    REQUIRE(k.modulation_rings()[0].lo < 0.0f);   // stayed on the start side
    REQUIRE(close(k.modulation_rings()[0].hi, 0.3f));  // other end untouched
    k.on_mouse_up(gap_pt);
}

TEST_CASE("Knob modulation dot rides strictly between the two endpoints",
          "[design-system][modulation]") {
    auto close = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    auto sweep_within = [&](const Knob& k, float end_a, float end_b) {
        float lo = std::min(end_a, end_b), hi = std::max(end_a, end_b);
        for (float ph : {-1.0f, -0.5f, 0.0f, 0.4f, 1.0f}) {
            const_cast<Knob&>(k).set_modulation_phase(ph);
            float v = k.modulated_value(0);
            REQUIRE(v >= lo - 1e-3f);
            REQUIRE(v <= hi + 1e-3f);
        }
    };

    // Unipolar-positive: range is [base, base+hi]; the dot must NOT dip below
    // base toward where the old "phase 0 = base" model anchored it.
    {
        Knob k; k.set_bounds({0, 0, 92, 92}); k.set_value(0.5f);
        k.set_modulation_rings({{0.0f, 0.5f, Color::hex(0x5E78FF)}});  // lo=0, hi=0.5
        sweep_within(k, 0.5f, 1.0f);
        k.set_modulation_phase(-1.0f); REQUIRE(close(k.modulated_value(0), 0.5f));  // low end
        k.set_modulation_phase(1.0f);  REQUIRE(close(k.modulated_value(0), 1.0f));  // high end
    }
    // Inverted (lo > hi, as if the ends were dragged past each other): the dot
    // still stays between them, just sweeping the other direction.
    {
        Knob k; k.set_bounds({0, 0, 92, 92}); k.set_value(0.4f);
        k.set_modulation_rings({{0.5f, 0.2f, Color::hex(0xF6B847)}});  // lo=+0.5, hi=+0.2
        sweep_within(k, 0.9f, 0.6f);  // endpoints clamp to 0.9 and 0.6
        k.set_modulation_phase(-1.0f); REQUIRE(close(k.modulated_value(0), 0.9f));  // base+lo
        k.set_modulation_phase(1.0f);  REQUIRE(close(k.modulated_value(0), 0.6f));  // base+hi
    }
}

TEST_CASE("MidiKeyboard pressed key uses the accent color, not default black",
          "[design-system][midi-keyboard][regression]") {
    // Regression: canvas::Color default-constructs to OPAQUE black (a=1), so the
    // old `highlight_color_.a > 0` sentinel was always true and every pressed key
    // painted solid black instead of falling back to accent.primary.
    MidiKeyboard kb;
    kb.set_range(60, 72);                 // C4..C5 (8 white keys)
    kb.set_bounds({0, 0, 400, 90});
    if (const ThemePreset* p = find_preset("ink-signal"))
        kb.set_theme(theme_from_preset(*p, /*dark=*/true));  // accent.primary = teal

    uint32_t w = 0, h = 0;
    auto resting = render_to_rgba(kb, 400, 90, 1.0f, &w, &h);
    if (resting.empty() || w == 0) return;  // no Skia raster backend → skip

    // First white key spans the leftmost 1/8 width; sample its lower region,
    // below where the black keys reach, so only the white key paints there.
    const int px = 25, py = 75;
    auto sample = [&](const std::vector<uint8_t>& buf) {
        size_t i = (static_cast<size_t>(py) * w + static_cast<size_t>(px)) * 4;
        return std::array<int, 3>{buf[i], buf[i + 1], buf[i + 2]};
    };
    auto rest_px = sample(resting);

    kb.note_on(60, 0.9f);                 // press C4
    auto pressed = render_to_rgba(kb, 400, 90, 1.0f, &w, &h);
    REQUIRE_FALSE(pressed.empty());
    auto press_px = sample(pressed);

    REQUIRE(press_px != rest_px);                                   // press is visible
    bool near_black = press_px[0] < 40 && press_px[1] < 40 && press_px[2] < 40;
    REQUIRE_FALSE(near_black);                                      // ← the bug
    REQUIRE(press_px[1] > press_px[0] + 30);                        // accent (teal): green ≫ red
}

TEST_CASE("value widgets adjust on scroll wheel", "[design-system][interaction][wheel]") {
    // delta_y < 0 = scroll up = increase.
    Knob k; k.set_value(0.5f); bool kf = false; k.on_change = [&](float) { kf = true; };
    REQUIRE(k.wants_wheel_value());
    k.on_wheel(-10.0f); REQUIRE(k.value() > 0.5f); REQUIRE(kf);
    k.on_wheel(40.0f);  REQUIRE(k.value() < 0.6f);   // scroll down decreases

    Fader f; f.set_value(0.5f); bool ff = false; f.on_change = [&](float) { ff = true; };
    f.on_wheel(-10.0f); REQUIRE(f.value() > 0.5f); REQUIRE(ff);

    RangeSlider s; s.set_min(0); s.set_max(10); s.set_value(5.0f);
    bool sf = false; s.on_change = [&](float) { sf = true; };
    s.on_wheel(-10.0f); REQUIRE(s.value() > 5.0f); REQUIRE(sf);

    Stepper st; st.set_range(-10, 10); st.set_step(1); st.set_value(0);
    bool stf = false; st.on_change = [&](double) { stf = true; };
    st.on_wheel(-1.0f); REQUIRE(st.value() == 1.0); REQUIRE(stf);
    st.on_wheel(1.0f);  REQUIRE(st.value() == 0.0);

    PanControl pan; pan.set_value(0.0f); bool pf = false; pan.on_change = [&](float) { pf = true; };
    pan.on_wheel(-10.0f); REQUIRE(pan.value() > 0.0f); REQUIRE(pf);
}

TEST_CASE("ToggleButton radio group is mutually exclusive", "[design-system][interaction][radio]") {
    View parent;
    auto a = std::make_unique<ToggleButton>(); auto* pa = a.get(); pa->set_radio_group(1);
    auto b = std::make_unique<ToggleButton>(); auto* pb = b.get(); pb->set_radio_group(1);
    auto c = std::make_unique<ToggleButton>(); auto* pc = c.get(); pc->set_radio_group(1);
    parent.add_child(std::move(a));
    parent.add_child(std::move(b));
    parent.add_child(std::move(c));

    pa->on_mouse_down({0, 0});
    REQUIRE(pa->is_on());
    pb->on_mouse_down({0, 0});
    REQUIRE(pb->is_on());
    REQUIRE_FALSE(pa->is_on());          // selecting b deselected a
    pb->on_mouse_down({0, 0});
    REQUIRE(pb->is_on());                // clicking the active radio is a no-op
    pc->on_mouse_down({0, 0});
    REQUIRE(pc->is_on());
    REQUIRE_FALSE(pb->is_on());

    // Independent (group 0) toggles still flip both ways.
    ToggleButton ind;
    ind.on_mouse_down({0, 0}); REQUIRE(ind.is_on());
    ind.on_mouse_down({0, 0}); REQUIRE_FALSE(ind.is_on());
}

TEST_CASE("RangeSlider skew maps the midpoint to the track center", "[design-system][skew]") {
    auto close = [](float a, float b, float tol) { return std::fabs(a - b) < tol; };
    RangeSlider s; s.set_bounds({0, 0, 200, 18});
    s.set_min(20); s.set_max(20000);

    // Linear default: center of the track is the arithmetic midpoint.
    s.on_mouse_event([] { MouseEvent e; e.is_down = true; e.position = {100, 9}; return e; }());
    REQUIRE(close(s.value(), 10010.0f, 50.0f));

    // Log skew: the center of the track now yields ~1000 (the chosen midpoint),
    // and the thumb for value 1000 sits at the center (position ≈ 0.5).
    s.set_skew_from_midpoint(1000.0f);
    s.on_mouse_event([] { MouseEvent e; e.is_down = true; e.position = {100, 9}; return e; }());
    REQUIRE(close(s.value(), 1000.0f, 60.0f));
    s.set_value(1000.0f);
    REQUIRE(close(s.position_for_value(), 0.5f, 0.02f));
    // Monotonic: dragging right always increases.
    s.on_mouse_event([] { MouseEvent e; e.is_down = true; e.position = {160, 9}; return e; }());
    REQUIRE(s.value() > 1000.0f);
}

TEST_CASE("Knob/Fader skew round-trips position and value", "[design-system][skew]") {
    auto close = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    Knob k; k.set_skew_from_midpoint(0.25f);
    REQUIRE(close(k.value_for_position(0.5f), 0.25f));   // center → midpoint
    REQUIRE(close(k.position_for_value() , 0.0f));        // value 0 → pos 0
    k.set_value(0.25f);
    REQUIRE(close(k.position_for_value(), 0.5f));

    Fader f; f.set_skew(1.0f);                            // linear identity
    REQUIRE(close(f.value_for_position(0.3f), 0.3f));
    f.set_skew_from_midpoint(0.25f);
    REQUIRE(close(f.value_for_position(0.5f), 0.25f));
}

TEST_CASE("ChannelStrip fader + pan drag and fire callbacks", "[design-system][interaction]") {
    ChannelStrip cs;
    cs.set_bounds({0, 0, 84, 200});   // matches showcase strip size
    cs.set_level(0.7f); cs.set_pan(0.0f);
    bool lvl_fired = false, pan_fired = false;
    cs.on_level_change = [&](float) { lvl_fired = true; };
    cs.on_pan_change   = [&](float) { pan_fired = true; };

    // Fader column (top half) — drag near the top sets a high level.
    cs.on_mouse_down({50.0f, 20.0f});
    REQUIRE(lvl_fired);
    REQUIRE(cs.level() > 0.8f);
    // Drag toward the bottom of the fader span lowers it.
    cs.on_mouse_drag({50.0f, 150.0f});
    REQUIRE(cs.level() < 0.3f);

    // Pan row (bottom band, y >= h - 36) — drag left of center pans left.
    cs.on_mouse_down({10.0f, 175.0f});
    REQUIRE(pan_fired);
    REQUIRE(cs.pan() < 0.0f);

    // Scroll wheel nudges the level.
    float before = cs.level();
    cs.on_wheel(-1.0f);
    REQUIRE(cs.level() > before);
}

TEST_CASE("ComboBox dropdown flips above field near viewport bottom", "[design-system][interaction][dropdown]") {
    // ScrollView viewport 200px tall; a 3-item menu is 72px.
    ScrollView sv;
    sv.set_bounds({0, 0, 300, 200});
    sv.set_content_size({300, 1000});

    auto owned = std::make_unique<ComboBox>();
    ComboBox* combo = owned.get();
    combo->set_items({"Sine", "Saw", "Square"});
    sv.add_child(std::move(owned));

    // Near the top of the viewport → menu drops DOWN.
    combo->set_bounds({0, 10, 120, 28});
    REQUIRE_FALSE(combo->flips_up());

    // Near the bottom of the viewport → menu pops UP (no room below).
    combo->set_bounds({0, 170, 120, 28});
    REQUIRE(combo->flips_up());

    // Scrolling the field back up into open space flips it down again —
    // proves the decision tracks the field's ON-SCREEN position.
    sv.set_scroll(0, 160);   // field on-screen y ≈ 10
    REQUIRE_FALSE(combo->flips_up());
}

TEST_CASE("Scrolling closes an open ComboBox dropdown", "[design-system][interaction][dropdown]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 300, 200});
    sv.set_content_size({300, 1000});

    auto owned = std::make_unique<ComboBox>();
    ComboBox* combo = owned.get();
    combo->set_items({"Sine", "Saw", "Square"});
    combo->set_bounds({0, 40, 120, 28});
    sv.add_child(std::move(owned));

    MouseEvent down; down.is_down = true; down.position = {10, 10};
    combo->on_mouse_event(down);   // open
    REQUIRE(combo->is_open());

    sv.scroll_by(0, 30, /*animate=*/false);
    REQUIRE_FALSE(combo->is_open());
    ComboBox::close_active_popup();  // tidy the static slot for later cases
}

TEST_CASE("Stepper click-to-type edits the value", "[design-system][interaction]") {
    Stepper s; s.set_bounds({0, 0, 140, 36}); s.set_range(-99, 99); s.set_value(0);
    s.on_mouse_down({70.0f, 18.0f});   // center cell → begin editing
    REQUIRE(s.is_editing());
    TextInputEvent te; te.text = "12"; s.on_text_input(te);
    KeyEvent enter; enter.key = KeyCode::enter; enter.is_down = true;
    REQUIRE(s.on_key_event(enter));
    REQUIRE_FALSE(s.is_editing());
    REQUIRE(s.value() == 12.0);
    // Escape cancels (value unchanged).
    s.on_mouse_down({70.0f, 18.0f});
    TextInputEvent te2; te2.text = "99"; s.on_text_input(te2);
    KeyEvent esc; esc.key = KeyCode::escape; esc.is_down = true; s.on_key_event(esc);
    REQUIRE_FALSE(s.is_editing());
    REQUIRE(s.value() == 12.0);
    // The +/- zones still nudge.
    s.on_mouse_down({6.0f, 18.0f});    // minus zone
    REQUIRE(s.value() == 11.0);
}

TEST_CASE("GroupBox header click toggles collapse + child visibility",
          "[design-system][interaction]") {
    GroupBox g; g.set_bounds({0, 0, 280, 120}); g.set_collapsible(true);
    auto child = std::make_unique<View>();
    View* c = child.get();
    g.add_child(std::move(child));
    REQUIRE(c->visible());            // expanded → content shown

    MouseEvent e{}; e.is_down = true; e.position = {140.0f, 10.0f};  // header band
    g.on_mouse_event(e);
    REQUIRE(g.collapsed());
    REQUIRE_FALSE(c->visible());      // collapsed → content hidden

    g.on_mouse_event(e);              // click again → expand
    REQUIRE_FALSE(g.collapsed());
    REQUIRE(c->visible());

    // A click below the header (in the body) does NOT toggle.
    g.set_collapsed(false);
    MouseEvent body{}; body.is_down = true; body.position = {140.0f, 80.0f};
    g.on_mouse_event(body);
    REQUIRE_FALSE(g.collapsed());
}

TEST_CASE("DualRangeSlider drags the nearer thumb independently",
          "[design-system][interaction]") {
    DualRangeSlider d; d.set_bounds({0, 0, 360, 18});
    d.set_range(0, 1); d.set_low(0.25f); d.set_high(0.70f);

    // Grab near the HIGH thumb (~x 249) and drag right → high grows, low fixed.
    d.on_mouse_down({249.0f, 9.0f});
    d.on_mouse_drag({330.0f, 9.0f});
    REQUIRE(d.high() > 0.70f);
    REQUIRE(d.low() == 0.25f);
    d.on_mouse_up({330.0f, 9.0f});

    // Grab near the LOW thumb (~x 93) and drag left → low shrinks, high fixed.
    float hi_now = d.high();
    d.on_mouse_down({93.0f, 9.0f});
    d.on_mouse_drag({20.0f, 9.0f});
    REQUIRE(d.low() < 0.25f);
    REQUIRE(d.high() == hi_now);

    // Disabled ignores input.
    DualRangeSlider e; e.set_bounds({0, 0, 360, 18}); e.set_low(0.3f); e.set_enabled(false);
    e.on_mouse_down({200.0f, 9.0f});
    e.on_mouse_drag({10.0f, 9.0f});
    REQUIRE(e.low() == 0.3f);
}

TEST_CASE("TreeView scroll offset clamps so the top row is always reachable",
          "[design-system][interaction][tree]") {
    // The disclosure tree must clamp its scroll offset to [0, max] so a wheel
    // gesture can never leave the first row cut off above the top (the
    // "scroll won't reach the top" nit) nor overscroll past the last row.
    TreeView tree;
    tree.set_bounds({0, 0, 200, 66});   // viewport shows 3 rows at 22px each
    auto& root = tree.root();
    auto& a = root.add_child("A");      // 6 visible rows once expanded → 132px
    a.expanded = true;
    a.add_child("A1"); a.add_child("A2");
    auto& b = root.add_child("B");
    b.expanded = true;
    b.add_child("B1"); b.add_child("B2");

    REQUIRE(tree.visible_node_count() == 6);
    REQUIRE(tree.content_height() == 132.0f);
    REQUIRE(tree.max_scroll_offset() == 66.0f);  // 132 - 66

    // Negative / over-scroll both clamp into range.
    tree.set_scroll_offset(-50.0f);
    REQUIRE(tree.scroll_offset() == 0.0f);            // top reachable
    tree.set_scroll_offset(9999.0f);
    REQUIRE(tree.scroll_offset() == 66.0f);

    // A wheel scroll up from the bottom returns all the way to the very top.
    MouseEvent up{};
    up.is_wheel = true;
    up.scroll_delta_y = -1000.0f;
    tree.on_mouse_event(up);
    REQUIRE(tree.scroll_offset() == 0.0f);
}

TEST_CASE("Stepper scrubs on vertical drag and shows zone hover/press state",
          "[design-system][interaction]") {
    Stepper s; s.set_bounds({0, 0, 140, 36}); s.set_range(-99, 99);
    s.set_step(1); s.set_value(0);
    int fired = 0; s.on_change = [&](double) { ++fired; };

    // Drag up from the center cell increases; down decreases (snapped to step).
    s.on_mouse_down({70.0f, 18.0f});   // center press → begins as click-to-type
    s.on_mouse_drag({70.0f, 0.0f});    // 18px up → +3 steps at 6px/step
    REQUIRE(s.value() == 3.0);
    REQUIRE_FALSE(s.is_editing());     // a real drag cancels the would-be edit
    s.on_mouse_drag({70.0f, 54.0f});   // back down past start → negative
    REQUIRE(s.value() < 0.0);
    s.on_mouse_up({70.0f, 54.0f});
    REQUIRE(fired > 0);

    // Hover/press state tracks the −/+ zones for the affordance tint.
    s.on_hover_move({6.0f, 18.0f});    // minus zone
    REQUIRE(s.hovered_zone() == 0);
    s.on_hover_move({134.0f, 18.0f});  // plus zone
    REQUIRE(s.hovered_zone() == 1);
    s.on_mouse_leave();
    REQUIRE(s.hovered_zone() == -1);
    s.on_mouse_down({134.0f, 18.0f});  // press the plus zone
    REQUIRE(s.pressed_zone() == 1);
    s.on_mouse_up({134.0f, 18.0f});
    REQUIRE(s.pressed_zone() == -1);
}

TEST_CASE("NumberBox steps, click-to-types, and scrubs on vertical drag",
          "[design-system][interaction]") {
    NumberBox n; n.set_bounds({0, 0, 120, 32}); n.set_range(-99, 99);
    n.set_step(1); n.set_value(0);
    int fired = 0; n.on_change = [&](double) { ++fired; };

    // ‹ / › end zones step (zone width == height == 32).
    n.on_mouse_down({110.0f, 16.0f});  // › increment
    REQUIRE(n.value() == 1.0);
    n.on_mouse_up({110.0f, 16.0f});
    n.on_mouse_down({6.0f, 16.0f});    // ‹ decrement
    REQUIRE(n.value() == 0.0);
    n.on_mouse_up({6.0f, 16.0f});

    // Center click types a value (new capability, mirrors Stepper).
    n.on_mouse_down({60.0f, 16.0f});
    REQUIRE(n.is_editing());
    TextInputEvent te; te.text = "42"; n.on_text_input(te);
    KeyEvent enter; enter.key = KeyCode::enter; enter.is_down = true;
    REQUIRE(n.on_key_event(enter));
    REQUIRE_FALSE(n.is_editing());
    REQUIRE(n.value() == 42.0);

    // Center drag scrubs instead of editing.
    n.set_value(0);
    n.on_mouse_down({60.0f, 16.0f});
    n.on_mouse_drag({60.0f, -12.0f});  // 28px up from y=16 → +4 steps at 6px/step ≈ round(4.67)=5
    REQUIRE_FALSE(n.is_editing());
    REQUIRE(n.value() > 0.0);
    n.on_mouse_up({60.0f, -12.0f});

    // Hover state tracks the chevron zones.
    n.on_hover_move({6.0f, 16.0f});
    REQUIRE(n.hovered_zone() == 0);
    n.on_hover_move({114.0f, 16.0f});
    REQUIRE(n.hovered_zone() == 1);
    n.on_mouse_leave();
    REQUIRE(n.hovered_zone() == -1);
    REQUIRE(fired > 0);
}

TEST_CASE("InlineValueEditor caret blinks on the clock, not on paint",
          "[design-system][interaction][caret]") {
    // The caret's phase used to advance by a hardcoded 1/60 inside paint(), so
    // its rate followed the display's refresh and repainting alone could blink
    // it. Painting must be a pure function of the blink state.
    FrameClock clock;
    auto parent = std::make_unique<View>();
    parent->set_frame_clock(&clock);

    auto owned = std::make_unique<InlineValueEditor>();
    InlineValueEditor* e = owned.get();
    e->set_bounds({0, 0, 96, 26});
    e->set_range(-60, 0);
    e->set_value(-12.0);
    parent->add_child(std::move(owned));

    auto caret_strokes = [](const pulp::canvas::RecordingCanvas& c) {
        size_t n = 0;
        for (const auto& cmd : c.commands())
            if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_line &&
                std::abs(cmd.f[0] - cmd.f[2]) < 0.01f && cmd.f[3] > cmd.f[1])
                ++n;
        return n;
    };

    pulp::canvas::RecordingCanvas rec;
    e->paint(rec);
    REQUIRE(caret_strokes(rec) == 0);  // not editing: no caret at all

    e->begin_edit();
    rec.clear();
    e->paint(rec);
    REQUIRE(caret_strokes(rec) == 1);

    // 200 paints with the clock stopped. Under the old code this advanced the
    // phase by 3.3 s and the caret would have blinked away repeatedly.
    for (int i = 0; i < 200; ++i) {
        rec.clear();
        e->paint(rec);
        REQUIRE(caret_strokes(rec) == 1);
    }

    // Only real elapsed time turns it off.
    clock.tick(0.90f);
    rec.clear();
    e->paint(rec);
    REQUIRE(caret_strokes(rec) == 0);

    // A keystroke holds it solid again, mid-blink.
    TextInputEvent t{};
    t.text = "5";
    e->on_text_input(t);
    rec.clear();
    e->paint(rec);
    REQUIRE(caret_strokes(rec) == 1);

    // Committing retires the caret.
    e->commit_edit();
    rec.clear();
    e->paint(rec);
    REQUIRE(caret_strokes(rec) == 0);
}

TEST_CASE("InlineValueEditor click-to-type: commit / out-of-range / cancel",
          "[design-system][interaction]") {
    InlineValueEditor e; e.set_bounds({0, 0, 96, 26});
    e.set_range(-60, 0); e.set_decimals(1); e.set_value(-12.0);
    double committed = 999.0; e.on_change = [&](double v) { committed = v; };
    auto type = [&](const char* s) { TextInputEvent t{}; t.text = s; e.on_text_input(t); };
    auto key = [&](KeyCode k) { KeyEvent ev{}; ev.is_down = true; ev.key = k; e.on_key_event(ev); };

    // Click → edit, clear the prefilled buffer, type a valid value, Enter.
    e.on_mouse_down({48.0f, 13.0f});
    REQUIRE(e.editing());
    for (int i = 0; i < 8; ++i) key(KeyCode::backspace);
    type("-3.5"); key(KeyCode::enter);
    REQUIRE_FALSE(e.editing());
    REQUIRE(std::fabs(committed - (-3.5)) < 1e-6);
    REQUIRE(std::fabs(e.value() - (-3.5)) < 1e-6);
    REQUIRE_FALSE(e.invalid());

    // Out-of-range commit → flagged invalid, value unchanged.
    e.on_mouse_down({48.0f, 13.0f});
    for (int i = 0; i < 8; ++i) key(KeyCode::backspace);
    type("12"); key(KeyCode::enter);
    REQUIRE(e.invalid());
    REQUIRE(std::fabs(e.value() - (-3.5)) < 1e-6);

    // Esc cancels without committing.
    double before = e.value();
    e.on_mouse_down({48.0f, 13.0f});
    type("99"); key(KeyCode::escape);
    REQUIRE_FALSE(e.editing());
    REQUIRE(e.value() == before);
}

TEST_CASE("DualRangeSlider no_cross clamps a dragged thumb at the other",
          "[design-system][interaction]") {
    DualRangeSlider d; d.set_bounds({0, 0, 360, 18}); d.set_range(0, 1);
    d.set_low(0.30f); d.set_high(0.60f); d.set_no_cross(true);
    // Grab the low thumb (~x 111) and drag far right past the high thumb.
    d.on_mouse_down({111.0f, 9.0f});
    d.on_mouse_drag({350.0f, 9.0f});
    REQUIRE(d.low() <= d.high());                          // never crosses
    REQUIRE(std::fabs(d.low() - d.high()) < 1e-3f);        // clamps at high (0.60)
}
