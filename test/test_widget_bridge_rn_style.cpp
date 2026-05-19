// test_widget_bridge_rn_style.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// pulp #1026 — React Native style-prop bridge primitives.
//
// The 4 RN-shaped style functions that flow through the bridge into
// View's geometry / shadow / transform / opacity slots:
//
//   * setShadow(id, color, offsetX, offsetY, opacity, radius) — RN
//     composes shadowOpacity into the alpha channel of shadowColor.
//   * setOpacity(id, alpha) — direct slot write.
//   * setTransform(id, scale, rotate, translateX, translateY) — RN
//     composes individual transform props into a single uniform
//     setTransform call.
//   * Other RN style-prop entries (visibility, font* family, etc.)
//     that thread through View's slots.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <thread>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

// ── pulp #1026: React Native style-prop bridge primitives ──────────────────

TEST_CASE("WidgetBridge setShadow lowers RN-shaped args onto setBoxShadow",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 0, 0, 48, 48)");
    // setShadow(id, color, offsetX, offsetY, opacity, radius). RN composes
    // shadowOpacity into the alpha channel of shadowColor; with #000000ff
    // and opacity 0.5 the resulting alpha is ~127/255.
    bridge.load_script("setShadow('gain', '#000000ff', 4, 8, 0.5, 12)");

    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->has_box_shadow());
    const auto& s = w->box_shadow();
    REQUIRE_THAT(s.offset_x, WithinAbs(4.0f, 1e-4f));
    REQUIRE_THAT(s.offset_y, WithinAbs(8.0f, 1e-4f));
    REQUIRE_THAT(s.blur, WithinAbs(12.0f, 1e-4f));
    REQUIRE_THAT(s.spread, WithinAbs(0.0f, 1e-4f));
    // alpha should be approximately 0.5 (1.0 * 0.5).
    REQUIRE_THAT(s.color.a, WithinAbs(0.5f, 1e-3f));
    REQUIRE(s.inset == false);
}

TEST_CASE("WidgetBridge setBackfaceVisibility plumbs the View flag",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    REQUIRE(w->backface_visible());

    bridge.load_script("setBackfaceVisibility('k', 'hidden')");
    REQUIRE_FALSE(w->backface_visible());

    bridge.load_script("setBackfaceVisibility('k', 'visible')");
    REQUIRE(w->backface_visible());
}

TEST_CASE("WidgetBridge setPointerEvents routes 4-valued enum to View",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    REQUIRE(w->pointer_events() == View::PointerEvents::auto_);

    bridge.load_script("setPointerEvents('k', 'none')");
    REQUIRE(w->pointer_events() == View::PointerEvents::none);
    REQUIRE_FALSE(w->hit_testable());

    bridge.load_script("setPointerEvents('k', 'box-only')");
    REQUIRE(w->pointer_events() == View::PointerEvents::box_only);
    REQUIRE(w->hit_testable());

    bridge.load_script("setPointerEvents('k', 'box-none')");
    REQUIRE(w->pointer_events() == View::PointerEvents::box_none);
    REQUIRE(w->hit_testable());

    bridge.load_script("setPointerEvents('k', 'auto')");
    REQUIRE(w->pointer_events() == View::PointerEvents::auto_);
    REQUIRE(w->hit_testable());
}

TEST_CASE("WidgetBridge setTransformOrigin sets normalized origin on View",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    // Default is (0.5, 0.5).
    REQUIRE_THAT(w->transform_origin_x(), WithinAbs(0.5f, 1e-5f));
    REQUIRE_THAT(w->transform_origin_y(), WithinAbs(0.5f, 1e-5f));

    bridge.load_script("setTransformOrigin('k', 0.0, 1.0)");
    REQUIRE_THAT(w->transform_origin_x(), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(w->transform_origin_y(), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("WidgetBridge setBorderColor / setBorderWidth / setBorderRadius granular setters",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->has_border());

    bridge.load_script("setBorderColor('k', '#ff8800')");
    REQUIRE(w->has_border());
    REQUIRE(w->border_color().r8() == 0xff);
    REQUIRE(w->border_color().g8() == 0x88);
    REQUIRE(w->border_color().b8() == 0x00);

    bridge.load_script("setBorderWidth('k', 4.5)");
    REQUIRE_THAT(w->border_width(), WithinAbs(4.5f, 1e-5f));

    bridge.load_script("setBorderRadius('k', 9.0)");
    REQUIRE_THAT(w->corner_radius(), WithinAbs(9.0f, 1e-5f));
}

TEST_CASE("WidgetBridge per-corner setBorder*Radius setters route to corner_radii",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    REQUIRE_FALSE(w->has_corner_radii());

    bridge.load_script("setBorderTopLeftRadius('k', 1.0)");
    bridge.load_script("setBorderTopRightRadius('k', 2.0)");
    bridge.load_script("setBorderBottomLeftRadius('k', 3.0)");
    bridge.load_script("setBorderBottomRightRadius('k', 4.0)");

    REQUIRE(w->has_corner_radii());
    REQUIRE_THAT(w->corner_radius_tl(), WithinAbs(1.0f, 1e-5f));
    REQUIRE_THAT(w->corner_radius_tr(), WithinAbs(2.0f, 1e-5f));
    REQUIRE_THAT(w->corner_radius_bl(), WithinAbs(3.0f, 1e-5f));
    REQUIRE_THAT(w->corner_radius_br(), WithinAbs(4.0f, 1e-5f));
}

TEST_CASE("WidgetBridge per-side setBorder*Color / Width route to BorderSide",
          "[view][bridge][issue-1026]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);

    // Each setter sets its own per-side state without corrupting other
    // sides. We assert each side's color and width round-trip.
    bridge.load_script("setBorderTopColor('k', '#11ff00')");
    bridge.load_script("setBorderTopWidth('k', 2.0)");
    bridge.load_script("setBorderRightColor('k', '#0011ff')");
    bridge.load_script("setBorderRightWidth('k', 3.0)");
    bridge.load_script("setBorderBottomColor('k', '#ff0011')");
    bridge.load_script("setBorderBottomWidth('k', 4.0)");
    bridge.load_script("setBorderLeftColor('k', '#fefefe')");
    bridge.load_script("setBorderLeftWidth('k', 5.0)");

    REQUIRE(w->has_border_sides());
    REQUIRE(w->border_top_color().r8() == 0x11);
    REQUIRE(w->border_top_color().g8() == 0xff);
    REQUIRE(w->border_top_color().b8() == 0x00);
    REQUIRE_THAT(w->border_top_width(), WithinAbs(2.0f, 1e-5f));

    REQUIRE(w->border_right_color().r8() == 0x00);
    REQUIRE(w->border_right_color().g8() == 0x11);
    REQUIRE(w->border_right_color().b8() == 0xff);
    REQUIRE_THAT(w->border_right_width(), WithinAbs(3.0f, 1e-5f));

    REQUIRE(w->border_bottom_color().r8() == 0xff);
    REQUIRE(w->border_bottom_color().g8() == 0x00);
    REQUIRE(w->border_bottom_color().b8() == 0x11);
    REQUIRE_THAT(w->border_bottom_width(), WithinAbs(4.0f, 1e-5f));

    REQUIRE(w->border_left_color().r8() == 0xfe);
    REQUIRE_THAT(w->border_left_width(), WithinAbs(5.0f, 1e-5f));

    // Now change ONLY the top color again — top width should be preserved.
    bridge.load_script("setBorderTopColor('k', '#aabbcc')");
    REQUIRE(w->border_top_color().r8() == 0xaa);
    REQUIRE_THAT(w->border_top_width(), WithinAbs(2.0f, 1e-5f));

    // And change ONLY the bottom width — bottom color should be preserved.
    bridge.load_script("setBorderBottomWidth('k', 7.0)");
    REQUIRE_THAT(w->border_bottom_width(), WithinAbs(7.0f, 1e-5f));
    REQUIRE(w->border_bottom_color().r8() == 0xff);
}

// pulp #1027 (audit PR #1166 finding #4) — Interleaved single-attribute
// border setters MUST preserve siblings. Audit found that the JS shim's
// `el.style.borderRadius='8px'; el.style.borderColor='red'` sequence
// silently dropped radius back to 0, because both lowered to
// setBorder(id, color, width, radius) with 0 for unset args. After the
// fix, the JS shim routes through setBorderColor / setBorderWidth /
// setBorderRadius which mutate exactly one slot.
TEST_CASE("WidgetBridge interleaved setBorderColor/Width/Radius preserves siblings",
          "[view][bridge][issue-1027][issue-1166]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);

    SECTION("set width+color via setBorder, then change only color via setBorderColor") {
        bridge.load_script("setBorder('k', '#112233', 3.0, 7.0)");
        REQUIRE(w->has_border());
        REQUIRE_THAT(w->border_width(), WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(w->corner_radius(), WithinAbs(7.0f, 1e-5f));

        bridge.load_script("setBorderColor('k', '#aabbcc')");
        REQUIRE(w->border_color().r8() == 0xaa);
        REQUIRE(w->border_color().g8() == 0xbb);
        REQUIRE(w->border_color().b8() == 0xcc);
        // Width and radius must NOT have been clobbered.
        REQUIRE_THAT(w->border_width(), WithinAbs(3.0f, 1e-5f));
        REQUIRE_THAT(w->corner_radius(), WithinAbs(7.0f, 1e-5f));
    }

    SECTION("set width+color via setBorder, then change only radius via setBorderRadius") {
        bridge.load_script("setBorder('k', '#112233', 4.0, 5.0)");
        bridge.load_script("setBorderRadius('k', 11.0)");
        REQUIRE_THAT(w->corner_radius(), WithinAbs(11.0f, 1e-5f));
        // Color and width must NOT have been clobbered.
        REQUIRE(w->border_color().r8() == 0x11);
        REQUIRE(w->border_color().g8() == 0x22);
        REQUIRE(w->border_color().b8() == 0x33);
        REQUIRE_THAT(w->border_width(), WithinAbs(4.0f, 1e-5f));
    }

    SECTION("audit failing case: set radius first, then color via setBorderColor") {
        // This is the exact case the audit called out: setting radius then
        // color used to leave width=1, radius=0 in the broken JS shim.
        // With the bridge setters routed correctly, radius survives.
        bridge.load_script("setBorderRadius('k', 8.0)");
        REQUIRE_THAT(w->corner_radius(), WithinAbs(8.0f, 1e-5f));

        bridge.load_script("setBorderColor('k', '#ff0000')");
        REQUIRE(w->border_color().r8() == 0xff);
        // Radius MUST still be 8, not 0.
        REQUIRE_THAT(w->corner_radius(), WithinAbs(8.0f, 1e-5f));
    }

    SECTION("set radius first, then width via setBorderWidth") {
        bridge.load_script("setBorderRadius('k', 6.0)");
        bridge.load_script("setBorderWidth('k', 2.5)");
        REQUIRE_THAT(w->border_width(), WithinAbs(2.5f, 1e-5f));
        // Radius MUST still be 6, not 0.
        REQUIRE_THAT(w->corner_radius(), WithinAbs(6.0f, 1e-5f));
    }

    SECTION("set color first, then width via setBorderWidth") {
        bridge.load_script("setBorderColor('k', '#00ff00')");
        bridge.load_script("setBorderWidth('k', 4.0)");
        REQUIRE_THAT(w->border_width(), WithinAbs(4.0f, 1e-5f));
        // Color must NOT have been clobbered to default.
        REQUIRE(w->border_color().g8() == 0xff);
        REQUIRE(w->border_color().r8() == 0x00);
    }

    SECTION("per-side variants stay independent under interleaved updates") {
        bridge.load_script("setBorderTopColor('k', '#101010'); setBorderTopWidth('k', 1.0)");
        bridge.load_script("setBorderRightColor('k', '#202020'); setBorderRightWidth('k', 2.0)");
        bridge.load_script("setBorderBottomColor('k', '#303030'); setBorderBottomWidth('k', 3.0)");
        bridge.load_script("setBorderLeftColor('k', '#404040'); setBorderLeftWidth('k', 4.0)");

        // Now change ONLY top color — every other side must be untouched.
        bridge.load_script("setBorderTopColor('k', '#ffffff')");
        REQUIRE(w->border_top_color().r8() == 0xff);
        REQUIRE_THAT(w->border_top_width(), WithinAbs(1.0f, 1e-5f));
        REQUIRE(w->border_right_color().r8() == 0x20);
        REQUIRE_THAT(w->border_right_width(), WithinAbs(2.0f, 1e-5f));
        REQUIRE(w->border_bottom_color().r8() == 0x30);
        REQUIRE_THAT(w->border_bottom_width(), WithinAbs(3.0f, 1e-5f));
        REQUIRE(w->border_left_color().r8() == 0x40);
        REQUIRE_THAT(w->border_left_width(), WithinAbs(4.0f, 1e-5f));
    }
}

// pulp #1027 — JS shim regression test: el.style.borderColor must NOT
// clobber el.style.borderRadius. This walks the actual web-compat-style-decl.js
// path (CSSStyleDeclaration._applyProperty) so we'd catch any future
// regression where the shim re-routes to setBorder(id, c, w, r).
TEST_CASE("CSS shim: setting borderRadius then borderColor preserves radius",
          "[view][bridge][web-compat][issue-1027][issue-1166]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);

    // Build a minimal CSSStyleDeclaration around the widget. Use the same
    // `_el._id` / `_el._nativeCreated` shape the prelude expects.
    bridge.load_script(
        "var __el = { _id: 'k', _nativeCreated: true };"
        "var __sd = new CSSStyleDeclaration(__el);"
        "__sd._applyProperty('borderRadius', '8px');"
        "__sd._applyProperty('borderColor', '#ff0000');"
    );
    REQUIRE(w->border_color().r8() == 0xff);
    REQUIRE(w->border_color().g8() == 0x00);
    // The audit's failing case — radius MUST survive the borderColor write.
    REQUIRE_THAT(w->corner_radius(), WithinAbs(8.0f, 1e-5f));

    // Reverse order: borderColor first, then borderRadius. Both must stick.
    bridge.load_script(
        "var __el2 = { _id: 'k', _nativeCreated: true };"
        "var __sd2 = new CSSStyleDeclaration(__el2);"
        "__sd2._applyProperty('borderColor', '#00ff00');"
        "__sd2._applyProperty('borderRadius', '12px');"
    );
    REQUIRE(w->border_color().g8() == 0xff);
    REQUIRE_THAT(w->corner_radius(), WithinAbs(12.0f, 1e-5f));

    // Setting borderWidth alone must not zero color or radius.
    bridge.load_script(
        "var __el3 = { _id: 'k', _nativeCreated: true };"
        "var __sd3 = new CSSStyleDeclaration(__el3);"
        "__sd3._applyProperty('borderWidth', '3px');"
    );
    REQUIRE_THAT(w->border_width(), WithinAbs(3.0f, 1e-5f));
    REQUIRE(w->border_color().g8() == 0xff); // preserved from previous
    REQUIRE_THAT(w->corner_radius(), WithinAbs(12.0f, 1e-5f)); // preserved
}

// pulp #1027 — Codex P1 review on PR #1166 follow-up: CSS per-side flat
// props must NOT clobber the unrelated attribute. Before the fix, the
// JS shim lowered `borderTopWidth: '2px'` to `setBorderSide(id, 'top', 2, "")`
// which reset the side's color, and `borderTopColor: 'red'` to
// `setBorderSide(id, 'top', 0, 'red')` which reset the side's width.
TEST_CASE("CSS shim: per-side flat props preserve unset attribute",
          "[view][bridge][web-compat][issue-1027][issue-1166]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);

    // Seed top side with both color and width, then mutate via the JS shim
    // one attribute at a time and verify the other side-attribute survives.
    bridge.load_script(
        "var __el = { _id: 'k', _nativeCreated: true };"
        "var __sd = new CSSStyleDeclaration(__el);"
        "__sd._applyProperty('borderTop', '2px solid #112233');"
    );
    REQUIRE_THAT(w->border_top_width(), WithinAbs(2.0f, 1e-5f));
    REQUIRE(w->border_top_color().r8() == 0x11);

    // Set borderTopColor only — width must survive.
    bridge.load_script("__sd._applyProperty('borderTopColor', '#ffaa00')");
    REQUIRE(w->border_top_color().r8() == 0xff);
    REQUIRE(w->border_top_color().g8() == 0xaa);
    REQUIRE_THAT(w->border_top_width(), WithinAbs(2.0f, 1e-5f));

    // Set borderTopWidth only — color must survive.
    bridge.load_script("__sd._applyProperty('borderTopWidth', '5px')");
    REQUIRE_THAT(w->border_top_width(), WithinAbs(5.0f, 1e-5f));
    REQUIRE(w->border_top_color().r8() == 0xff);
    REQUIRE(w->border_top_color().g8() == 0xaa);

    // Reverse order — width first, then color, on the bottom side this time.
    bridge.load_script(
        "__sd._applyProperty('borderBottomWidth', '3px');"
        "__sd._applyProperty('borderBottomColor', '#00ddee');"
    );
    REQUIRE_THAT(w->border_bottom_width(), WithinAbs(3.0f, 1e-5f));
    REQUIRE(w->border_bottom_color().r8() == 0x00);
    REQUIRE(w->border_bottom_color().g8() == 0xdd);
    REQUIRE(w->border_bottom_color().b8() == 0xee);
}

// pulp #1027 — `border:` CSS shorthand must preserve a previously-set
// border-radius (CSS L3 spec: shorthand sets only width/style/color).
TEST_CASE("CSS shim: 'border:' shorthand preserves border-radius",
          "[view][bridge][web-compat][issue-1027][issue-1166]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);

    bridge.load_script(
        "var __el = { _id: 'k', _nativeCreated: true };"
        "var __sd = new CSSStyleDeclaration(__el);"
        "__sd._applyProperty('borderRadius', '10px');"
        "__sd._applyProperty('border', '2px solid #336699');"
    );
    REQUIRE_THAT(w->border_width(), WithinAbs(2.0f, 1e-5f));
    REQUIRE(w->border_color().r8() == 0x33);
    REQUIRE(w->border_color().g8() == 0x66);
    REQUIRE(w->border_color().b8() == 0x99);
    // Radius must survive the shorthand assignment.
    REQUIRE_THAT(w->corner_radius(), WithinAbs(10.0f, 1e-5f));
}

// pulp #1148 — generalized overlay-click routing. The bridge must expose
// claimOverlay(id) / releaseOverlay(id) so @pulp/react's `<View overlay>`
// JSX prop can opt a widget in as the active click-eligible overlay.
TEST_CASE("WidgetBridge claimOverlay / releaseOverlay drive View::active_overlay_",
          "[view][bridge][issue-1148]") {
    // Reset state — other tests may leave the slot set.
    pulp::view::View::active_overlay_ = nullptr;

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createPanel('popover', '')");
    auto* popover = bridge.widget("popover");
    REQUIRE(popover != nullptr);
    REQUIRE(pulp::view::View::active_overlay_ == nullptr);

    bridge.load_script("claimOverlay('popover')");
    REQUIRE(pulp::view::View::active_overlay_ == popover);

    bridge.load_script("releaseOverlay('popover')");
    REQUIRE(pulp::view::View::active_overlay_ == nullptr);

    // releaseOverlay on a non-holder is a silent no-op (does not null
    // a different widget's claim).
    bridge.load_script("createPanel('other', ''); claimOverlay('other')");
    auto* other = bridge.widget("other");
    REQUIRE(pulp::view::View::active_overlay_ == other);
    bridge.load_script("releaseOverlay('popover')");
    REQUIRE(pulp::view::View::active_overlay_ == other);

    // Cleanup so the global state doesn't leak into the next test.
    pulp::view::View::active_overlay_ = nullptr;
}

// pulp #1361 — claimOverlay must install on_overlay_dismissed so React
// `<View overlay onDismissed>` consumers can flip setOpen(false) when the
// framework dismisses the overlay via ESC or outside-click.
TEST_CASE("WidgetBridge claimOverlay installs dismiss callback that fires "
          "__dispatch__('id', 'dismiss', 0) [issue-1361]",
          "[view][bridge][issue-1361]") {
    pulp::view::View::active_overlay_ = nullptr;

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createPanel('popover', '')");
    auto* popover = bridge.widget("popover");
    REQUIRE(popover != nullptr);

    // Install a JS-side recorder for __dispatch__ so we can observe the
    // bridge firing the 'dismiss' event when dismiss_active_overlay()
    // runs.
    bridge.load_script(
        "globalThis.__dismissLog = [];"
        "const __orig = globalThis.__dispatch__;"
        "globalThis.__dispatch__ = (id, type, val) => {"
        "  if (type === 'dismiss') globalThis.__dismissLog.push(id);"
        "  return __orig ? __orig(id, type, val) : undefined;"
        "}");

    bridge.load_script("claimOverlay('popover')");
    REQUIRE(pulp::view::View::active_overlay_ == popover);
    REQUIRE(static_cast<bool>(popover->on_overlay_dismissed));

    // Simulate the platform host's ESC / outside-click dismissal path.
    pulp::view::View::dismiss_active_overlay();
    REQUIRE(pulp::view::View::active_overlay_ == nullptr);

    // The dismiss callback should have fired __dispatch__('popover', 'dismiss', 0).
    auto count = engine.evaluate("globalThis.__dismissLog.length")
                       .getWithDefault<double>(-1);
    REQUIRE(count == 1);
    auto first_id = engine.evaluate("globalThis.__dismissLog[0]")
                          .getWithDefault<std::string>("");
    REQUIRE(first_id == "popover");

    // releaseOverlay (the JSX-unmount path) must clear the dismiss
    // callback so a subsequent dismiss_active_overlay() can't re-fire on
    // the now-detached widget.
    pulp::view::View::active_overlay_ = popover;  // simulate re-claim
    popover->on_overlay_dismissed = []() {};      // re-install (claim path)
    bridge.load_script("releaseOverlay('popover')");
    REQUIRE_FALSE(static_cast<bool>(popover->on_overlay_dismissed));

    pulp::view::View::active_overlay_ = nullptr;
}

// pulp #1420 — `display` CSS values translate to native bridge calls.
// Spectr triage of yoga drift (post-#1395 harness) showed 5 display
// values across 79 sites: flex (63), block (10), inline-block (3),
// none (2), inline-flex (1). Before this fix, inline-block and
// inline-flex were silently dropped. After: inline-block ≡ block,
// inline-flex ≡ flex (matches RN + CSS spec for non-text-flowing
// formatting contexts).
TEST_CASE("CSSStyleDeclaration display routes none/flex/block/inline-block/inline-flex correctly",
          "[view][bridge][css][issue-1420]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    auto apply_display = [&](const std::string& id, const std::string& value) {
        std::string js = "(function(){"
            "createPanel('" + id + "', '');"
            "var el = { _id: '" + id + "', _nativeCreated: true };"
            "var sd = new CSSStyleDeclaration(el);"
            "sd._applyProperty('display', '" + value + "');"
            "})();";
        bridge.load_script(js);
    };

    apply_display("p_flex", "flex");
    apply_display("p_block", "block");
    apply_display("p_inline_block", "inline-block");
    apply_display("p_inline_flex", "inline-flex");
    apply_display("p_none", "none");

    auto* p_flex = dynamic_cast<Panel*>(bridge.widget("p_flex"));
    auto* p_block = dynamic_cast<Panel*>(bridge.widget("p_block"));
    auto* p_inline_block = dynamic_cast<Panel*>(bridge.widget("p_inline_block"));
    auto* p_inline_flex = dynamic_cast<Panel*>(bridge.widget("p_inline_flex"));
    auto* p_none = dynamic_cast<Panel*>(bridge.widget("p_none"));
    REQUIRE(p_flex != nullptr);
    REQUIRE(p_block != nullptr);
    REQUIRE(p_inline_block != nullptr);
    REQUIRE(p_inline_flex != nullptr);
    REQUIRE(p_none != nullptr);

    // display: flex / inline-flex must set flex direction to row
    // (overriding the RN-style column default).
    REQUIRE(p_flex->flex().direction == FlexDirection::row);
    REQUIRE(p_inline_flex->flex().direction == FlexDirection::row);

    // display: block / inline-block must NOT touch flex direction.
    REQUIRE(p_block->flex().direction == FlexDirection::column);
    REQUIRE(p_inline_block->flex().direction == FlexDirection::column);

    // All four "visible" variants stay visible. display: none flips
    // the View::visible() flag (the canonical CSS-spec "skip render"
    // signal) — the bridge wires setVisible → View::set_visible.
    REQUIRE(p_flex->visible());
    REQUIRE(p_block->visible());
    REQUIRE(p_inline_block->visible());
    REQUIRE(p_inline_flex->visible());
    REQUIRE_FALSE(p_none->visible());
}

// pulp-internal #105 / rn-display + yoga-display coverage-gap closures —
// `display: contents` is intentionally NOT implemented in Pulp's display
// dispatcher. Per CLAUDE.md's flex+grid-only layout policy (Yoga is the
// engine; CSS block-flow / inline-flow / table-flow are out of scope by
// design), `display: contents` — which renders an element's children as
// if they were children of the element's parent — cannot be modeled
// without a parallel layout engine. Closing the coverage-gap rows means
// pinning the safe no-op: an unrecognized display value must not crash
// the dispatch path, must not silently mutate visible() / flex(), and
// must let consumers fall back to whatever the previous display state
// was.
TEST_CASE("CSSStyleDeclaration display: contents is a safe arch-deferred no-op",
          "[view][bridge][css][issue-1420][display-contents]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Start the panel with display: flex so we have a known baseline:
    // visible=true, direction=row. Then write display: contents and
    // assert nothing moves.
    bridge.load_script(R"((function(){
        createPanel('p_contents', '');
        var el = { _id: 'p_contents', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(el);
        sd._applyProperty('display', 'flex');
        sd._applyProperty('display', 'contents');
    })();)");

    auto* p_contents = dynamic_cast<Panel*>(bridge.widget("p_contents"));
    REQUIRE(p_contents != nullptr);

    // Two invariants:
    //  1. The bridge did not crash and the widget exists.
    //  2. `display: contents` did NOT touch visibility — the prior
    //     `display: flex` state survives. (If a future change adds a
    //     `contents` branch that calls setVisible(false), this assert
    //     fails first.)
    REQUIRE(p_contents->visible());

    // And the flex direction set by the prior `display: flex` is also
    // intact — `display: contents` is a no-op on the flex direction
    // because the dispatcher doesn't have a `contents` branch.
    REQUIRE(p_contents->flex().direction == FlexDirection::row);
}

