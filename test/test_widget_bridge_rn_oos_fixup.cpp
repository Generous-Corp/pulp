// test_widget_bridge_rn_oos_fixup.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// pulp #1737 RN-OOS-fixup (catalog audit 2026-05-11).
//
// Multiple final-sweep entries from the RN-OOS catalog reconciliation:
//   * Material shim's rn/elevation
//   * includeFontPadding round-trip
//   * pulp #1812 borderCurve squircle paint dispatch
//   * isolation honest CSS-subset
//   * other RN-side OOS catalog hygiene checks
//
// Tests pin the bridge ↔ View slot contract for properties Pulp
// deliberately treats as no-ops, sensible-fallbacks, or wires through
// to the documented Pulp-side surface (e.g. RN's elevation maps to
// Material-style shadow under specific theme conditions).

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

// ── pulp #1737 RN-OOS-fixup (catalog audit 2026-05-11) ──────────────────
// Followup wave: 4 RN box-shadow longhand setters + 2 CSS scroll-behavior
// slots. All 6 cited in compat.json mapsTo claims — these tests pin the
// bridge fn surface so the audit's catalog claims are evidence-backed.

TEST_CASE("WidgetBridge setShadowColor mutates View::shadow_.color + activates has_shadow_",
          "[view][bridge][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    bridge.load_script("setShadowColor('gain', '#ff0000')");

    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->has_box_shadow());
    REQUIRE_THAT(w->box_shadow().color.r, WithinAbs(1.0f, 0.01f));
    REQUIRE_THAT(w->box_shadow().color.g, WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(w->box_shadow().color.b, WithinAbs(0.0f, 0.01f));
}

TEST_CASE("WidgetBridge setShadowOffset mutates offset_x / offset_y in isolation",
          "[view][bridge][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    bridge.load_script("setShadowOffset('gain', 7, 11)");

    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->has_box_shadow());
    REQUIRE_THAT(w->box_shadow().offset_x, WithinAbs(7.0f, 0.001f));
    REQUIRE_THAT(w->box_shadow().offset_y, WithinAbs(11.0f, 0.001f));
}

TEST_CASE("WidgetBridge setShadowOpacity writes color alpha (0..1)",
          "[view][bridge][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    bridge.load_script("setShadowOpacity('gain', 0.5)");

    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->has_box_shadow());
    REQUIRE_THAT(w->box_shadow().color.a, WithinAbs(0.5f, 0.001f));
}

TEST_CASE("WidgetBridge setShadowRadius writes the blur field of View::shadow_",
          "[view][bridge][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    bridge.load_script("setShadowRadius('gain', 22)");

    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->has_box_shadow());
    REQUIRE_THAT(w->box_shadow().blur, WithinAbs(22.0f, 0.001f));
}

TEST_CASE("WidgetBridge setScrollBehavior stores the keyword on View",
          "[view][bridge][issue-1737]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    bridge.load_script("setScrollBehavior('gain', 'smooth')");

    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->scroll_behavior() == "smooth");

    bridge.load_script("setScrollBehavior('gain', 'auto')");
    REQUIRE(w->scroll_behavior() == "auto");
}

TEST_CASE("WidgetBridge setOverscrollBehavior stores the keyword on View",
          "[view][bridge][issue-1737]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    bridge.load_script("setOverscrollBehavior('gain', 'contain')");

    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->overscroll_behavior() == "contain");

    bridge.load_script("setOverscrollBehavior('gain', 'none')");
    REQUIRE(w->overscroll_behavior() == "none");
}

// pulp #1737 RN-OOS-fixup final sweep — rn/elevation Material shim.
TEST_CASE("WidgetBridge setElevation shims to Material-approx box-shadow",
          "[view][bridge][issue-1737]") {
    using Catch::Matchers::WithinAbs;
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");

    // elevation 4 -> offset_y=2, blur=5, alpha≈0.19 (per the formula in
    // widget_bridge.cpp: offset_y=max(1, n/2), blur=n+1, alpha=clamp(0.15+n*0.01, 0.15, 0.30)).
    bridge.load_script("setElevation('gain', 4)");
    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->has_box_shadow());
    REQUIRE_THAT(w->box_shadow().offset_x, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(w->box_shadow().offset_y, WithinAbs(2.0f, 0.001f));
    REQUIRE_THAT(w->box_shadow().blur,     WithinAbs(5.0f, 0.001f));
    REQUIRE_THAT(w->box_shadow().color.a,  WithinAbs(0.19f, 0.01f));

    // elevation 0 -> clears the shadow entirely.
    bridge.load_script("setElevation('gain', 0)");
    REQUIRE_FALSE(w->has_box_shadow());

    // elevation 24 (max) -> alpha saturates at 0.30.
    bridge.load_script("setElevation('gain', 24)");
    REQUIRE(w->has_box_shadow());
    REQUIRE_THAT(w->box_shadow().color.a, WithinAbs(0.30f, 0.001f));
}

// pulp #1737 RN-OOS-fixup (final sweep) — includeFontPadding round-trip.
TEST_CASE("WidgetBridge setIncludeFontPadding stores the keyword on View (round-trip only)",
          "[view][bridge][issue-1737]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");

    // Default state — Pulp's View has include_font_padding_ = true.
    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->include_font_padding() == true);

    // Setting `false` (the common case — remove Android padding):
    // Pulp accepts the keyword and stores it. Text shaping is
    // unchanged because Pulp never had Android-vestigial padding.
    bridge.load_script("setIncludeFontPadding('gain', false)");
    REQUIRE(w->include_font_padding() == false);

    // Setting `true` round-trips even though Pulp can't add Android-
    // style padding — author can still query the slot.
    bridge.load_script("setIncludeFontPadding('gain', true)");
    REQUIRE(w->include_font_padding() == true);
}

// pulp #1737 RN-OOS-fixup #1812 — borderCurve squircle paint dispatch.
TEST_CASE("WidgetBridge setBorderCurve toggles between circular and continuous",
          "[view][bridge][issue-1737]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);
    REQUIRE(w->border_curve() == View::BorderCurve::circular);  // default

    bridge.load_script("setBorderCurve('gain', 'continuous')");
    REQUIRE(w->border_curve() == View::BorderCurve::continuous);

    bridge.load_script("setBorderCurve('gain', 'circular')");
    REQUIRE(w->border_curve() == View::BorderCurve::circular);

    // Unknown keyword falls back to circular (matches RN spec: unknown → default).
    bridge.load_script("setBorderCurve('gain', 'banana')");
    REQUIRE(w->border_curve() == View::BorderCurve::circular);
}

// pulp #1737 RN-OOS-fixup (final round) — isolation honest CSS-subset.
TEST_CASE("WidgetBridge setIsolation round-trips on View slot (no paint impact)",
          "[view][bridge][issue-1737]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('gain', 10, 10, 48, 48)");
    auto* w = bridge.widget("gain");
    REQUIRE(w != nullptr);

    bridge.load_script("setIsolation('gain', 'isolate')");
    REQUIRE(w->isolation() == "isolate");

    bridge.load_script("setIsolation('gain', 'auto')");
    REQUIRE(w->isolation() == "auto");
}



// pulp-internal Tier-1 closure for css/textTransform (2026-05-12).
// The setTextTransform bridge already accepts the 4 CSS spec values
// (uppercase / lowercase / capitalize / none) and routes them onto
// Label::TextTransform. The existing widget-bridge sanity test
// (line ~875) only exercised `uppercase` once. This focused test
// pins the full supported value set so the row's closure (move
// `full-width` / `full-size-kana` to arch-deferred-CJK-Unicode-width
// in compat.json) is honest.
TEST_CASE("setTextTransform pins all 4 CSS spec values to Label::TextTransform enum",
          "[view][bridge][css][tier1-closure][css-textTransform]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('upper',      'hello', '');
        createLabel('lower',      'HELLO', '');
        createLabel('capitalize', 'hello world', '');
        createLabel('none_',      'hello', '');

        setTextTransform('upper',      'uppercase');
        setTextTransform('lower',      'lowercase');
        setTextTransform('capitalize', 'capitalize');
        setTextTransform('none_',      'none');
    )");

    auto tt = [&](const std::string& id) -> Label::TextTransform {
        return dynamic_cast<Label*>(bridge.widget(id))->text_transform();
    };
    REQUIRE(tt("upper")      == Label::TextTransform::uppercase);
    REQUIRE(tt("lower")      == Label::TextTransform::lowercase);
    REQUIRE(tt("capitalize") == Label::TextTransform::capitalize);
    REQUIRE(tt("none_")      == Label::TextTransform::none);
}

// pulp #1923 — drag-style interactions (FilterBank band drawing, slider
// thumb drag, scroll gestures) lost state between pointerdown and the
// immediately-following pointermove because safe_dispatch_eval() didn't
// pump microtasks after dispatching the JS handler. React's setState
// commit is queued as a microtask; without an explicit pump_message_loop
// after engine.evaluate(), the move handler runs against the pre-down
// state and silently bails. This test pins the contract: any side-effect
// the pointerdown handler queues via queueMicrotask / Promise.then
// (which is what React 18's setState uses under the hood) MUST be
// visible to the next dispatched event on the same widget.
TEST_CASE("WidgetBridge pumps microtasks after JS dispatch so drag-state commits before next event",
          "[view][bridge][events][issue-1923]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Pointerdown queues a microtask that mutates `drag_active` (stand-in
    // for a React setState commit). Pointermove reads `drag_active` and
    // records what it saw at the time it fired. Pre-fix the recorded
    // value is `false` (microtask never drained); post-fix it is `true`.
    bridge.load_script(R"(
        var drag_active = false;
        var move_saw_drag_active = null;
        var move_count = 0;

        createLabel('surface', 'Surface', '');
        on('surface', 'pointerdown', function(e) {
            // Stand-in for React's setState commit: queue a microtask
            // that flips the committed state. React 18's scheduler
            // uses queueMicrotask / Promise.resolve().then for the
            // discrete-event lane, so this is the realistic shape.
            queueMicrotask(function() { drag_active = true; });
        });
        on('surface', 'pointermove', function(e) {
            if (move_saw_drag_active === null) {
                move_saw_drag_active = drag_active;
            }
            move_count++;
        });
        registerPointer('surface');
    )");

    auto* surface = bridge.widget("surface");
    REQUIRE(surface != nullptr);
    REQUIRE(surface->on_pointer_event);
    REQUIRE(surface->on_drag);

    // Sanity: nothing has run yet.
    REQUIRE_FALSE(engine.evaluate("drag_active").getWithDefault<bool>(true));

    // Dispatch pointerdown. After safe_dispatch_eval returns, the fix
    // guarantees the queued microtask has drained and `drag_active` is
    // committed. Pre-fix this assertion fails — the microtask is still
    // pending in QuickJS' job queue.
    MouseEvent down{};
    down.is_down = true;
    down.position = {10.0f, 10.0f};
    down.window_position = {110.0f, 110.0f};
    down.pointer_id = 1;
    down.pointer_type = PointerType::mouse;
    down.button = MouseButton::left;
    surface->on_mouse_event(down);

    REQUIRE(engine.evaluate("drag_active").getWithDefault<bool>(false));

    // Now dispatch pointermove. The handler must observe the committed
    // drag_active value (true), not the pre-down value (false). This is
    // the user-visible regression in #1923: pointermove handlers see
    // stale state and bail.
    surface->on_drag({12.0f, 14.0f});

    REQUIRE(engine.evaluate("move_count").getWithDefault<int>(0) == 1);
    REQUIRE(engine.evaluate("move_saw_drag_active").getWithDefault<bool>(false));
}

// pulp #1576 — bridge-level pin for min/max width/height with %, auto,
// and calc-family inputs. The dispatcher at web-compat-style-decl.js
// cases minWidth / minHeight / maxWidth / maxHeight previously had a
// dual-path (calc-family detection guard + parseCSSLength fallback);
// #1576 collapsed both into a single resolveCSSLength call. These
// tests pin the unified shape end-to-end through the bridge so a
// regression that re-introduces the dual-path (or drops % preservation
// for calc-family inputs) fails first.
TEST_CASE("minWidth / minHeight / maxWidth / maxHeight route % and calc-family through unified resolveCSSLength",
          "[view][bridge][css][issue-1576]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        // Plain % path — unified resolveCSSLength must preserve unit='%'.
        createPanel('mw-pct', '');
        var sd1 = new CSSStyleDeclaration({ _id: 'mw-pct', _nativeCreated: true });
        sd1._applyProperty('minWidth', '25%');

        createPanel('mh-pct', '');
        var sd2 = new CSSStyleDeclaration({ _id: 'mh-pct', _nativeCreated: true });
        sd2._applyProperty('minHeight', '33%');

        // calc-family % path — Codex P2: calc(50%) must reach the
        // bridge as a percent, not absolute px.
        createPanel('xw-calc-pct', '');
        var sd3 = new CSSStyleDeclaration({ _id: 'xw-calc-pct', _nativeCreated: true });
        sd3._applyProperty('maxWidth', 'min(50%, 75%)');

        // calc-family px path — calc(100px + 50px) must resolve to 150px.
        createPanel('xh-calc-px', '');
        var sd4 = new CSSStyleDeclaration({ _id: 'xh-calc-px', _nativeCreated: true });
        sd4._applyProperty('maxHeight', 'calc(100px + 50px)');
    )");

    auto* mw_pct      = dynamic_cast<Panel*>(bridge.widget("mw-pct"));
    auto* mh_pct      = dynamic_cast<Panel*>(bridge.widget("mh-pct"));
    auto* xw_calc_pct = dynamic_cast<Panel*>(bridge.widget("xw-calc-pct"));
    auto* xh_calc_px  = dynamic_cast<Panel*>(bridge.widget("xh-calc-px"));
    REQUIRE(mw_pct      != nullptr);
    REQUIRE(mh_pct      != nullptr);
    REQUIRE(xw_calc_pct != nullptr);
    REQUIRE(xh_calc_px  != nullptr);

    // % survives: dim_<dim>.unit is the percent sentinel.
    REQUIRE(mw_pct->flex().dim_min_width.unit  == DimensionUnit::percent);
    REQUIRE_THAT(mw_pct->flex().dim_min_width.value, WithinAbs(25.0f, 0.001f));
    REQUIRE(mh_pct->flex().dim_min_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(mh_pct->flex().dim_min_height.value, WithinAbs(33.0f, 0.001f));

    // calc-family % survives — Codex P2 pin (min(50%, 75%) = 50%).
    REQUIRE(xw_calc_pct->flex().dim_max_width.unit  == DimensionUnit::percent);
    REQUIRE_THAT(xw_calc_pct->flex().dim_max_width.value, WithinAbs(50.0f, 0.001f));

    // calc-family px resolves to 150.
    REQUIRE(xh_calc_px->flex().dim_max_height.unit == DimensionUnit::px);
    REQUIRE_THAT(xh_calc_px->flex().dim_max_height.value, WithinAbs(150.0f, 0.001f));
}

// Focus-guard for global key shortcuts. Bare-key shortcuts (no Ctrl/Alt/
// Meta/Cmd; Shift alone counts as bare since it just picks the upper-case
// glyph) must NOT fire while a text input has focus — otherwise typing a
// `?` into a search box would open the global cheatsheet. Modifier chords
// (Cmd+S, Cmd+,) are always-global by design and must still fire when an
// input is focused.
//
// Prereq for the default-shortcuts pass (planning/2026-05-16-default-
// keyboard-shortcuts.md), which adds a bare-`?` cheatsheet binding.
TEST_CASE("WidgetBridge focus-guard: bare-key shortcuts suppressed while text input focused",
          "[view][bridge][shortcuts][focus-guard]") {
    using namespace pulp::view;

    // Always start the test with a clean focus slot so prior cases don't
    // bleed in (the slot is a static and tests share the same process).
    View::focused_input_ = nullptr;

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var bare_count = 0;
        var shift_count = 0;
        var cmd_count = 0;
        function bareHit()  { bare_count++; }
        function shiftHit() { shift_count++; }
        function cmdHit()   { cmd_count++; }

        // Bare-`?` (no modifiers): cheatsheet pattern.
        registerShortcut(63, 0, 'bareHit');
        // Shift+`?` (kModShift = 1): also bare for guard purposes — Shift
        // alone is just the glyph selector when typing.
        registerShortcut(63, 1, 'shiftHit');
        // Cmd+`,` (kModCmd = 16): always-global modifier chord.
        registerShortcut(44, 16, 'cmdHit');
    )");

    SECTION("no input focused: bare-key fires") {
        REQUIRE(View::focused_input_ == nullptr);
        bridge.forward_key_event(63, 0, true);
        REQUIRE(engine.evaluate("bare_count").getWithDefault<int>(0) == 1);
    }

    SECTION("text input focused: bare-key suppressed") {
        TextEditor input;
        input.claim_input_focus();
        REQUIRE(View::focused_input_ == &input);
        REQUIRE(input.accepts_text_input());

        bridge.forward_key_event(63, 0, true);
        REQUIRE(engine.evaluate("bare_count").getWithDefault<int>(0) == 0);

        // Shift alone is also guarded.
        bridge.forward_key_event(63, 1, true);
        REQUIRE(engine.evaluate("shift_count").getWithDefault<int>(0) == 0);

        // Modifier chord still fires — Cmd+, opens Settings even from an
        // input.
        bridge.forward_key_event(44, 16, true);
        REQUIRE(engine.evaluate("cmd_count").getWithDefault<int>(0) == 1);

        input.release_input_focus();
        REQUIRE(View::focused_input_ == nullptr);
    }

    SECTION("non-text focusable (knob/button) focused: bare-key still fires") {
        // Codex review pin (#2120): `focused_input_` is claimed by any
        // focusable widget, not just text inputs. The guard MUST check
        // `accepts_text_input()` — otherwise clicking a knob would kill
        // every global single-key shortcut until focus moved away.
        View knob_like;
        knob_like.set_focusable(true);
        knob_like.claim_input_focus();
        REQUIRE(View::focused_input_ == &knob_like);
        REQUIRE_FALSE(knob_like.accepts_text_input());

        bridge.forward_key_event(63, 0, true);
        REQUIRE(engine.evaluate("bare_count").getWithDefault<int>(0) == 1);

        bridge.forward_key_event(63, 1, true);
        REQUIRE(engine.evaluate("shift_count").getWithDefault<int>(0) == 1);

        knob_like.release_input_focus();
    }

    SECTION("focus released: bare-key fires again") {
        TextEditor input;
        input.claim_input_focus();
        bridge.forward_key_event(63, 0, true);
        REQUIRE(engine.evaluate("bare_count").getWithDefault<int>(0) == 0);

        input.release_input_focus();
        bridge.forward_key_event(63, 0, true);
        REQUIRE(engine.evaluate("bare_count").getWithDefault<int>(0) == 1);
    }
}
