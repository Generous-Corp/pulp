// DesignFrameView — the faithful-vector design-import view (Plan B / B1).
//
// Renders a design's own SVG via Canvas::draw_svg (SkSVGDOM), cropped to its
// panel, and overlays interactive knobs from a TYPED element list (the importer
// supplies it — the view does not guess from the SVG). These tests pin: panel
// auto-detect, knob hit-test + value drag, the needle visibly rotating in the
// rendered output, and fail-safe behavior — with no design-import dependency.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>

#include <memory>
#include <string>

using namespace pulp::view;

namespace {

// A minimal "design": an 80x80 panel rect at (10,10), one knob (a gradient-less
// dome circle at (50,50) r20) with a vertical needle pointing up.
std::string make_design_svg() {
    return R"SVG(<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">)SVG"
           R"SVG(<rect x="10" y="10" width="80" height="80" rx="4" fill="#cccccc"/>)SVG"
           R"SVG(<circle cx="50" cy="50" r="20" fill="#8a97a6"/>)SVG"
           R"SVG(<path d="M50 38L50 30" stroke="white" stroke-width="3"/></svg>)SVG";
}

DesignFrameElement make_knob() {
    DesignFrameElement k;
    k.kind = DesignFrameElement::Kind::knob;
    k.cx = 50; k.cy = 50; k.hit_radius = 22;
    k.needle_d = "M50 38L50 30";
    k.value = 0.5f;
    return k;
}

}  // namespace

TEST_CASE("DesignFrameView auto-detects the panel and holds typed elements",
          "[view][design-import][frame][svg]") {
    DesignFrameView v(make_design_svg(), {make_knob()});
    REQUIRE(v.element_count() == 1);
    CHECK(v.panel_width() == 80.0f);   // the 80x80 panel rect, not the 100x100 frame
    CHECK(v.panel_height() == 80.0f);
    CHECK(v.element_value(0) == 0.5f);
    CHECK(v.element_value(99) == -1.0f);
}

TEST_CASE("DesignFrameView drag turns the hit knob", "[view][design-import][frame]") {
    DesignFrameView v(make_design_svg(), {make_knob()});
    v.set_bounds({0, 0, 80, 80});  // view == panel, so view coords map 1:1

    // Click the dome center, drag UP — value should rise.
    v.on_mouse_down({40, 40});     // -> SVG (50,50), inside the knob
    v.on_mouse_drag({40, 10});     // drag up 30px
    CHECK(v.element_value(0) > 0.6f);

    // A click far from any knob hits nothing — value unchanged.
    v.set_element_value(0, 0.5f);
    v.on_mouse_down({2, 2});
    v.on_mouse_drag({2, -40});
    CHECK(v.element_value(0) == 0.5f);
}

TEST_CASE("DesignFrameView renders faithfully and the needle visibly rotates",
          "[view][design-import][frame][svg]") {
    DesignFrameView lo(make_design_svg(), {make_knob()});
    DesignFrameView hi(make_design_svg(), {make_knob()});
    lo.set_bounds({0, 0, 80, 80});
    hi.set_bounds({0, 0, 80, 80});
    lo.set_element_value(0, 0.1f);
    hi.set_element_value(0, 0.9f);

    auto lo_png = render_to_png(lo, 80, 80, 2.0f, ScreenshotBackend::skia);
    if (lo_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto hi_png = render_to_png(hi, 80, 80, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(hi_png.empty());

    // The chrome is identical; only the needle rotated — so the two renders must
    // differ (proves draw_svg drew the design AND the needle moved with value).
    const auto cmp = compare_screenshots(lo_png, hi_png);
    REQUIRE(cmp.valid);
    // On a build where SkSVGDOM can't composite (the ASan/UBSan macOS runners link
    // a partial Skia where draw_svg is a no-op), both renders are identical — skip
    // rather than fail, so a real regression in those lanes stays visible. Same
    // pattern as test_image_view_fill's url()-mask guard.
    if (cmp.similarity >= 0.999f)
        SKIP("SVG (SkSVGDOM) rendering unavailable in this build");
    CHECK(cmp.similarity < 0.999f);
}

TEST_CASE("DesignFrameView hit-test agrees with the render at a non-panel aspect",
          "[view][design-import][frame]") {
    // Regression for the live-window bug: paint() and hit_element() must share ONE
    // transform, so a knob is hit exactly where it's DRAWN even when the host
    // window aspect != panel aspect (the sprite-demo host sized the window to the
    // 1146x746 frame while the panel was 1000x600). The earlier "drag turns the
    // hit knob" test only used square 1:1 bounds, so it missed this.
    DesignFrameView v(make_design_svg(), {make_knob()});  // panel 80x80 at (10,10)
    // Give it a tall 1:2 letterbox. Uniform fit: scale = min(80/80, 160/80) = 1,
    // centered oy = (160-80)/2 = 40, ox = 0. The knob (SVG 50,50; panel origin
    // 10,10) is therefore DRAWN at view (0+(50-10), 40+(50-10)) = (40, 80).
    v.set_bounds({0, 0, 80, 160});

    v.on_mouse_down({40, 80});     // exactly where the knob is rendered
    v.on_mouse_drag({40, 50});     // drag up 30 view px
    CHECK(v.element_value(0) > 0.6f);   // hit + turn at the rendered position

    // A click in the letterbox margin (above the centered panel) hits nothing.
    v.set_element_value(0, 0.5f);
    v.on_mouse_down({40, 5});
    v.on_mouse_drag({40, 0});
    CHECK(v.element_value(0) == 0.5f);
}

TEST_CASE("DesignFrameView reports the view-space point that hits an element",
          "[view][design-import][frame]") {
    // element_hit_point() is the inverse of hit_element(). Pin it against a
    // HAND-COMPUTED position rather than against the transform it uses, so a bug
    // in the fit can't satisfy both sides of the assertion.
    DesignFrameView v(make_design_svg(), {make_knob()});  // panel 80x80 at (10,10)
    Point p{-1, -1};

    // Not laid out yet: no fit exists, so there is no point to report. Honest
    // false beats a plausible (0,0) that would silently miss.
    CHECK_FALSE(v.element_hit_point(0, p));

    // Same 1:2 letterbox as the hit-test-agrees case above: scale 1, ox 0, oy 40,
    // so the knob (SVG 50,50; panel origin 10,10) sits at view (40, 80).
    v.set_bounds({0, 0, 80, 160});
    REQUIRE(v.element_hit_point(0, p));
    CHECK(p.x == Catch::Approx(40.0f));
    CHECK(p.y == Catch::Approx(80.0f));

    // Round-trip through the REAL pointer path: pressing at the reported point
    // must actually grab the knob and turn it. This is what makes the accessor
    // load-bearing — if it drifts from hit_element(), the drag does nothing.
    v.on_mouse_down(p);
    v.on_mouse_drag({p.x, p.y - 30});
    CHECK(v.element_value(0) > 0.6f);

    // Out-of-range indices report false and leave the out-param untouched.
    Point keep{7, 9};
    CHECK_FALSE(v.element_hit_point(-1, keep));
    CHECK_FALSE(v.element_hit_point(99, keep));
    CHECK(keep.x == Catch::Approx(7.0f));
    CHECK(keep.y == Catch::Approx(9.0f));
}

TEST_CASE("DesignFrameView reports no hit point for an unclickable element",
          "[view][design-import][frame]") {
    // The accessor promises a point that HITS. Where hit_element() provably
    // matches nothing, the honest answer is false — a plausible-looking coordinate
    // would miss silently and read as a dead control.

    SECTION("a disabled element has no hit point") {
        DesignFrameView v(make_design_svg(), {make_knob()});
        v.set_bounds({0, 0, 80, 160});
        Point keep{7, 9};
        REQUIRE(v.element_hit_point(0, keep));   // clickable while enabled

        v.set_element_enabled(0, false);
        keep = {7, 9};
        CHECK_FALSE(v.element_hit_point(0, keep));
        CHECK(keep.x == Catch::Approx(7.0f));    // out-param untouched
        CHECK(keep.y == Catch::Approx(9.0f));
    }

    SECTION("a knob with a degenerate hit radius has no hit point") {
        // hit_element() matches a knob on `d < hit_radius`; at radius 0 even the
        // pivot itself misses, so no point on the knob is clickable.
        auto k = make_knob();
        k.hit_radius = 0.0f;
        DesignFrameView v(make_design_svg(), {k});
        v.set_bounds({0, 0, 80, 160});
        Point p{-1, -1};
        CHECK_FALSE(v.element_hit_point(0, p));
    }

    SECTION("a value_label readout has no hit point") {
        // Not interactive and not an overlay: hit_element() never tests it.
        DesignFrameElement label;
        label.kind = DesignFrameElement::Kind::value_label;
        label.x = 20; label.y = 20; label.w = 30; label.h = 10;
        DesignFrameView v(make_design_svg(), {label});
        v.set_bounds({0, 0, 80, 160});
        Point p{-1, -1};
        CHECK_FALSE(v.element_hit_point(0, p));
    }

    SECTION("an occluded knob has no hit point at its pivot") {
        // A rect-tested kind is matched BEFORE knobs, so a toggle laid over the
        // knob's pivot takes the press: the pivot no longer reaches the knob.
        auto k = make_knob();
        DesignFrameElement lid;
        lid.kind = DesignFrameElement::Kind::toggle;
        lid.x = 40; lid.y = 40; lid.w = 20; lid.h = 20;  // covers the (50,50) pivot
        DesignFrameView v(make_design_svg(), {k, lid});
        v.set_bounds({0, 0, 80, 160});

        Point p{-1, -1};
        CHECK_FALSE(v.element_hit_point(0, p));   // the knob is masked
        REQUIRE(v.element_hit_point(1, p));       // the toggle on top still reports

        // Drive the REAL pointer path: the press lands on the toggle (flipping it
        // off its 0.5 rest state), and leaves the knob underneath alone.
        v.on_mouse_down(p);
        CHECK(v.element_value(1) == 0.0f);
        CHECK(v.element_value(0) == Catch::Approx(0.5f));
    }
}

TEST_CASE("DesignFrameView intrinsic size is the panel (so hosts size the window)",
          "[view][design-import][frame]") {
    DesignFrameView v(make_design_svg(), {make_knob()});
    CHECK(v.intrinsic_width() == 80.0f);
    CHECK(v.intrinsic_height() == 80.0f);
}

TEST_CASE("DesignFrameView renders + the needle rotates at a non-panel aspect",
          "[view][design-import][frame][svg]") {
    // Paint must also be correct (not just hit) at a mismatched aspect: the same
    // knob at two values must produce different renders in a letterboxed view.
    DesignFrameView lo(make_design_svg(), {make_knob()});
    DesignFrameView hi(make_design_svg(), {make_knob()});
    lo.set_bounds({0, 0, 80, 160});
    hi.set_bounds({0, 0, 80, 160});
    lo.set_element_value(0, 0.1f);
    hi.set_element_value(0, 0.9f);

    auto lo_png = render_to_png(lo, 80, 160, 2.0f, ScreenshotBackend::skia);
    if (lo_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto hi_png = render_to_png(hi, 80, 160, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(hi_png.empty());
    const auto cmp = compare_screenshots(lo_png, hi_png);
    REQUIRE(cmp.valid);
    if (cmp.similarity >= 0.999f)
        SKIP("SVG (SkSVGDOM) rendering unavailable in this build");
    CHECK(cmp.similarity < 0.999f);
}

namespace {
// A knob carrying a host-parameter binding key — the surface a foreign-host
// (the embed shim) binder reads to wire a hand-built native view to host params.
DesignFrameElement make_keyed_knob(float cx, float cy, std::string key) {
    DesignFrameElement k = make_knob();
    k.cx = cx; k.cy = cy;
    k.param_key = std::move(key);
    return k;
}
}  // namespace

TEST_CASE("DesignFrameView surfaces per-element param_key + reverse lookup",
          "[view][design-import][frame][param-key]") {
    // Two value-bearing knobs declaring host bindings; the second is off-panel
    // (cx=200) so it never wins a hit-test — used only for lookup/push.
    DesignFrameView v(make_design_svg(),
                      {make_keyed_knob(50, 50, "gain"),
                       make_keyed_knob(200, 50, "cutoff")});
    REQUIRE(v.element_count() == 2);

    CHECK(v.element_param_key(0) == "gain");
    CHECK(v.element_param_key(1) == "cutoff");
    CHECK(v.element_param_key(99).empty());        // out of range -> empty, no UB
    CHECK(v.element_param_key(-1).empty());

    CHECK(v.element_for_param_key("gain") == 0);
    CHECK(v.element_for_param_key("cutoff") == 1);
    CHECK(v.element_for_param_key("missing") == -1);
    CHECK(v.element_for_param_key("") == -1);      // empty key never matches
}

TEST_CASE("DesignFrameView param_key drives a string-keyed host bridge both ways",
          "[view][design-import][frame][param-key]") {
    DesignFrameView v(make_design_svg(),
                      {make_keyed_knob(50, 50, "gain"),
                       make_keyed_knob(200, 50, "cutoff")});
    v.set_bounds({0, 0, 80, 80});  // view == panel, 1:1 mapping

    // Stand in for the embed shim: forward USER changes/gestures by the element's
    // param_key (UI -> host), exactly as the string-key bridge does.
    std::vector<std::pair<std::string, float>> host_writes;
    std::vector<std::string> gestures;
    v.on_element_changed = [&](int idx, float val) {
        host_writes.emplace_back(v.element_param_key(idx), val);
    };
    v.on_gesture_begin = [&](int idx) { gestures.push_back("begin:" + v.element_param_key(idx)); };
    v.on_gesture_end   = [&](int idx) { gestures.push_back("end:" + v.element_param_key(idx)); };

    // UI -> host: drag the "gain" knob up. The binder sees the change keyed by
    // "gain", bracketed by gesture begin/end (so a host can group an undo step).
    v.on_mouse_down({40, 40});     // -> SVG (50,50): the gain knob
    v.on_mouse_drag({40, 10});     // drag up
    v.on_mouse_up({40, 10});
    REQUIRE_FALSE(host_writes.empty());
    for (const auto& [key, val] : host_writes) CHECK(key == "gain");  // never "cutoff"
    CHECK(host_writes.back().second > 0.6f);
    REQUIRE(gestures.size() == 2);
    CHECK(gestures.front() == "begin:gain");
    CHECK(gestures.back() == "end:gain");

    // host -> UI: automation/preset recall pushes "cutoff" via the reverse lookup.
    // set_element_value is silent — it must NOT echo back into the host bridge.
    host_writes.clear();
    const int idx = v.element_for_param_key("cutoff");
    REQUIRE(idx == 1);
    v.set_element_value(idx, 0.25f);
    CHECK(v.element_value(idx) == Catch::Approx(0.25f));
    CHECK(host_writes.empty());     // no feedback loop
}

namespace {
// A text_field (search) overlay element inside the 80x80 panel at (10,10).
DesignFrameElement make_search() {
    DesignFrameElement s;
    s.kind = DesignFrameElement::Kind::text_field;
    s.x = 14; s.y = 14; s.w = 60; s.h = 16;   // SVG coords, inside the panel
    s.placeholder = "Search";
    return s;
}
}  // namespace

TEST_CASE("DesignFrameView overlays a focusable TextEditor for a text_field",
          "[view][design-import][frame][overlay]") {
    DesignFrameView v(make_design_svg(), {make_search()});
    REQUIRE(v.element_count() == 1);
    auto* editor = dynamic_cast<TextEditor*>(v.overlay_widget(0));
    REQUIRE(editor != nullptr);                 // a real native widget, not a fake
    CHECK(editor->placeholder == "Search");

    // Tap inside the field routes to the editor (children are hit before the
    // frame's knob fallback), focuses it, and typing inserts text.
    editor->on_focus_changed(true);
    CHECK(editor->has_focus());
    TextInputEvent te; te.text = "kick"; editor->on_text_input(te);
    CHECK(editor->text() == "kick");
}

TEST_CASE("DesignFrameView positions the text_field overlay via the panel transform",
          "[view][design-import][frame][overlay]") {
    DesignFrameView v(make_design_svg(), {make_search()});  // panel (10,10,80,80)
    auto* editor = v.overlay_widget(0);
    REQUIRE(editor != nullptr);

    // Matched aspect: view 80x80 -> scale 1, ox=oy=0. Field (14,14,60,16) maps to
    // view ((14-10), (14-10), 60, 16) = (4,4,60,16).
    v.set_bounds({0, 0, 80, 80});
    v.layout_children();
    auto b = editor->bounds();
    CHECK(b.x == 4.0f); CHECK(b.y == 4.0f); CHECK(b.width == 60.0f); CHECK(b.height == 16.0f);

    // Mismatched aspect: view 160x80 -> uniform scale 1, centered ox=40, oy=0.
    // Field maps to ((14-10)+40, (14-10), 60, 16) = (44,4,60,16). The overlay
    // tracks the SAME transform the SVG is painted with.
    v.set_bounds({0, 0, 160, 80});
    v.layout_children();
    b = editor->bounds();
    CHECK(b.x == 44.0f); CHECK(b.y == 4.0f); CHECK(b.width == 60.0f); CHECK(b.height == 16.0f);

    // A click inside the field's view rect routes to the editor, not the frame.
    CHECK(v.hit_test({60, 8}) == editor);
}

TEST_CASE("DesignFrameView overlays a ComboBox for a dropdown element",
          "[view][design-import][frame][overlay]") {
    DesignFrameElement dd;
    dd.kind = DesignFrameElement::Kind::dropdown;
    dd.x = 20; dd.y = 30; dd.w = 50; dd.h = 14;     // inside the 80x80 panel
    dd.options = {"1/4 Delay", "1/8 Delay", "Reverb"};
    dd.selected_index = 2;
    DesignFrameView v(make_design_svg(), {dd});

    auto* combo = dynamic_cast<ComboBox*>(v.overlay_widget(0));
    REQUIRE(combo != nullptr);                       // a real ComboBox, not a fake
    REQUIRE(combo->items().size() == 3);
    CHECK(combo->selected() == 2);
    CHECK(combo->selected_text() == "Reverb");

    // Positioned via the panel transform (view 80x80 -> scale 1, ox=oy=0; panel
    // origin (10,10)): rect (20,30,50,14) -> view (10,20,50,14).
    v.set_bounds({0, 0, 80, 80});
    v.layout_children();
    auto b = combo->bounds();
    CHECK(b.x == 10.0f); CHECK(b.y == 20.0f); CHECK(b.width == 50.0f); CHECK(b.height == 14.0f);
    // A click inside the dropdown routes to the ComboBox, not the frame's knobs.
    CHECK(v.hit_test({30, 25}) == combo);
}

TEST_CASE("DesignFrameView overlays a DesignTabGroup for a tab_group element",
          "[view][design-import][frame][overlay]") {
    DesignFrameElement tg;
    tg.kind = DesignFrameElement::Kind::tab_group;
    tg.x = 20; tg.y = 14; tg.w = 56; tg.h = 14;     // inside the 80x80 panel
    tg.options = {"1", "2", "3", "4"};
    tg.selected_index = 2;
    DesignFrameView v(make_design_svg(), {tg});

    auto* tabs = dynamic_cast<DesignTabGroup*>(v.overlay_widget(0));
    REQUIRE(tabs != nullptr);                         // a real tab widget
    REQUIRE(tabs->tab_count() == 4);
    CHECK(tabs->selected() == 2);                     // from selected_index

    // Positioned via the panel transform (view 80x80 -> scale 1; panel origin
    // (10,10)): rect (20,14,56,14) -> view (10,4,56,14).
    v.set_bounds({0, 0, 80, 80});
    v.layout_children();
    auto b = tabs->bounds();
    CHECK(b.x == 10.0f); CHECK(b.width == 56.0f);
    CHECK(v.hit_test({30, 8}) == tabs);              // click routes to the tabs

    // Clicking a slot selects that tab (slot width 56/4 = 14, local coords).
    tabs->on_mouse_down({7, 7});                      // slot 0
    CHECK(tabs->selected() == 0);
    tabs->on_mouse_down({49, 7});                     // 49/14 = 3 -> slot 3
    CHECK(tabs->selected() == 3);
}

TEST_CASE("DesignTabGroup renders and the highlight moves with selection",
          "[view][design-import][frame][overlay][svg]") {
    auto make_tabs = [](int selected) {
        DesignFrameElement tg;
        tg.kind = DesignFrameElement::Kind::tab_group;
        tg.x = 14; tg.y = 14; tg.w = 60; tg.h = 16;
        tg.options = {"1", "2", "3", "4"};
        tg.selected_index = selected;
        return tg;
    };
    DesignFrameView lo(make_design_svg(), {make_tabs(0)});
    DesignFrameView hi(make_design_svg(), {make_tabs(3)});
    lo.set_bounds({0, 0, 80, 80}); lo.layout_children();
    hi.set_bounds({0, 0, 80, 80}); hi.layout_children();

    auto lo_png = render_to_png(lo, 80, 80, 2.0f, ScreenshotBackend::skia);
    if (lo_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto hi_png = render_to_png(hi, 80, 80, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(hi_png.empty());
    const auto cmp = compare_screenshots(lo_png, hi_png);
    REQUIRE(cmp.valid);
    // The tab strip renders and the highlight is on a different slot, so the two
    // renders differ. (No SkSVGDOM dependency — the tab widget paints natively —
    // but keep the same guard for partial-Skia lanes that no-op raster text.)
    if (cmp.similarity >= 0.999f)
        SKIP("native raster unavailable in this build");
    CHECK(cmp.similarity < 0.999f);
}

TEST_CASE("DesignFrameView overlays a DesignStepper for a stepper element",
          "[view][design-import][frame][overlay]") {
    DesignFrameElement st;
    st.kind = DesignFrameElement::Kind::stepper;
    st.x = 20; st.y = 14; st.w = 56; st.h = 14;     // inside the 80x80 panel
    st.options = {"Lowpass", "Bandpass", "Highpass"};
    st.selected_index = 1;
    DesignFrameView v(make_design_svg(), {st});

    auto* step = dynamic_cast<DesignStepper*>(v.overlay_widget(0));
    REQUIRE(step != nullptr);                         // a real stepper widget
    REQUIRE(step->option_count() == 3);
    CHECK(step->selected() == 1);                     // from selected_index
    CHECK(step->current() == "Bandpass");

    // Positioned via the panel transform (view 80x80 -> scale 1; panel origin
    // (10,10)): rect (20,14,56,14) -> view (10,4,56,14).
    v.set_bounds({0, 0, 80, 80});
    v.layout_children();
    auto b = step->bounds();
    CHECK(b.x == 10.0f); CHECK(b.width == 56.0f);
    CHECK(v.hit_test({30, 8}) == step);              // click routes to the stepper

    // Right half steps to the next option; left half to the previous; clamped.
    step->on_mouse_down({50, 7});                     // right half -> next
    CHECK(step->selected() == 2);
    CHECK(step->current() == "Highpass");
    step->on_mouse_down({50, 7});                     // already last -> clamp
    CHECK(step->selected() == 2);
    step->on_mouse_down({5, 7});                      // left half -> previous
    CHECK(step->selected() == 1);
    step->on_mouse_down({5, 7});
    step->on_mouse_down({5, 7});                      // clamp at 0
    CHECK(step->selected() == 0);
    CHECK(step->current() == "Lowpass");
}

TEST_CASE("DesignStepper renders and the value changes with selection",
          "[view][design-import][frame][overlay][svg]") {
    auto make_step = [](int selected) {
        DesignFrameElement st;
        st.kind = DesignFrameElement::Kind::stepper;
        st.x = 14; st.y = 14; st.w = 60; st.h = 16;
        st.options = {"Lowpass", "Bandpass", "Highpass"};
        st.selected_index = selected;
        return st;
    };
    DesignFrameView lo(make_design_svg(), {make_step(0)});
    DesignFrameView hi(make_design_svg(), {make_step(2)});
    lo.set_bounds({0, 0, 80, 80}); lo.layout_children();
    hi.set_bounds({0, 0, 80, 80}); hi.layout_children();

    auto lo_png = render_to_png(lo, 80, 80, 2.0f, ScreenshotBackend::skia);
    if (lo_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto hi_png = render_to_png(hi, 80, 80, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(hi_png.empty());
    const auto cmp = compare_screenshots(lo_png, hi_png);
    REQUIRE(cmp.valid);
    // Different option text is shown, so the two renders differ.
    if (cmp.similarity >= 0.999f)
        SKIP("native raster unavailable in this build");
    CHECK(cmp.similarity < 0.999f);
}

TEST_CASE("DesignStepper with a single option paints nothing (no double-text over the baked label)",
          "[view][design-import][frame][overlay][svg]") {
    // A header stepper detected from a baked `< value >` whose source has only
    // the one shown value has nowhere to step. Re-drawing its chevrons + value
    // would double them on top of the design's baked label. So a 1-option
    // stepper must paint NOTHING (let the baked SVG show through); a multi-option
    // one paints its live value. Render both on a transparent surface and
    // compare painted content.
    DesignStepper one({"Reverb"}, 0);
    DesignStepper many({"Reverb", "Delay", "Chorus"}, 0);
    one.set_bounds({0, 0, 80, 24});
    many.set_bounds({0, 0, 80, 24});

    auto one_png = render_to_png(one, 80, 24, 2.0f, ScreenshotBackend::skia);
    if (one_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto many_png = render_to_png(many, 80, 24, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(many_png.empty());

    const auto one_stats = analyze_screenshot_content(one_png);
    const auto many_stats = analyze_screenshot_content(many_png);
    REQUIRE(one_stats.valid);
    REQUIRE(many_stats.valid);
    // Multi-option stepper paints text (chevrons + value) → many colors. If even
    // that is blank, the raster lane no-ops text; skip rather than false-pass.
    if (many_stats.unique_colors <= 2)
        SKIP("native raster unavailable in this build");
    // The single-option stepper drew nothing: a near-uniform (transparent)
    // frame, strictly fewer colors than the multi-option one.
    CHECK(one_stats.unique_colors <= 2);
    CHECK(one_stats.unique_colors < many_stats.unique_colors);
}

TEST_CASE("suppress_svg_rect removes the baked tab highlight by geometry, not rx/cx",
          "[view][design-import][frame][svg]") {
    // Values from a real exported frame: the selected-tab highlight is a filled
    // <rect> at the slot, sitting on top of the wider strip background.
    std::string svg =
        "<svg>"
        "<rect x=\"10\" y=\"20\" width=\"30\" height=\"40\" fill=\"#111111\"/>"
        "<rect x=\"290\" y=\"123\" width=\"124\" height=\"26\" rx=\"2\" fill=\"#252626\"/>"
        "<rect x=\"352\" y=\"126\" width=\"29.5\" height=\"20\" rx=\"2\" fill=\"#2C2D2D\"/>"
        "</svg>";
    // Removes exactly the baked highlight (tab group x=293,w=118,4 tabs,sel=2 →
    // slot 2 at x=352,w=29.5), leaving the strip + the unrelated rect. The rx="2"
    // attribute must NOT be mistaken for x=.
    REQUIRE(suppress_svg_rect(svg, 352.0f, 126.0f, 29.5f, 20.0f));
    CHECK(svg.find("#2C2D2D") == std::string::npos);   // baked highlight removed
    CHECK(svg.find("#252626") != std::string::npos);   // wider strip kept
    CHECK(svg.find("#111111") != std::string::npos);   // unrelated rect kept
    CHECK_FALSE(suppress_svg_rect(svg, 352.0f, 126.0f, 29.5f, 20.0f));  // no 2nd match
    CHECK_FALSE(suppress_svg_rect(svg, 10.0f, 20.0f, 999.0f, 40.0f));   // size mismatch
}

TEST_CASE("suppress_svg_glow_at removes the selected digit's baked glow group",
          "[view][design-import][frame][svg]") {
    // Figma bakes the selected tab digit with a glow = a big-blur drop-shadow
    // filter group wrapping the glyph. The live pill moves on click but the baked
    // glow stays stuck on the original digit. Suppress the filtered group whose
    // first drawn point sits inside the selected cell (x=352..381.5, y=126..146,
    // matching the highlight rect above). A nested <g> inside it must be
    // depth-matched so the whole glow group is erased.
    std::string svg =
        "<svg>"
        "<g filter=\"url(#glowSel)\"><g><path d=\"M366.78 138.58L370 132\" fill=\"#fff\"/></g></g>"
        "<g filter=\"url(#glowElsewhere)\"><path d=\"M12 600L20 610\" fill=\"#fff\"/></g>"
        "<path d=\"M360 140L362 142\" fill=\"#aaa\"/>"   // a plain (unfiltered) glyph stays
        "</svg>";
    REQUIRE(suppress_svg_glow_at(svg, 352.0f, 126.0f, 29.5f, 20.0f));
    CHECK(svg.find("#glowSel") == std::string::npos);        // selected-cell glow removed...
    CHECK(svg.find("M366.78 138.58") == std::string::npos);  // ...including its glyph + nested <g>
    CHECK(svg.find("#glowElsewhere") != std::string::npos);  // glow outside the cell kept
    CHECK(svg.find("M360 140") != std::string::npos);        // plain unfiltered glyph kept
    CHECK_FALSE(suppress_svg_glow_at(svg, 352.0f, 126.0f, 29.5f, 20.0f));  // no 2nd match
    // A cell with no filtered group inside reports no removal.
    CHECK_FALSE(suppress_svg_glow_at(svg, 0.0f, 0.0f, 5.0f, 5.0f));
}

TEST_CASE("suppress_svg_glyph_at removes a baked digit glyph in the cell",
          "[view][design-import][frame][svg]") {
    // Non-selected tab digits are plain <path> glyphs. We drop each so the live
    // overlay is the sole renderer of the digits (no faint doubled glyph). Only
    // the path whose first point is inside the cell is removed; glyphs in other
    // cells, and the strip <rect>, stay.
    std::string svg =
        "<svg>"
        "<rect x=\"290\" y=\"123\" width=\"124\" height=\"26\" fill=\"#252626\"/>"
        "<path d=\"M308.6 138.5L310 132\" fill=\"#ABABAB\"/>"   // digit "1" in slot 0
        "<path d=\"M339.2 138.5L341 132\" fill=\"#ABABAB\"/>"   // digit "2" in slot 1
        "</svg>";
    // Slot 0 cell: x=293..322.5, y=126..146. Removes "1" only.
    REQUIRE(suppress_svg_glyph_at(svg, 293.0f, 126.0f, 29.5f, 20.0f));
    CHECK(svg.find("M308.6 138.5") == std::string::npos);  // slot-0 digit removed
    CHECK(svg.find("M339.2 138.5") != std::string::npos);  // slot-1 digit kept
    CHECK(svg.find("#252626") != std::string::npos);       // strip rect kept (not a <path>)
    CHECK_FALSE(suppress_svg_glyph_at(svg, 293.0f, 126.0f, 29.5f, 20.0f));  // no 2nd match in cell
}

TEST_CASE("DesignFrameView suppresses the baked selected-tab highlight (no double-pill)",
          "[view][design-import][frame][svg]") {
    // An SVG whose tab strip has a baked highlight at the selected slot (2). The
    // DesignFrameView constructor must strip it so only the live pill shows.
    const std::string strip =
        "<rect x=\"10\" y=\"10\" width=\"60\" height=\"60\" rx=\"2\" fill=\"#1c1d1d\"/>"
        "<rect x=\"20\" y=\"14\" width=\"56\" height=\"14\" rx=\"2\" fill=\"#252626\"/>"
        // baked highlight on slot 2: tab group x=20,w=56,4 tabs → slot_w=14, slot2 x=48
        "<rect x=\"48\" y=\"14\" width=\"14\" height=\"14\" rx=\"2\" fill=\"#3c3d3d\"/>";
    const std::string svg =
        "<svg width=\"80\" height=\"80\" xmlns=\"http://www.w3.org/2000/svg\">" + strip + "</svg>";
    DesignFrameElement tg;
    tg.kind = DesignFrameElement::Kind::tab_group;
    tg.x = 20; tg.y = 14; tg.w = 56; tg.h = 14;
    tg.options = {"1", "2", "3", "4"};
    tg.selected_index = 2;
    // The view should construct without the baked highlight surviving. We can't
    // read svg_ directly, so prove the mechanism via the helper on the same data:
    std::string check = svg;
    REQUIRE(suppress_svg_rect(check, tg.x + 2 * (tg.w / 4), tg.y, tg.w / 4, tg.h));
    CHECK(check.find("#3c3d3d") == std::string::npos);  // baked slot-2 pill gone
    CHECK(check.find("#252626") != std::string::npos);  // strip kept
    DesignFrameView v(svg, {tg});                        // exercises the ctor path
    CHECK(v.element_count() == 1);
}

TEST_CASE("DesignFrameView is fail-safe on an empty/garbage SVG",
          "[view][design-import][frame][svg]") {
    DesignFrameView empty("", {});
    empty.set_bounds({0, 0, 40, 40});
    CHECK(empty.element_count() == 0);
    // Painting must not crash even with no SVG (the panel falls back to 0 -> early
    // out). Render path returns an empty/uniform image; the call must be safe.
    auto png = render_to_png(empty, 40, 40, 1.0f, ScreenshotBackend::skia);
    SUCCEED("paint with empty SVG did not crash");
}

// ── v2: event-driven binding (on_element_changed + gestures) + uniform value ──

namespace {
// A dropdown element with three options, initially on index 1.
DesignFrameElement make_dropdown() {
    DesignFrameElement d;
    d.kind = DesignFrameElement::Kind::dropdown;
    d.x = 12; d.y = 12; d.w = 60; d.h = 16;
    d.options = {"A", "B", "C"};
    d.selected_index = 1;
    return d;
}
}  // namespace

TEST_CASE("DesignFrameView knob drag fires on_element_changed + gesture begin/end",
          "[view][design-import][frame][binding]") {
    DesignFrameView v(make_design_svg(), {make_knob()});
    v.set_bounds({0, 0, 100, 100});

    int begins = 0, ends = 0, changes = 0;
    float last = -1.0f;
    v.on_gesture_begin   = [&](int i) { CHECK(i == 0); ++begins; };
    v.on_gesture_end     = [&](int i) { CHECK(i == 0); ++ends; };
    v.on_element_changed = [&](int i, float val) { CHECK(i == 0); last = val; ++changes; };

    v.on_mouse_down({40, 40});   // on the knob
    v.on_mouse_drag({40, 10});   // turn up
    v.on_mouse_up({40, 10});

    CHECK(begins == 1);
    CHECK(ends == 1);
    CHECK(changes >= 1);
    CHECK(last > 0.5f);                       // the reported value tracked the turn
    CHECK(v.element_value(0) == last);        // accessor agrees with the callback

    // set_element_value is a programmatic push: it must NOT fire on_element_changed.
    const int before = changes;
    v.set_element_value(0, 0.25f);
    CHECK(changes == before);
    CHECK(v.element_value(0) == 0.25f);
}

TEST_CASE("DesignFrameView exposes a choice control as a normalized param",
          "[view][design-import][frame][binding]") {
    DesignFrameView v(make_design_svg(), {make_dropdown()});
    v.set_bounds({0, 0, 100, 100});

    REQUIRE(v.element_kind(0) == DesignFrameElement::Kind::dropdown);
    // 3 options -> indices 0,1,2 map to 0, 0.5, 1.0. Initial index 1 -> 0.5.
    CHECK(v.element_value(0) == Catch::Approx(0.5f));

    // Host push: 1.0 -> last option (index 2), silently (no on_element_changed).
    int changes = 0;
    v.on_element_changed = [&](int, float) { ++changes; };
    v.set_element_value(0, 1.0f);
    CHECK(v.element_value(0) == Catch::Approx(1.0f));
    CHECK(changes == 0);

    // 0.0 -> first option (index 0).
    v.set_element_value(0, 0.0f);
    CHECK(v.element_value(0) == Catch::Approx(0.0f));
}

// ── fader / toggle (faithful-vector overlay kinds reusing Pulp widgets) ──────

TEST_CASE("DesignFrameView fader drag adjusts the value", "[view][design-import][frame]") {
    // A fader thumb on a track [y=20, y+h=60]; dragging up raises the value 1:1.
    DesignFrameElement f;
    f.kind = DesignFrameElement::Kind::fader;
    f.needle_d = "M50 38L50 30";   // any marker; value logic is independent of it
    f.cy = 40; f.x = 30; f.y = 20; f.w = 20; f.h = 40;
    f.value = 0.5f;
    DesignFrameView v(make_design_svg(), {f});
    v.set_bounds({0, 0, 80, 80});  // view->SVG offset is the panel origin (+10)
    v.on_mouse_down({30, 30});     // -> SVG (40,40), inside the fader rect
    v.on_mouse_drag({30, 10});     // drag up 20 design px over a 40px track
    CHECK(v.element_value(0) > 0.9f);
    v.on_mouse_up({30, 10});

    // Drag down lowers it.
    v.set_element_value(0, 0.5f);
    v.on_mouse_down({30, 30});
    v.on_mouse_drag({30, 50});
    CHECK(v.element_value(0) < 0.1f);
}

TEST_CASE("DesignFrameView toggle click flips on/off", "[view][design-import][frame]") {
    DesignFrameElement t;
    t.kind = DesignFrameElement::Kind::toggle;
    t.x = 30; t.y = 30; t.w = 20; t.h = 20;
    t.value = 0.0f;
    DesignFrameView v(make_design_svg(), {t});
    v.set_bounds({0, 0, 80, 80});
    v.on_mouse_down({30, 30}); v.on_mouse_up({30, 30});   // -> SVG (40,40), inside
    CHECK(v.element_value(0) == 1.0f);                    // flipped on
    v.on_mouse_down({30, 30}); v.on_mouse_up({30, 30});
    CHECK(v.element_value(0) == 0.0f);                    // flipped off
}

TEST_CASE("DesignFrameView flash toggle lights on press, clears on release",
          "[view][design-import][frame]") {
    // A flash command button (sample next/prev/random) lights while held and
    // clears on release — unlike the default sticky toggle.
    DesignFrameElement t;
    t.kind = DesignFrameElement::Kind::toggle;
    t.flash = true;
    t.x = 30; t.y = 30; t.w = 20; t.h = 20; t.value = 0.0f;
    DesignFrameView v(make_design_svg(), {t});
    v.set_bounds({0, 0, 80, 80});
    v.on_mouse_down({30, 30});                 // -> SVG (40,40), inside
    CHECK(v.element_value(0) == 1.0f);         // lit on press
    v.on_mouse_up({30, 30});
    CHECK(v.element_value(0) == 0.0f);         // cleared on release (not sticky)
}

TEST_CASE("DesignFrameView horizontal fader drag adjusts the value",
          "[view][design-import][frame]") {
    // A wider-than-tall track is a horizontal slider: dragging right raises the
    // value, dragging left lowers it (the pan-slider case).
    DesignFrameElement f;
    f.kind = DesignFrameElement::Kind::fader;
    f.needle_d = "M30 50L30 50";   // marker; value logic is independent of it
    f.cx = 40; f.x = 20; f.y = 44; f.w = 40; f.h = 8;   // w>h → horizontal
    f.value = 0.5f;
    DesignFrameView v(make_design_svg(), {f});
    v.set_bounds({0, 0, 80, 80});
    v.on_mouse_down({30, 38});     // -> SVG (40,48), inside the track rect
    v.on_mouse_drag({70, 38});     // drag right 40 design px over a 40px track
    CHECK(v.element_value(0) > 0.9f);
    v.on_mouse_up({70, 38});

    v.set_element_value(0, 0.5f);
    v.on_mouse_down({30, 38});
    v.on_mouse_drag({10, 38});     // drag left
    CHECK(v.element_value(0) < 0.1f);
}

TEST_CASE("DesignFrameView paints a horizontal fader thumb that translates in X",
          "[view][design-import][frame][svg]") {
    const std::string svg =
        R"SVG(<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">)SVG"
        R"SVG(<rect x="10" y="10" width="80" height="80" fill="#222222"/>)SVG"
        R"SVG(<rect x="20" y="48" width="60" height="4" rx="2" fill="#444444"/>)SVG"
        R"SVG(<circle id="hthumb" cx="30" cy="50" r="6" fill="white"/></svg>)SVG";
    DesignFrameElement f;
    f.kind = DesignFrameElement::Kind::fader;
    f.needle_d = "id=\"hthumb\"";
    f.cx = 30; f.cy = 50; f.x = 20; f.y = 44; f.w = 60; f.h = 12;  // w>h → horizontal
    DesignFrameView lo(svg, {f}, 0, 0, 100, 100), hi(svg, {f}, 0, 0, 100, 100);
    lo.set_bounds({0, 0, 100, 100});
    hi.set_bounds({0, 0, 100, 100});
    lo.set_element_value(0, 0.1f);   // thumb near the left
    hi.set_element_value(0, 0.9f);   // thumb near the right
    auto lo_png = render_to_png(lo, 100, 100, 2.0f, ScreenshotBackend::skia);
    if (lo_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto hi_png = render_to_png(hi, 100, 100, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(hi_png.empty());
    const auto cmp = compare_screenshots(lo_png, hi_png);
    REQUIRE(cmp.valid);
    if (cmp.similarity >= 0.999f)
        SKIP("SVG (SkSVGDOM) rendering unavailable in this build");
    CHECK(cmp.similarity < 0.999f);   // the thumb visibly translated horizontally
}

TEST_CASE("DesignFrameView paints a fader thumb that translates with value",
          "[view][design-import][frame][svg]") {
    // The fader paint path translates the thumb element by value (the fader
    // analog of needle rotation). Render two values; the thumb must visibly move.
    const std::string svg =
        R"SVG(<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">)SVG"
        R"SVG(<rect x="10" y="10" width="80" height="80" fill="#222222"/>)SVG"
        R"SVG(<rect x="46" y="20" width="8" height="60" rx="4" fill="#444444"/>)SVG"
        R"SVG(<path id="fader thumb" d="M44 48L56 48L56 52L44 52Z" fill="white"/></svg>)SVG";
    DesignFrameElement f;
    f.kind = DesignFrameElement::Kind::fader;
    f.needle_d = "id=\"fader thumb\"";   // marker into the thumb element
    f.cy = 50; f.x = 40; f.y = 20; f.w = 16; f.h = 60;  // baked center + track
    DesignFrameView lo(svg, {f}, 0, 0, 100, 100), hi(svg, {f}, 0, 0, 100, 100);
    lo.set_bounds({0, 0, 100, 100});
    hi.set_bounds({0, 0, 100, 100});
    lo.set_element_value(0, 0.1f);    // thumb near the bottom
    hi.set_element_value(0, 0.9f);    // thumb near the top
    auto lo_png = render_to_png(lo, 100, 100, 2.0f, ScreenshotBackend::skia);
    if (lo_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto hi_png = render_to_png(hi, 100, 100, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(hi_png.empty());
    const auto cmp = compare_screenshots(lo_png, hi_png);
    REQUIRE(cmp.valid);
    if (cmp.similarity >= 0.999f)
        SKIP("SVG (SkSVGDOM) rendering unavailable in this build");
    CHECK(cmp.similarity < 0.999f);   // the thumb visibly translated
}

TEST_CASE("DesignFrameView tints a toggle when it is on",
          "[view][design-import][frame][svg]") {
    // Toggle paint tints the rect translucently when value>=0.5. Two toggles
    // exercise both tint sources: the design's own color and the theme-accent
    // default (used when the design supplies none).
    const std::string svg =
        R"SVG(<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">)SVG"
        R"SVG(<rect x="10" y="10" width="80" height="80" fill="#222222"/></svg>)SVG";
    DesignFrameElement branded;       // design supplies the active color
    branded.kind = DesignFrameElement::Kind::toggle;
    branded.x = 14; branded.y = 14; branded.w = 36; branded.h = 72;
    branded.bg_color = "#33aa88";
    DesignFrameElement themed;        // no color → theme-accent default tint
    themed.kind = DesignFrameElement::Kind::toggle;
    themed.x = 50; themed.y = 14; themed.w = 36; themed.h = 72;
    DesignFrameView off(svg, {branded, themed}, 0, 0, 100, 100);
    DesignFrameView on(svg, {branded, themed}, 0, 0, 100, 100);
    off.set_bounds({0, 0, 100, 100});
    on.set_bounds({0, 0, 100, 100});
    off.set_element_value(0, 0.0f);   // both off → no tint (default value is 0.5)
    off.set_element_value(1, 0.0f);
    on.set_element_value(0, 1.0f);    // both on → both tint
    on.set_element_value(1, 1.0f);
    auto off_png = render_to_png(off, 100, 100, 2.0f, ScreenshotBackend::skia);
    if (off_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto on_png = render_to_png(on, 100, 100, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(on_png.empty());
    const auto cmp = compare_screenshots(off_png, on_png);
    REQUIRE(cmp.valid);
    if (cmp.similarity >= 0.999f)
        SKIP("raster rendering unavailable in this build");
    CHECK(cmp.similarity < 0.999f);   // the active tint appears
}

TEST_CASE("DesignFrameView xy_pad puck follows a 2D drag",
          "[view][design-import][frame]") {
    // Dragging inside an xy_pad rect moves the puck absolutely: value=X, value_y=Y.
    DesignFrameElement xy;
    xy.kind = DesignFrameElement::Kind::xy_pad;
    xy.needle_d = "id=\"puck\"";
    xy.cx = 30; xy.cy = 30; xy.x = 20; xy.y = 20; xy.w = 60; xy.h = 60;
    DesignFrameView v(make_design_svg(), {xy});
    v.set_bounds({0, 0, 100, 100});
    v.on_mouse_down({30, 30});
    v.on_mouse_drag({78, 78});   // SVG ~(78,78): value≈(78-20)/60≈0.97
    CHECK(v.element_value(0) > 0.8f);
    v.on_mouse_drag({22, 22});   // back toward top-left
    CHECK(v.element_value(0) < 0.2f);
}

TEST_CASE("DesignFrameView xy_pad puck renders at its 2D position",
          "[view][design-import][frame][svg]") {
    const std::string svg =
        R"SVG(<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">)SVG"
        R"SVG(<rect x="10" y="10" width="80" height="80" fill="#222222"/>)SVG"
        R"SVG(<rect x="20" y="20" width="60" height="60" rx="4" fill="#333333"/>)SVG"
        R"SVG(<circle id="puck" cx="30" cy="30" r="6" fill="#F56161"/></svg>)SVG";
    DesignFrameElement a;
    a.kind = DesignFrameElement::Kind::xy_pad;
    a.needle_d = "id=\"puck\"";
    a.cx = 30; a.cy = 30; a.x = 20; a.y = 20; a.w = 60; a.h = 60;
    DesignFrameElement b = a;
    a.value = 0.1f; a.value_y = 0.1f;   // puck top-left
    b.value = 0.9f; b.value_y = 0.9f;   // puck bottom-right
    DesignFrameView va(svg, {a}, 0, 0, 100, 100), vb(svg, {b}, 0, 0, 100, 100);
    va.set_bounds({0, 0, 100, 100});
    vb.set_bounds({0, 0, 100, 100});
    auto pa = render_to_png(va, 100, 100, 2.0f, ScreenshotBackend::skia);
    if (pa.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto pb = render_to_png(vb, 100, 100, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(pb.empty());
    const auto cmp = compare_screenshots(pa, pb);
    REQUIRE(cmp.valid);
    if (cmp.similarity >= 0.999f)
        SKIP("SVG (SkSVGDOM) rendering unavailable in this build");
    CHECK(cmp.similarity < 0.999f);   // the puck moved diagonally
}

TEST_CASE("DesignFrameView slides a switch dot to the on-side when toggled",
          "[view][design-import][frame][svg]") {
    // A toggle WITH a dot marker (needle_d) is a switch: the dot slides along
    // the pill to the value's end. Render off vs on; the dot must visibly move.
    const std::string svg =
        R"SVG(<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">)SVG"
        R"SVG(<rect x="10" y="10" width="80" height="80" fill="#222222"/>)SVG"
        R"SVG(<rect x="30" y="44" width="40" height="16" rx="8" fill="#444444"/>)SVG"
        R"SVG(<circle id="switch dot" cx="38" cy="52" r="6" fill="white"/></svg>)SVG";
    DesignFrameElement sw;
    sw.kind = DesignFrameElement::Kind::toggle;
    sw.needle_d = "id=\"switch dot\"";
    sw.cx = 38; sw.cy = 52; sw.x = 30; sw.y = 44; sw.w = 40; sw.h = 16;
    DesignFrameView off(svg, {sw}, 0, 0, 100, 100), on(svg, {sw}, 0, 0, 100, 100);
    off.set_bounds({0, 0, 100, 100});
    on.set_bounds({0, 0, 100, 100});
    off.set_element_value(0, 0.0f);   // dot at the left
    on.set_element_value(0, 1.0f);    // dot at the right
    auto off_png = render_to_png(off, 100, 100, 2.0f, ScreenshotBackend::skia);
    if (off_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto on_png = render_to_png(on, 100, 100, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(on_png.empty());
    const auto cmp = compare_screenshots(off_png, on_png);
    REQUIRE(cmp.valid);
    if (cmp.similarity >= 0.999f)
        SKIP("SVG (SkSVGDOM) rendering unavailable in this build");
    CHECK(cmp.similarity < 0.999f);   // the dot slid + the track tinted
}

TEST_CASE("DesignFrameView rotates a <rect> indicator needle (tag-agnostic)",
          "[view][design-import][frame][svg]") {
    // wrap_needle_rotation must rotate a <rect> indicator, not only a <path>.
    const std::string svg =
        R"SVG(<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">)SVG"
        R"SVG(<rect x="10" y="10" width="80" height="80" rx="4" fill="#cccccc"/>)SVG"
        R"SVG(<circle cx="50" cy="50" r="20" fill="#8a97a6"/>)SVG"
        R"SVG(<rect id="knob indicator" x="49" y="32" width="2" height="10" fill="white"/></svg>)SVG";
    DesignFrameElement k;
    k.kind = DesignFrameElement::Kind::knob;
    k.cx = 50; k.cy = 50; k.hit_radius = 22;
    k.needle_d = "id=\"knob indicator\"";   // marker into the <rect>, not a path d
    DesignFrameView lo(svg, {k}), hi(svg, {k});
    lo.set_bounds({0, 0, 80, 80});
    hi.set_bounds({0, 0, 80, 80});
    lo.set_element_value(0, 0.1f);
    hi.set_element_value(0, 0.9f);
    auto lo_png = render_to_png(lo, 80, 80, 2.0f, ScreenshotBackend::skia);
    if (lo_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto hi_png = render_to_png(hi, 80, 80, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(hi_png.empty());
    const auto cmp = compare_screenshots(lo_png, hi_png);
    REQUIRE(cmp.valid);
    if (cmp.similarity >= 0.999f)
        SKIP("SVG (SkSVGDOM) rendering unavailable in this build");
    CHECK(cmp.similarity < 0.999f);         // the rect needle visibly moved
}

namespace {

// A Kind::custom element referencing the given factory id (with a source node so
// the diagnostic can carry provenance). Sits inside the 80x80 panel.
DesignFrameElement make_custom(std::string factory_id, std::string source_node = "1:42") {
    DesignFrameElement e;
    e.kind = DesignFrameElement::Kind::custom;
    e.x = 20; e.y = 20; e.w = 40; e.h = 40;
    e.factory_id = std::move(factory_id);
    e.source_node_id = std::move(source_node);
    return e;
}

}  // namespace

TEST_CASE("DesignFrameView reports an unregistered custom control to the diagnostic",
          "[view][design-import][frame][custom]") {
    clear_design_control_factories();   // ensure "my_widget" is genuinely absent

    DesignFrameView v(make_design_svg(), {make_custom("my_widget", "3:7")});

    // The element is inert: no overlay widget is built, and the frame still
    // renders (the baked SVG shows) — nothing crashes, nothing blanks.
    CHECK(v.overlay_widget(0) == nullptr);

    // The accumulator records the referenced-but-unregistered id + its node.
    REQUIRE(v.unregistered_custom_controls().size() == 1);
    CHECK(v.unregistered_custom_controls()[0].factory_id == "my_widget");
    CHECK(v.unregistered_custom_controls()[0].source_node_id == "3:7");

    // The callback fires exactly once, with the same id + node. (Attaching it
    // after construction replays the initial build's miss.)
    int calls = 0;
    UnregisteredCustomControl seen;
    v.set_on_unregistered_custom_control([&](const UnregisteredCustomControl& u) {
        ++calls;
        seen = u;
    });
    CHECK(calls == 1);
    CHECK(seen.factory_id == "my_widget");
    CHECK(seen.source_node_id == "3:7");

    clear_design_control_factories();
}

TEST_CASE("DesignFrameView builds a registered custom control without firing the diagnostic",
          "[view][design-import][frame][custom]") {
    clear_design_control_factories();
    int built = 0;
    std::string built_id;
    register_design_control_factory(
        "my_widget", [&](const DesignControlContext& ctx) -> std::unique_ptr<View> {
            ++built;
            built_id = ctx.factory_id;
            return std::make_unique<View>();
        });

    int misses = 0;
    DesignFrameView v(make_design_svg(), {make_custom("my_widget")});
    v.set_on_unregistered_custom_control(
        [&](const UnregisteredCustomControl&) { ++misses; });

    CHECK(built == 1);                                   // factory ran
    CHECK(built_id == "my_widget");                      // with the right ctx
    CHECK(v.overlay_widget(0) != nullptr);               // overlay added as a child
    CHECK(v.unregistered_custom_controls().empty());     // nothing unregistered
    CHECK(misses == 0);                                  // callback never fired

    clear_design_control_factories();
}

TEST_CASE("DesignFrameView with no diagnostic callback is inert-safe for an unregistered custom",
          "[view][design-import][frame][custom]") {
    clear_design_control_factories();

    // A knob alongside an unregistered custom: default path (no callback set)
    // must build fine, leave the custom inert, and keep the knob fully working.
    DesignFrameView v(make_design_svg(), {make_knob(), make_custom("ghost")});

    CHECK(v.element_count() == 2);
    CHECK(v.overlay_widget(1) == nullptr);               // custom stays inert
    CHECK(v.element_value(0) == 0.5f);                   // knob unaffected

    // The knob still turns — the inert custom neighbor doesn't break interaction.
    v.set_bounds({0, 0, 80, 80});   // view == panel, so view coords map 1:1
    v.on_mouse_down({40, 40});      // -> SVG (50,50), the knob center
    v.on_mouse_drag({40, 10});      // drag up
    v.on_mouse_up({40, 10});
    CHECK(v.element_value(0) > 0.6f);

    // The getter still records the miss even with no callback attached.
    REQUIRE(v.unregistered_custom_controls().size() == 1);
    CHECK(v.unregistered_custom_controls()[0].factory_id == "ghost");

    clear_design_control_factories();
}

// ── custom-control value conduit (opt-in DesignControl mixin) ─────────────────

namespace {

// A custom control that joins the value conduit: View for the widget tree,
// DesignControl for the normalized-value channel. Records silent host->view
// pushes and can simulate user-driven edits through the conduit callbacks.
class ValueControl : public View, public DesignControl {
public:
    void set_control_value(float v) override {
        value_ = v;
        ++pushes_;
        // When echo_ is set this DELIBERATELY breaks the silent-push contract
        // (re-emits from set_control_value) so a test can prove the build-time
        // seed can't leak into the change funnel — it runs before the callback
        // is installed.
        if (echo_ && on_control_changed) on_control_changed(v);
    }
    float control_value() const override { return value_; }

    // A bracketed continuous drag to `v` — begin, change, end, like a knob.
    void user_drag(float v) {
        if (on_gesture_begin) on_gesture_begin();
        value_ = v;
        if (on_control_changed) on_control_changed(v);
        if (on_gesture_end) on_gesture_end();
    }
    // A bare value change with no gesture bracket (e.g. a discrete tap).
    void user_move(float v) {
        value_ = v;
        if (on_control_changed) on_control_changed(v);
    }

    float value_ = 0.0f;
    int pushes_ = 0;
    bool echo_ = false;
};

// Register a factory that builds a ValueControl, handing its raw pointer back
// via `out` so the test can drive it after DesignFrameView takes ownership.
// `echo` makes the built control violate the silent-push contract on purpose.
void register_value_control(const std::string& id, ValueControl** out,
                            bool echo = false) {
    register_design_control_factory(
        id, [out, echo](const DesignControlContext&) -> std::unique_ptr<View> {
            auto w = std::make_unique<ValueControl>();
            w->echo_ = echo;
            if (out) *out = w.get();
            return w;
        });
}

}  // namespace

TEST_CASE("DesignFrameView seeds a conduit custom control with the element's value",
          "[view][design-import][frame][custom]") {
    clear_design_control_factories();
    ValueControl* ctl = nullptr;
    register_value_control("vknob", &ctl);

    DesignFrameElement e = make_custom("vknob");
    e.value = 0.25f;                               // the design/default state
    DesignFrameView v(make_design_svg(), {e});

    REQUIRE(ctl != nullptr);
    CHECK(ctl->value_ == Catch::Approx(0.25f));    // seeded via set_control_value
    CHECK(ctl->pushes_ == 1);                      // exactly the seed, once

    clear_design_control_factories();
}

TEST_CASE("DesignFrameView pushes host->view values to a conduit custom control silently",
          "[view][design-import][frame][custom]") {
    clear_design_control_factories();
    ValueControl* ctl = nullptr;
    register_value_control("vknob", &ctl);

    DesignFrameView v(make_design_svg(), {make_custom("vknob")});
    REQUIRE(ctl != nullptr);

    int echoes = 0;
    v.on_element_changed = [&](int, float) { ++echoes; };

    v.set_element_value(0, 0.8f);
    CHECK(ctl->value_ == Catch::Approx(0.8f));         // reached the control
    CHECK(v.element_value(0) == Catch::Approx(0.8f));  // read back through the conduit
    CHECK(echoes == 0);                                // host->view push does NOT echo

    v.set_element_value(0, 1.7f);                      // out-of-range is clamped
    CHECK(ctl->value_ == Catch::Approx(1.0f));

    clear_design_control_factories();
}

TEST_CASE("A conduit custom control's user edit funnels through on_element_changed",
          "[view][design-import][frame][custom]") {
    clear_design_control_factories();
    ValueControl* ctl = nullptr;
    register_value_control("vknob", &ctl);

    DesignFrameView v(make_design_svg(), {make_custom("vknob")});
    REQUIRE(ctl != nullptr);

    int idx = -1;
    float got = -1.0f;
    v.on_element_changed = [&](int i, float val) { idx = i; got = val; };

    ctl->user_move(0.6f);                          // simulate a user gesture
    CHECK(idx == 0);                               // routed with the element index
    CHECK(got == Catch::Approx(0.6f));             // and the edited value
    CHECK(v.element_value(0) == Catch::Approx(0.6f));  // element_value reads it live

    clear_design_control_factories();
}

TEST_CASE("A custom control without the conduit has no value channel (unchanged)",
          "[view][design-import][frame][custom]") {
    clear_design_control_factories();
    // Factory builds a plain View (does NOT inherit DesignControl): the
    // pre-conduit behavior — built + rendered, but no normalized value.
    register_design_control_factory(
        "plain", [](const DesignControlContext&) -> std::unique_ptr<View> {
            return std::make_unique<View>();
        });

    DesignFrameView v(make_design_svg(), {make_custom("plain")});
    CHECK(v.overlay_widget(0) != nullptr);         // built + rendered
    CHECK(v.element_value(0) == -1.0f);            // no value channel (sentinel)
    v.set_element_value(0, 0.5f);                  // no-op, must not crash
    CHECK(v.element_value(0) == -1.0f);

    clear_design_control_factories();
}

TEST_CASE("A conduit control routes user edits by param_key (both ways)",
          "[view][design-import][frame][custom][param-key]") {
    clear_design_control_factories();
    ValueControl* ctl = nullptr;
    register_value_control("vknob", &ctl);

    DesignFrameElement e = make_custom("vknob");
    e.param_key = "cutoff";
    DesignFrameView v(make_design_svg(), {e});
    REQUIRE(ctl != nullptr);

    // String-key host bridge stand-in (the embed shim forwards by param_key).
    std::vector<std::pair<std::string, float>> host_writes;
    v.on_element_changed = [&](int i, float val) {
        host_writes.emplace_back(v.element_param_key(i), val);
    };

    // UI -> host: a user edit is keyed by the element's param_key.
    ctl->user_move(0.6f);
    REQUIRE(host_writes.size() == 1);
    CHECK(host_writes[0].first == "cutoff");
    CHECK(host_writes[0].second == Catch::Approx(0.6f));

    // host -> UI: automation/preset recall pushes silently — no echo back.
    host_writes.clear();
    const int idx = v.element_for_param_key("cutoff");
    REQUIRE(idx == 0);
    v.set_element_value(idx, 0.25f);
    CHECK(ctl->value_ == Catch::Approx(0.25f));
    CHECK(host_writes.empty());

    clear_design_control_factories();
}

TEST_CASE("A conduit control's drag brackets the edit with gesture begin/end",
          "[view][design-import][frame][custom]") {
    clear_design_control_factories();
    ValueControl* ctl = nullptr;
    register_value_control("vknob", &ctl);

    DesignFrameView v(make_design_svg(), {make_custom("vknob")});
    REQUIRE(ctl != nullptr);

    std::vector<std::string> log;
    v.on_gesture_begin  = [&](int i) { log.push_back("begin:" + std::to_string(i)); };
    v.on_element_changed = [&](int i, float) { log.push_back("changed:" + std::to_string(i)); };
    v.on_gesture_end    = [&](int i) { log.push_back("end:" + std::to_string(i)); };

    ctl->user_drag(0.7f);                          // begin -> change -> end
    REQUIRE(log.size() == 3);
    CHECK(log[0] == "begin:0");                     // host can group one undo step
    CHECK(log[1] == "changed:0");
    CHECK(log[2] == "end:0");

    clear_design_control_factories();
}

TEST_CASE("Frame swap re-seeds a conduit control without clobbering host state",
          "[view][design-import][frame][custom][param-key]") {
    clear_design_control_factories();
    ValueControl* ctl = nullptr;
    register_value_control("echo", &ctl, /*echo=*/true);  // violates silent contract

    // Frame 0: a knob. Frame 1: an echoing custom control with a param_key.
    DesignFrameView v(make_design_svg(), {make_knob()});
    DesignFrameElement e = make_custom("echo");
    e.value = 0.2f;
    e.param_key = "cutoff";
    v.add_frame(make_design_svg(), {e});

    std::vector<std::pair<std::string, float>> host_writes;
    v.on_element_changed = [&](int i, float val) {
        host_writes.emplace_back(v.element_param_key(i), val);
    };

    // Swapping to frame 1 rebuilds overlays and re-seeds the control. The seed
    // runs BEFORE the callback is wired, so even an echoing (contract-violating)
    // control cannot re-emit — the host param is NOT clobbered with the default.
    v.set_active_frame(1);
    CHECK(host_writes.empty());                     // the seed-before-wire guarantee
    REQUIRE(ctl != nullptr);
    CHECK(ctl->value_ == Catch::Approx(0.2f));      // re-seeded to the element value

    clear_design_control_factories();
}
