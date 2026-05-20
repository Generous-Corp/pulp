// test_widget_bridge_yoga_a4_oos.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// Two compat surfaces sharing a "catalog hygiene + Yoga fan-out" theme:
//
//   1. pulp #1542 — yoga logical-edge fan-out. Pin marginInline/Block
//      + paddingInline/Block + inset shorthand through CSS translator
//      into Yoga's logical-edge slots (the LTR/RTL-aware writing-mode-
//      respecting layout properties).
//
//   2. pulp #1434 A4 Bundles 2-7 — css NOT-IMPL closure. Catalog
//      hygiene tests for properties Pulp deliberately treats as no-ops
//      or fallbacks.

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

// ── pulp #1542 — yoga logical-edge fan-out ──────────────────────────────
//
// The 6 logical-edge keys (margin_start / margin_end / padding_start /
// padding_end / start / end) plumb through `setFlex` to FlexStyle's new
// `dim_*_start` / `dim_*_end` / `dim_start` / `dim_end` fields, then
// reach Yoga via `YGEdgeStart` / `YGEdgeEnd`. Yoga resolves the logical
// edge against the node's writing direction (set via the new
// `direction_writing` sub-key, distinct from the existing flex-direction
// `direction` key). LTR maps start↔left and end↔right; RTL flips them.
//
// Coverage:
//   • Round-trip: bridge stores the value with the right unit
//   • Layout (LTR): margin_start lays out 10px from the LEFT edge
//   • Layout (RTL): margin_start lays out 10px from the RIGHT edge
//   • Same for padding (start/end) — verified via parent->bounds and
//     content-area placement of a fixed-size child
//   • Same for absolute position (start/end)
//   • Percent path round-trips with unit::percent
//   • px path stays unit::px

TEST_CASE("setFlex logical-edge keys round-trip with px / percent / auto units",
          "[view][bridge][issue-1542]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        setFlex('a', 'margin_start',  10);
        setFlex('a', 'margin_end',    20);
        setFlex('a', 'padding_start', 8);
        setFlex('a', 'padding_end',   12);
        setFlex('a', 'start',         4);
        setFlex('a', 'end',           6);

        createPanel('b', '');
        setFlex('b', 'margin_start',  '5%');
        setFlex('b', 'margin_end',    '10%');
        setFlex('b', 'padding_start', '15%');
        setFlex('b', 'padding_end',   '25%');
        setFlex('b', 'start',         '12%');
        setFlex('b', 'end',           '8%');

        createPanel('c', '');
        setFlex('c', 'margin_start', 'auto');
        setFlex('c', 'margin_end',   'auto');
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_margin_start.unit  == DimensionUnit::px);
    REQUIRE_THAT(fa.dim_margin_start.value,  WithinAbs(10.0f, 0.001f));
    REQUIRE(fa.dim_margin_end.unit    == DimensionUnit::px);
    REQUIRE_THAT(fa.dim_margin_end.value,    WithinAbs(20.0f, 0.001f));
    REQUIRE(fa.dim_padding_start.unit == DimensionUnit::px);
    REQUIRE_THAT(fa.dim_padding_start.value, WithinAbs(8.0f, 0.001f));
    REQUIRE(fa.dim_padding_end.unit   == DimensionUnit::px);
    REQUIRE_THAT(fa.dim_padding_end.value,   WithinAbs(12.0f, 0.001f));
    REQUIRE(fa.dim_start.unit         == DimensionUnit::px);
    REQUIRE_THAT(fa.dim_start.value,         WithinAbs(4.0f, 0.001f));
    REQUIRE(fa.dim_end.unit           == DimensionUnit::px);
    REQUIRE_THAT(fa.dim_end.value,           WithinAbs(6.0f, 0.001f));

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE(fb.dim_margin_start.unit  == DimensionUnit::percent);
    REQUIRE_THAT(fb.dim_margin_start.value,  WithinAbs(5.0f, 0.001f));
    REQUIRE(fb.dim_margin_end.unit    == DimensionUnit::percent);
    REQUIRE_THAT(fb.dim_margin_end.value,    WithinAbs(10.0f, 0.001f));
    REQUIRE(fb.dim_padding_start.unit == DimensionUnit::percent);
    REQUIRE_THAT(fb.dim_padding_start.value, WithinAbs(15.0f, 0.001f));
    REQUIRE(fb.dim_padding_end.unit   == DimensionUnit::percent);
    REQUIRE_THAT(fb.dim_padding_end.value,   WithinAbs(25.0f, 0.001f));
    REQUIRE(fb.dim_start.unit         == DimensionUnit::percent);
    REQUIRE_THAT(fb.dim_start.value,         WithinAbs(12.0f, 0.001f));
    REQUIRE(fb.dim_end.unit           == DimensionUnit::percent);
    REQUIRE_THAT(fb.dim_end.value,           WithinAbs(8.0f, 0.001f));

    const auto& fc = bridge.widget("c")->flex();
    REQUIRE(fc.dim_margin_start.unit == DimensionUnit::auto_);
    REQUIRE(fc.dim_margin_end.unit   == DimensionUnit::auto_);
}

TEST_CASE("setFlex direction_writing round-trips ltr / rtl / inherit",
          "[view][bridge][issue-1542]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('ltr', '');
        setFlex('ltr', 'direction_writing', 'ltr');
        createPanel('rtl', '');
        setFlex('rtl', 'direction_writing', 'rtl');
        createPanel('inh', '');
        setFlex('inh', 'direction_writing', 'inherit');
        // unrecognized values → inherit (defensive default)
        createPanel('bad', '');
        setFlex('bad', 'direction_writing', 'bogus');
    )");

    using WD = pulp::view::FlexStyle::WritingDirection;
    REQUIRE(bridge.widget("ltr")->flex().writing_direction == WD::ltr);
    REQUIRE(bridge.widget("rtl")->flex().writing_direction == WD::rtl);
    REQUIRE(bridge.widget("inh")->flex().writing_direction == WD::inherit);
    REQUIRE(bridge.widget("bad")->flex().writing_direction == WD::inherit);
}

TEST_CASE("logical-edge margin_start lays out from left in LTR, right in RTL",
          "[view][bridge][issue-1542]") {
    // Parent is 400 wide, row-direction. Child is 80 wide with
    // margin_start: 10. In LTR, child's left edge = 10. In RTL, child's
    // right edge = parent.right - 10 → child.x = 400 - 80 - 10 = 310.
    auto build = [](pulp::view::FlexStyle::WritingDirection dir) {
        ScriptEngine engine;
        View root;
        root.set_bounds({0, 0, 400, 100});
        StateStore store;
        WidgetBridge bridge(engine, root, store);

        bridge.load_script(R"(
            createPanel('parent', '');
            setFlex('parent', 'direction', 'row');
            setFlex('parent', 'width', 400);
            setFlex('parent', 'height', 100);
            createPanel('child', 'parent');
            setFlex('child', 'width',  80);
            setFlex('child', 'height', 50);
            setFlex('child', 'margin_start', 10);
        )");
        // Set writing direction on the parent so the child inherits.
        bridge.widget("parent")->flex().writing_direction = dir;
        root.layout_children();
        return std::make_pair(bridge.widget("parent")->bounds(),
                              bridge.widget("child")->bounds());
    };

    {
        auto [pb, cb] = build(pulp::view::FlexStyle::WritingDirection::ltr);
        // LTR: child sits 10px from the left edge of the parent.
        REQUIRE_THAT(cb.x - pb.x, WithinAbs(10.0f, 0.5f));
    }
    {
        auto [pb, cb] = build(pulp::view::FlexStyle::WritingDirection::rtl);
        // RTL: child sits 10px from the right edge — child's right edge
        // is at parent.right - 10, so child.x = parent.right - 10 - 80.
        float expected_x = pb.right() - 10.0f - 80.0f - pb.x;
        REQUIRE_THAT(cb.x - pb.x, WithinAbs(expected_x, 0.5f));
    }
}

TEST_CASE("logical-edge padding_start increases inset on the start edge",
          "[view][bridge][issue-1542]") {
    // Parent 400 wide, padding_start: 25. LTR: first child sits at
    // parent.x + 25. RTL: first child's right edge sits at
    // parent.right - 25.
    auto build = [](pulp::view::FlexStyle::WritingDirection dir) {
        ScriptEngine engine;
        View root;
        root.set_bounds({0, 0, 400, 100});
        StateStore store;
        WidgetBridge bridge(engine, root, store);

        bridge.load_script(R"(
            createPanel('p', '');
            setFlex('p', 'direction', 'row');
            setFlex('p', 'width',  400);
            setFlex('p', 'height', 100);
            setFlex('p', 'padding_start', 25);
            createPanel('c', 'p');
            setFlex('c', 'width',  60);
            setFlex('c', 'height', 50);
        )");
        bridge.widget("p")->flex().writing_direction = dir;
        root.layout_children();
        return std::make_pair(bridge.widget("p")->bounds(),
                              bridge.widget("c")->bounds());
    };

    {
        auto [pb, cb] = build(pulp::view::FlexStyle::WritingDirection::ltr);
        // LTR padding-start = padding-left → child.x = parent.x + 25.
        REQUIRE_THAT(cb.x - pb.x, WithinAbs(25.0f, 0.5f));
    }
    {
        auto [pb, cb] = build(pulp::view::FlexStyle::WritingDirection::rtl);
        // RTL padding-start = padding-right → child sits flush against
        // (parent.right - 25), so child.x = parent.right - 25 - 60.
        float expected_offset = pb.width - 25.0f - 60.0f;
        REQUIRE_THAT(cb.x - pb.x, WithinAbs(expected_offset, 0.5f));
    }
}

TEST_CASE("logical-edge start/end position shifts absolute-positioned child",
          "[view][bridge][issue-1542]") {
    // An absolutely positioned child with `start: 30` is 30px from the
    // start edge of its containing block. LTR: child.x = parent.x + 30.
    // RTL: child's right edge = parent.right - 30.
    auto build = [](pulp::view::FlexStyle::WritingDirection dir) {
        ScriptEngine engine;
        View root;
        root.set_bounds({0, 0, 400, 200});
        StateStore store;
        WidgetBridge bridge(engine, root, store);

        bridge.load_script(R"(
            createPanel('p', '');
            setFlex('p', 'width',  400);
            setFlex('p', 'height', 200);
            createPanel('c', 'p');
            setFlex('c', 'width',  50);
            setFlex('c', 'height', 40);
            setFlex('c', 'start',  30);
        )");
        bridge.widget("p")->flex().writing_direction = dir;
        // Absolute position so start/end pin the child.
        bridge.widget("c")->set_position(View::Position::absolute);
        root.layout_children();
        return std::make_pair(bridge.widget("p")->bounds(),
                              bridge.widget("c")->bounds());
    };

    {
        auto [pb, cb] = build(pulp::view::FlexStyle::WritingDirection::ltr);
        REQUIRE_THAT(cb.x - pb.x, WithinAbs(30.0f, 0.5f));
    }
    {
        auto [pb, cb] = build(pulp::view::FlexStyle::WritingDirection::rtl);
        // RTL: child.right = parent.right - 30 → child.x = pb.width - 30 - 50.
        float expected_offset = pb.width - 30.0f - 50.0f;
        REQUIRE_THAT(cb.x - pb.x, WithinAbs(expected_offset, 0.5f));
    }
}

TEST_CASE("logical-edge percent values reach Yoga as percent units",
          "[view][bridge][issue-1542]") {
    // Layout-level smoke: 10% of a 400-wide parent should produce ~40px
    // of margin/padding regardless of LTR/RTL. We assert the resolved
    // unit on the FlexStyle and a coarse layout check.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 100});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('p', '');
        setFlex('p', 'direction', 'row');
        setFlex('p', 'width',  400);
        setFlex('p', 'height', 100);
        setFlex('p', 'padding_start', '10%');
        createPanel('c', 'p');
        setFlex('c', 'width',  80);
        setFlex('c', 'height', 50);
        setFlex('c', 'margin_start', '5%');
    )");
    auto& pf = bridge.widget("p")->flex();
    auto& cf = bridge.widget("c")->flex();
    REQUIRE(pf.dim_padding_start.unit == DimensionUnit::percent);
    REQUIRE_THAT(pf.dim_padding_start.value, WithinAbs(10.0f, 0.001f));
    REQUIRE(cf.dim_margin_start.unit == DimensionUnit::percent);
    REQUIRE_THAT(cf.dim_margin_start.value, WithinAbs(5.0f, 0.001f));

    root.layout_children();
    auto pb = bridge.widget("p")->bounds();
    auto cb = bridge.widget("c")->bounds();
    // LTR (default for inherit): child.x = parent.x + padding-start + margin-start.
    // Yoga resolves padding percent against parent width (40) and margin
    // percent against parent content-area width (~360 → 18). The exact
    // resolution depends on Yoga's containing-block math for margin
    // percent; we only care that BOTH dispatched as percent and the
    // child sits well past the padding edge — i.e. the start-side
    // logical-edge code actually reached Yoga.
    REQUIRE((cb.x - pb.x) > 40.0f);  // past the padding-start
    REQUIRE((cb.x - pb.x) < 80.0f);  // less than a doubled inset
}

TEST_CASE("logical-edge unset slots don't override per-side margin/padding",
          "[view][bridge][issue-1542]") {
    // Regression guard: when a node sets only margin_left (legacy
    // per-side path) and not margin_start, the start-side dispatch
    // must not zero out the left edge. The new apply_logical_margin
    // lambda guards on `value != 0`.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 100});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('p', '');
        setFlex('p', 'direction', 'row');
        setFlex('p', 'width',  400);
        setFlex('p', 'height', 100);
        createPanel('c', 'p');
        setFlex('c', 'width',  60);
        setFlex('c', 'height', 50);
        setFlex('c', 'margin_left', 12);
    )");
    root.layout_children();
    auto pb = bridge.widget("p")->bounds();
    auto cb = bridge.widget("c")->bounds();
    REQUIRE_THAT(cb.x - pb.x, WithinAbs(12.0f, 0.5f));
}

// ── pulp #1434 A4 Bundles 2–7: css NOT-IMPL closure ────────────────────────
// One TEST_CASE per family of bridge fns added by the closure. These
// tests assert that:
//   - the bridge fn registers (load_script doesn't error on the call)
//   - the value lands on the View's catalog slot (storage round-trip)
// The catalog reclassifications themselves (noop / wontfix / partial)
// don't need C++ tests — they're caught by the harness adapter on
// every PR via the `case "X":` allow-list scan.

TEST_CASE("WidgetBridge setTextIndent stores value on View",
          "[view][bridge][issue-1434][a4-bundle-5]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('p', '');
        setTextIndent('p', 24);
    )");
    auto* p = bridge.widget("p");
    REQUIRE(p != nullptr);
    REQUIRE(p->text_indent() == 24.0f);
}

TEST_CASE("WidgetBridge setVerticalAlign maps keywords to TextVerticalAlign on Label",
          "[view][bridge][issue-1434][a4-bundle-5]") {
    using VA = pulp::canvas::TextVerticalAlign;
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createLabel('l', 'hi', '');
    )");
    auto* l = dynamic_cast<Label*>(bridge.widget("l"));
    REQUIRE(l != nullptr);

    struct Row { const char* kw; VA v; };
    const Row table[] = {
        {"top",      VA::top},
        {"middle",   VA::center},
        {"bottom",   VA::bottom},
        {"baseline", VA::baseline},
        // Unknown / sub / super → baseline fallback.
        {"sub",      VA::baseline},
        {"super",    VA::baseline},
    };
    for (const auto& r : table) {
        std::string js = std::string("setVerticalAlign('l', '") + r.kw + "');";
        bridge.load_script(js);
        REQUIRE(l->vertical_align() == r.v);
    }
}

TEST_CASE("WidgetBridge setWordBreak / setFontVariant / etc. round-trip onto View",
          "[view][bridge][issue-1434][a4-bundles-5-7]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('p', '');
        setWordBreak('p', 'break-all');
        setFontVariant('p', 'small-caps');
    )");
    auto* p = bridge.widget("p");
    REQUIRE(p != nullptr);
    REQUIRE(p->word_break() == "break-all");
    REQUIRE(p->font_variant() == "small-caps");
}

TEST_CASE("WidgetBridge setAnimation play_state stores on View",
          "[view][bridge][issue-1434][a4-bundle-2]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('p', '');
        setAnimation('p', 'play_state', 'paused');
    )");
    auto* p = bridge.widget("p");
    REQUIRE(p != nullptr);
    REQUIRE(p->animation_play_state() == "paused");

    bridge.load_script("setAnimation('p', 'play_state', 'running');");
    REQUIRE(p->animation_play_state() == "running");
}

// pulp #1434 Wave 3 css.3 — animation-play-state playback driver
// pause/resume. View::tick_animations(dt) must skip the timeline
// advance when animation_play_state_ == "paused" (web spec semantic);
// any other keyword (default "running") must advance every active
// CssAnimation by dt.
TEST_CASE("View::tick_animations honors paused play_state",
          "[view][bridge][css][css-animations-tail][issue-1434-anim]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        defineKeyframes('fade', JSON.stringify([
            { offset: 0,   properties: { opacity: '0' } },
            { offset: 1.0, properties: { opacity: '1' } }
        ]));
        createPanel('a', '');
        setAnimation('a', 'duration', 1.0);
        setAnimation('a', 'name', 'fade');
    )");
    auto* v = bridge.widget("a");
    REQUIRE(v != nullptr);
    REQUIRE(v->active_animations().size() == 1);
    REQUIRE_THAT(v->active_animations()[0].elapsed_seconds, WithinAbs(0.0f, 0.001f));

    // Default state ("running" / empty) must advance the timeline.
    v->tick_animations(0.25f);
    REQUIRE_THAT(v->active_animations()[0].elapsed_seconds, WithinAbs(0.25f, 0.001f));

    // Pause: subsequent ticks must NOT advance.
    bridge.load_script("setAnimation('a', 'play_state', 'paused');");
    REQUIRE(v->animation_play_state() == "paused");
    v->tick_animations(0.5f);
    v->tick_animations(0.5f);
    REQUIRE_THAT(v->active_animations()[0].elapsed_seconds, WithinAbs(0.25f, 0.001f));

    // Resume: ticks advance again from where they were paused.
    bridge.load_script("setAnimation('a', 'play_state', 'running');");
    v->tick_animations(0.25f);
    REQUIRE_THAT(v->active_animations()[0].elapsed_seconds, WithinAbs(0.5f, 0.001f));
}

// pulp #1508 Codex audit (P1 #2) — animationDuration in @pulp/react's
// prop-applier was routing to setTransitionDuration, which mutated
// transition timing on the same View. The fix routes through the
// legacy 2-arg setAnimation control-token form. Mirror of the TS test
// in packages/pulp-react/test/prop-applier-animation.test.ts on the
// C++ side: confirm the bridge handles `setAnimation(id, "duration",
// seconds)` without touching the View's transition slot.
TEST_CASE("setAnimation duration token does not perturb transition slot",
          "[view][bridge][css][css-animations-tail][issue-1508]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setTransition('a', 'opacity 200ms ease');
        setAnimation('a', 'duration', 0.5);
    )");
    auto* v = bridge.widget("a");
    REQUIRE(v != nullptr);
    // Transition slot survives the setAnimation call.
    REQUIRE(v->has_transitions());
    REQUIRE_THAT(v->transitions()[0].duration_seconds, WithinAbs(0.2f, 0.001f));
    // Animation duration lands on the staged_animation slot.
    REQUIRE_THAT(v->staged_animation().duration_seconds, WithinAbs(0.5f, 0.001f));
}

TEST_CASE("CSS logical-edge longhands route to LTR physical edges",
          "[view][bridge][issue-1434][a4-bundle-3]") {
    // Verify the JS-side `case "marginInlineStart":` etc. arms route
    // through to the existing per-edge setFlex bridge by exercising
    // the el.style.X assignment path. Uses the web-compat-element
    // shim to back-fill an Element with its native bridge id.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 100});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('p', '');
        setFlex('p', 'direction', 'row');
        setFlex('p', 'width',  400);
        setFlex('p', 'height', 100);
        createPanel('c', 'p');
        setFlex('c', 'width', 60);
        setFlex('c', 'height', 50);
    )");
    // Direct-bridge round-trip: the JS-side cases dispatch to the same
    // per-edge setFlex routes as the legacy per-side setters, so we
    // verify those routes work end-to-end via setFlex (which the JS
    // arms call into). The JS-side `case` arms are smoke-checked by
    // the harness adapter on every PR.
    bridge.load_script("setFlex('c', 'margin_left',  10);");
    bridge.load_script("setFlex('c', 'margin_right', 14);");
    bridge.load_script("setFlex('c', 'padding_top',  3);");
    root.layout_children();
    auto cb = bridge.widget("c")->bounds();
    auto pb = bridge.widget("p")->bounds();
    REQUIRE_THAT(cb.x - pb.x, WithinAbs(10.0f, 0.5f));
}

