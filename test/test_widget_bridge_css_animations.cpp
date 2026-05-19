// test_widget_bridge_css_animations.cpp — extracted from
// test_widget_bridge.cpp in the 2026-05 Phase 5 (P5-1 follow-up)
// refactor.
//
// pulp #1434 Phase A2-1 — CSS animations + transitions surface.
// Round-trips the full CSS-Animations + CSS-Transitions shim through
// the bridge: animation-name / -duration / -delay / -iteration-count
// / -direction / -fill-mode / -timing-function / -play-state and the
// `animation` shorthand decomposition; transition-property /
// -duration / -timing-function / -delay and the `transition`
// shorthand.

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

// ── pulp #1434 Phase A2-1 — CSS animations + transitions ──────────────
//
// PR 1 of the multi-PR ladder. Establishes the CssEasing /
// AnimatableProperty / TransitionSpec / CssAnimation /
// CssKeyframesRegistry types + the parse_transition_shorthand parser
// + the bridge surface (setTransition / setTransitionProperty /
// setTransitionDuration / setTransitionDelay /
// setTransitionTimingFunction / defineKeyframes / setAnimation). PRs
// 2-5 ladder on this substrate to wire frame-driven playback.

TEST_CASE("parse_transition_shorthand: single property",
          "[view][bridge][css][issue-1434-anim]") {
    auto specs = parse_transition_shorthand("opacity 200ms ease");
    REQUIRE(specs.size() == 1);
    REQUIRE(specs[0].property_name == "opacity");
    REQUIRE(specs[0].property == AnimatableProperty::opacity);
    REQUIRE_THAT(specs[0].duration_seconds, WithinAbs(0.2f, 0.001f));
    REQUIRE(specs[0].easing.kind == CssEasing::Kind::ease);
}

TEST_CASE("parse_transition_shorthand: comma-separated entries",
          "[view][bridge][css][issue-1434-anim]") {
    auto specs = parse_transition_shorthand(
        "opacity 200ms ease, transform 300ms ease-in 100ms");
    REQUIRE(specs.size() == 2);
    REQUIRE(specs[0].property_name == "opacity");
    REQUIRE_THAT(specs[0].duration_seconds, WithinAbs(0.2f, 0.001f));
    REQUIRE_THAT(specs[0].delay_seconds, WithinAbs(0.0f, 0.001f));
    REQUIRE(specs[1].property_name == "transform");
    REQUIRE_THAT(specs[1].duration_seconds, WithinAbs(0.3f, 0.001f));
    REQUIRE_THAT(specs[1].delay_seconds, WithinAbs(0.1f, 0.001f));
    REQUIRE(specs[1].easing.kind == CssEasing::Kind::ease_in);
}

TEST_CASE("parse_transition_shorthand: cubic-bezier(...)",
          "[view][bridge][css][issue-1434-anim]") {
    auto specs = parse_transition_shorthand(
        "transform 1s cubic-bezier(0.42, 0, 0.58, 1)");
    REQUIRE(specs.size() == 1);
    REQUIRE(specs[0].easing.kind == CssEasing::Kind::cubic_bezier);
    REQUIRE_THAT(specs[0].easing.p1x, WithinAbs(0.42f, 0.001f));
    REQUIRE_THAT(specs[0].easing.p1y, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(specs[0].easing.p2x, WithinAbs(0.58f, 0.001f));
    REQUIRE_THAT(specs[0].easing.p2y, WithinAbs(1.0f, 0.001f));
}

TEST_CASE("parse_transition_shorthand: steps(N, end|start)",
          "[view][bridge][css][issue-1434-anim]") {
    auto a = parse_transition_shorthand("opacity 1s steps(4, end)");
    REQUIRE(a[0].easing.kind == CssEasing::Kind::steps_end);
    REQUIRE(a[0].easing.steps_count == 4);
    auto b = parse_transition_shorthand("opacity 1s steps(8, jump-start)");
    REQUIRE(b[0].easing.kind == CssEasing::Kind::steps_start);
    REQUIRE(b[0].easing.steps_count == 8);
}

TEST_CASE("parse_transition_shorthand: none clears the list",
          "[view][bridge][css][issue-1434-anim]") {
    auto specs = parse_transition_shorthand("none");
    REQUIRE(specs.empty());
}

TEST_CASE("CssEasing endpoints: at(0)=0, at(1)=1",
          "[view][bridge][css][issue-1434-anim]") {
    CssEasing e;
    REQUIRE_THAT(e.at(0.0f), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(e.at(1.0f), WithinAbs(1.0f, 0.001f));
    e = CssEasing::from_keyword("ease-in-out");
    REQUIRE_THAT(e.at(0.0f), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(e.at(1.0f), WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(e.at(0.5f), WithinAbs(0.5f, 0.01f));  // symmetric at midpoint
}

TEST_CASE("CssAnimation::tick advances and finishes",
          "[view][bridge][css][issue-1434-anim]") {
    CssAnimation a{};
    a.spec.duration_seconds = 1.0f;
    a.spec.delay_seconds = 0.0f;
    a.spec.easing.kind = CssEasing::Kind::linear;
    a.start_value = 0.0f;
    a.end_value = 100.0f;
    REQUIRE(a.active);
    REQUIRE_THAT(a.tick(0.5f), WithinAbs(50.0f, 0.001f));
    REQUIRE(a.active);
    REQUIRE_THAT(a.tick(0.5f), WithinAbs(100.0f, 0.001f));
    REQUIRE(!a.active);
}

TEST_CASE("setTransition stores TransitionSpecs on the View",
          "[view][bridge][css][issue-1434-anim]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setTransition('a', 'opacity 200ms ease, transform 300ms ease-in 100ms');
    )");
    const auto& ts = bridge.widget("a")->transitions();
    REQUIRE(ts.size() == 2);
    REQUIRE(ts[0].property_name == "opacity");
    REQUIRE(ts[1].property_name == "transform");
}

TEST_CASE("setTransition('none') clears the list",
          "[view][bridge][css][issue-1434-anim]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setTransition('a', 'opacity 200ms ease');
        setTransition('a', 'none');
    )");
    REQUIRE(bridge.widget("a")->transitions().empty());
}

TEST_CASE("View::find_transition_for matches 'all' as fallback",
          "[view][bridge][css][issue-1434-anim]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setTransition('a', 'all 500ms');
    )");
    const auto* m = bridge.widget("a")->find_transition_for("opacity");
    REQUIRE(m != nullptr);
    REQUIRE(m->property_name == "all");
    REQUIRE_THAT(m->duration_seconds, WithinAbs(0.5f, 0.001f));
}

TEST_CASE("defineKeyframes populates the registry",
          "[view][bridge][css][issue-1434-anim]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        defineKeyframes('fade', JSON.stringify([
            { offset: 0,   properties: { opacity: '0' } },
            { offset: 1.0, properties: { opacity: '1' } }
        ]));
    )");
    const auto* block = bridge.css_keyframes_registry().find("fade");
    REQUIRE(block != nullptr);
    REQUIRE(block->name == "fade");
    REQUIRE(block->stops.size() == 2);
    REQUIRE_THAT(block->stops[0].offset, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(block->stops[1].offset, WithinAbs(1.0f, 0.001f));
}

TEST_CASE("setAnimation seeds Animation from registry",
          "[view][bridge][css][issue-1434-anim]") {
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
        setAnimation('a', 'fade', 0.4, 1, 'normal');
    )");
    const auto& anims = bridge.widget("a")->active_animations();
    REQUIRE(anims.size() == 1);
    REQUIRE(anims[0].property == AnimatableProperty::opacity);
    REQUIRE_THAT(anims[0].spec.duration_seconds, WithinAbs(0.4f, 0.001f));
}

// pulp #1508 Codex audit (P1 #1) — legacy control-token ABI.
// `setAnimation(id, "name", animName)` is the path
// web-compat-style-decl.js takes for `style.animationName = ...` and
// for the `animation:` shorthand. Pre-fix the new positional handler
// greedily took arg1 as anim_name, so the registry lookup missed on
// the literal control token "name" and nothing was seeded.
TEST_CASE("setAnimation legacy control-token: name resolves against registry",
          "[view][bridge][css][issue-1434-anim]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        defineKeyframes('fade-in', JSON.stringify([
            { offset: 0,   properties: { opacity: '0' } },
            { offset: 1.0, properties: { opacity: '1' } }
        ]));
        createPanel('a', '');
        // web-compat-style-decl.js path — one control token at a time.
        setAnimation('a', 'duration', 0.25);
        setAnimation('a', 'easing', 'ease-in-out');
        setAnimation('a', 'name', 'fade-in');
    )");
    auto* v = bridge.widget("a");
    REQUIRE(v != nullptr);
    REQUIRE(v->staged_animation().name == "fade-in");
    REQUIRE_THAT(v->staged_animation().duration_seconds, WithinAbs(0.25f, 0.001f));
    // Registry resolution happens at `name` arrival; the staged
    // duration / easing flow into the seeded Animation.
    const auto& anims = v->active_animations();
    REQUIRE(anims.size() == 1);
    REQUIRE(anims[0].property == AnimatableProperty::opacity);
    REQUIRE_THAT(anims[0].spec.duration_seconds, WithinAbs(0.25f, 0.001f));
    REQUIRE(anims[0].spec.easing.kind == CssEasing::Kind::ease_in_out);
}

// pulp #1508 Codex audit (P1 #1) — legacy control-token form must not
// look up "duration" in the keyframes registry. Pre-fix this would
// silently no-op; post-fix it stages duration on the View.
TEST_CASE("setAnimation legacy control-token: duration stages without registry hit",
          "[view][bridge][css][issue-1434-anim]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        setAnimation('a', 'duration', 0.5);
    )");
    auto* v = bridge.widget("a");
    REQUIRE(v != nullptr);
    REQUIRE_THAT(v->staged_animation().duration_seconds, WithinAbs(0.5f, 0.001f));
    // No keyframes registered, no animation seeded, but no failure.
    REQUIRE(v->active_animations().empty());
}

// pulp #1508 Codex audit (P2) — TransitionSpec default easing must be
// CSS `ease`, not `linear`. CSS spec for transition-timing-function
// defaults to `ease`; declarations like `transition: opacity 200ms`
// (no explicit easing token) must inherit that default.
TEST_CASE("TransitionSpec default-constructed easing == CSS ease",
          "[view][bridge][css][issue-1434-anim]") {
    TransitionSpec spec{};
    REQUIRE(spec.easing.kind == CssEasing::Kind::ease);
    CssEasing default_easing{};
    REQUIRE(default_easing.kind == CssEasing::Kind::ease);
    // And the parse path with no explicit easing token preserves the default.
    auto specs = parse_transition_shorthand("opacity 200ms");
    REQUIRE(specs.size() == 1);
    REQUIRE(specs[0].easing.kind == CssEasing::Kind::ease);
}

// pulp #1434 Phase A2-5 — fontFamily accepts a CSS comma-separated
// list and picks the first non-empty family. Outer quotes (single or
// double) are stripped per CSS spec. Whitespace is trimmed.
TEST_CASE("setFontFamily parses comma-separated list and strips quotes",
          "[view][bridge][css][issue-1434-fontfamily]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('t1', 'a');
        createLabel('t2', 'b');
        createLabel('t3', 'c');
        createLabel('t4', 'd');
        setFontFamily('t1', 'Inter Tight, system-ui, sans-serif');
        setFontFamily('t2', '"JetBrains Mono", Menlo');
        setFontFamily('t3', "'Helvetica Neue', Arial");
        setFontFamily('t4', '   ,  Roboto  , Arial');
    )");

    auto* l1 = dynamic_cast<Label*>(bridge.widget("t1"));
    auto* l2 = dynamic_cast<Label*>(bridge.widget("t2"));
    auto* l3 = dynamic_cast<Label*>(bridge.widget("t3"));
    auto* l4 = dynamic_cast<Label*>(bridge.widget("t4"));
    REQUIRE(l1); REQUIRE(l2); REQUIRE(l3); REQUIRE(l4);
    // pulp #1737 (#932 followup): bridge now stores the CSS comma-list
    // verbatim — SkiaCanvas resolution walks the whole list at paint
    // time. Pre-fix the bridge stripped to the first family; tests
    // were assert against that legacy behaviour. Updated to reflect
    // the new contract: Label.font_family() carries the full list.
    REQUIRE(l1->font_family() == "Inter Tight, system-ui, sans-serif");
    REQUIRE(l2->font_family() == "\"JetBrains Mono\", Menlo");
    REQUIRE(l3->font_family() == "'Helvetica Neue', Arial");
    REQUIRE(l4->font_family() == "   ,  Roboto  , Arial");
}

// pulp #1434 Phase A2-5 — when fontFamily is set on a non-Label
// container View, the value lands in the inheritable_font_family_
// slot so child Labels can read it via the parent walk. Mirrors the
// existing letter_spacing / font_weight cascade pattern.
TEST_CASE("setFontFamily on container View populates inheritable slot",
          "[view][bridge][css][issue-1434-fontfamily]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('p', '');
        setFontFamily('p', '"Custom Display", sans-serif');
    )");

    auto* panel = bridge.widget("p");
    REQUIRE(panel != nullptr);
    auto inh = panel->inheritable_font_family();
    REQUIRE(inh.has_value());
    // pulp #1737 (#932 followup): inheritable slot now carries the
    // full CSS comma-list verbatim (was stripped to first family
    // pre-fix). SkiaCanvas resolution walks the list at paint time.
    REQUIRE(*inh == "\"Custom Display\", sans-serif");
}

// pulp #1434 Phase A2-5 — CSS shim el.style.fontFamily forwards the
// comma-separated list straight through to the bridge fn, where the
// list-parsing happens. Verifies the @pulp/react CSS shim wires the
// new prop without dropping it.
TEST_CASE("CSSStyleDeclaration forwards font-family to bridge",
          "[view][bridge][css][issue-1434-fontfamily]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('lbl', 'hi');
        var s = new CSSStyleDeclaration({ _id: 'lbl', _nativeCreated: true });
        s._applyProperty('fontFamily', '"Atkinson Hyperlegible", Georgia, serif');
    )");

    auto* lbl = dynamic_cast<Label*>(bridge.widget("lbl"));
    REQUIRE(lbl != nullptr);
    REQUIRE(lbl->font_family() == "\"Atkinson Hyperlegible\", Georgia, serif");
}

