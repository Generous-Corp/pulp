// WidgetBridge Yoga-layer tests, three coherent clusters from compat.json:
//
//   1. pulp #1434 (rn batch C) — dimension percent strings
//      CSS width / height / minWidth / minHeight / maxWidth / maxHeight
//      "NN%" strings propagate through the JS translator and bridge
//      to Yoga's percent API.
//
//   2. pulp #1545 — yoga/flexBasis% catalog promotion
//      flexBasis "NN%" strings reach Yoga's flex-basis-percent slot;
//      shorthand "flex: 1 1 50%" decomposes basis correctly.
//
//   3. pulp #1434 (rn batch B) — yoga value-aliasing
//      RN-style flexDirection / justifyContent / alignItems / alignSelf
//      / order / flexWrap value aliases translate to the canonical
//      Yoga enum values.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>
#include <thread>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

// ── pulp #1434 (rn batch C) — dimension percent strings ─────────────────────
//
// `min_width`/`min_height`/`max_width`/`max_height`/`flex_basis` accept
// either a number (px) or a percentage string (`'50%'`). `flex_basis`
// also accepts `'auto'`. Yoga's `YGNodeStyleSet*Percent` /
// `YGNodeStyleSetFlexBasisAuto` APIs are dispatched on
// `FlexStyle::dim_*.unit` in `yoga_layout.cpp`.

TEST_CASE("setFlex min/max width/height accept percent strings",
          "[view][bridge][css][issue-1434-rn-batch-c]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        setFlex('child', 'min_width', '25%');
        setFlex('child', 'min_height', '15%');
        setFlex('child', 'max_width', '75%');
        setFlex('child', 'max_height', '90%');
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);
    const auto& f = child->flex();

    REQUIRE(f.dim_min_width.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_min_width.value, WithinAbs(25.0f, 0.001f));
    REQUIRE(f.dim_min_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_min_height.value, WithinAbs(15.0f, 0.001f));
    REQUIRE(f.dim_max_width.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_max_width.value, WithinAbs(75.0f, 0.001f));
    REQUIRE(f.dim_max_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(f.dim_max_height.value, WithinAbs(90.0f, 0.001f));
}

// Viewport units stay symbolic in FlexStyle and resolve from the current root
// bounds during every layout. This is essential for imported plugin editors:
// the host can resize the viewport after the React tree was materialized.
TEST_CASE("setFlex height accepts px, %, viewport units, and auto",
          "[view][bridge][css][issue-1712][rn-height]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('px',  '');
        createPanel('pct', '');
        createPanel('aut', '');
        createPanel('view', '');
        setFlex('px',  'height', 120);
        setFlex('pct', 'height', '50%');
        setFlex('view', 'height', '25vh');
        setFlex('aut', 'height', 'auto');
    )");

    auto* px = bridge.widget("px");
    auto* pct = bridge.widget("pct");
    auto* aut = bridge.widget("aut");
    auto* view = bridge.widget("view");
    REQUIRE(px != nullptr);
    REQUIRE(pct != nullptr);
    REQUIRE(aut != nullptr);
    REQUIRE(view != nullptr);

    REQUIRE(px->flex().dim_height.unit == DimensionUnit::px);
    REQUIRE_THAT(px->flex().dim_height.value, WithinAbs(120.0f, 0.001f));

    REQUIRE(pct->flex().dim_height.unit == DimensionUnit::percent);
    REQUIRE_THAT(pct->flex().dim_height.value, WithinAbs(50.0f, 0.001f));

    REQUIRE(view->flex().dim_height.unit == DimensionUnit::vh);
    REQUIRE_THAT(view->flex().dim_height.value, WithinAbs(25.0f, 0.001f));

    REQUIRE(aut->flex().dim_height.unit == DimensionUnit::auto_);
}

TEST_CASE("viewport max-height re-resolves after host resize",
          "[view][bridge][css][viewport-units][responsive]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 800, 600});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('modal', '');
        setFlex('modal', 'height', 1000);
        setFlex('modal', 'max_height', '90vh');
    )");

    auto* modal = bridge.widget("modal");
    REQUIRE(modal != nullptr);
    REQUIRE(modal->flex().dim_max_height.unit == DimensionUnit::vh);
    REQUIRE_THAT(modal->flex().dim_max_height.value, WithinAbs(90.0f, 0.001f));

    root.layout_children();
    REQUIRE_THAT(modal->bounds().height, WithinAbs(540.0f, 1.0f));

    root.set_bounds({0, 0, 800, 400});
    root.layout_children();
    REQUIRE_THAT(modal->bounds().height, WithinAbs(360.0f, 1.0f));
}

TEST_CASE("setFlex dimensions reject finite doubles that overflow float",
          "[view][bridge][css][finite]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createPanel('bounded-dimension', '')");
    auto* child = bridge.widget("bounded-dimension");
    REQUIRE(child != nullptr);
    child->flex().preferred_width = 41.0f;
    child->flex().dim_width = {41.0f, DimensionUnit::px};

    bridge.load_script("setFlex('bounded-dimension', 'width', 1e100)");
    CHECK(child->flex().preferred_width == 41.0f);
    CHECK(child->flex().dim_width.value == 41.0f);

    bridge.load_script("setFlex('bounded-dimension', 'width', '1e100px')");
    CHECK(child->flex().preferred_width == 41.0f);
    CHECK(child->flex().dim_width.value == 41.0f);
}

TEST_CASE("setFlex min/max width/height numeric path stays px",
          "[view][bridge][css][issue-1434-rn-batch-c]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        setFlex('child', 'min_width', 50);
        setFlex('child', 'min_height', 30);
        setFlex('child', 'max_width', 200);
        setFlex('child', 'max_height', 150);
    )");

    const auto& f = bridge.widget("child")->flex();
    REQUIRE(f.dim_min_width.unit == DimensionUnit::px);
    REQUIRE_THAT(f.min_width, WithinAbs(50.0f, 0.001f));
    REQUIRE_THAT(f.min_height, WithinAbs(30.0f, 0.001f));
    REQUIRE_THAT(f.max_width, WithinAbs(200.0f, 0.001f));
    REQUIRE_THAT(f.max_height, WithinAbs(150.0f, 0.001f));
}

TEST_CASE("setFlex flex_basis accepts 'auto', percent string, and number",
          "[view][bridge][css][issue-1434-rn-batch-c]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a', 'flex_basis', 'auto');
        createPanel('b','');  setFlex('b', 'flex_basis', '40%');
        createPanel('c','');  setFlex('c', 'flex_basis', 80);
    )");

    const auto& fa = bridge.widget("a")->flex();
    REQUIRE(fa.dim_flex_basis.unit == DimensionUnit::auto_);

    const auto& fb = bridge.widget("b")->flex();
    REQUIRE(fb.dim_flex_basis.unit == DimensionUnit::percent);
    REQUIRE_THAT(fb.dim_flex_basis.value, WithinAbs(40.0f, 0.001f));

    const auto& fc = bridge.widget("c")->flex();
    REQUIRE(fc.dim_flex_basis.unit == DimensionUnit::px);
    REQUIRE_THAT(fc.flex_basis, WithinAbs(80.0f, 0.001f));
}

TEST_CASE("max_width percent caps the child at the resolved pixel size",
          "[view][bridge][css][issue-1434-rn-batch-c]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('child', '');
        setFlex('child', 'max_width', '50%');
        setFlex('child', 'flex_grow', 1);
    )");

    auto* child = bridge.widget("child");
    REQUIRE(child != nullptr);

    root.layout_children();
    REQUIRE(child->bounds().width <= 200.5f);
}

// ── pulp #1545 — yoga/flexBasis% catalog promotion (partial → supported) ────
//
// The setFlex flex_basis path was added in pulp #1434 rn batch C; the
// catalog entry stayed at "partial" until it could be re-verified that
// YGNodeStyleSetFlexBasisPercent actually drives layout end-to-end (not
// just stamps the FlexStyle field). This test asserts that:
//   1. A 50% flex_basis on a single child of a row-direction parent
//      resolves to half the parent's width after layout.
//   2. 'auto' flex_basis collapses to the child's intrinsic / zero-px
//      basis (not a percent of parent), so siblings can share the line.
// Together these confirm the dispatch chain
// (bridge → FlexStyle::dim_flex_basis.unit → yoga_layout.cpp dispatch →
// YGNodeStyleSetFlexBasis{Percent,Auto}) is wired correctly.

TEST_CASE("flex_basis percent resolves against parent main-axis size",
          "[view][bridge][css][issue-1545]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 100});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        setFlex('', 'direction', 'row');
        createPanel('a', '');
        setFlex('a', 'flex_basis', '50%');
        setFlex('a', 'flex_grow', 0);
        setFlex('a', 'flex_shrink', 0);
    )");

    auto* a = bridge.widget("a");
    REQUIRE(a != nullptr);

    root.layout_children();

    // 50% of the 400px parent main-axis = 200px.
    REQUIRE_THAT(a->bounds().width, WithinAbs(200.0f, 0.5f));
}

TEST_CASE("flex_basis 'auto' does not consume parent main-axis as a percent",
          "[view][bridge][css][issue-1545]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 100});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        setFlex('', 'direction', 'row');
        createPanel('a', '');
        setFlex('a', 'flex_basis', 'auto');
        setFlex('a', 'flex_grow', 1);
        createPanel('b', '');
        setFlex('b', 'flex_basis', 'auto');
        setFlex('b', 'flex_grow', 1);
    )");

    auto* a = bridge.widget("a");
    auto* b = bridge.widget("b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    root.layout_children();

    // With auto basis + equal flex_grow, the 400px main axis splits
    // evenly between two siblings. If 'auto' had been mis-dispatched as
    // a percent, one child would have eaten the full main axis.
    REQUIRE_THAT(a->bounds().width, WithinAbs(200.0f, 1.0f));
    REQUIRE_THAT(b->bounds().width, WithinAbs(200.0f, 1.0f));
}

// ── pulp #1434 (rn batch B) — yoga value-aliasing ───────────────────────────
//
// The bridge's setFlex value mapper now accepts the CSS / RN canonical
// spellings (`flex-start` / `flex-end` for align*+justify; `column` /
// `row-reverse` / `column-reverse` for direction) alongside the Yoga /
// pulp short forms. The CSS shim's `_cssToFlex` already mapped the
// prefixed forms to bare ones for the CSS path, but @pulp/react's
// prop-applier passes RN values through verbatim — so bridge-side
// acceptance is the cross-surface fix.

TEST_CASE("setFlex direction accepts row / row-reverse / column / column-reverse / col",
          "[view][bridge][css][issue-1434-rn-batch-b]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        createPanel('b', '');
        createPanel('c', '');
        createPanel('d', '');
        createPanel('e', '');
        setFlex('a', 'direction', 'row');
        setFlex('b', 'direction', 'row-reverse');
        setFlex('c', 'direction', 'column');
        setFlex('d', 'direction', 'column-reverse');
        setFlex('e', 'direction', 'col');
    )");

    auto get_dir = [&](const std::string& id) {
        return bridge.widget(id)->flex().direction;
    };

    REQUIRE(get_dir("a") == FlexDirection::row);
    REQUIRE(get_dir("b") == FlexDirection::row_reverse);
    REQUIRE(get_dir("c") == FlexDirection::column);
    REQUIRE(get_dir("d") == FlexDirection::column_reverse);
    REQUIRE(get_dir("e") == FlexDirection::column);  // legacy 'col' alias
}

TEST_CASE("setFlex align_items accepts start / flex-start / end / flex-end / center / stretch",
          "[view][bridge][css][issue-1434-rn-batch-b]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','align_items','start');
        createPanel('b','');  setFlex('b','align_items','flex-start');
        createPanel('c','');  setFlex('c','align_items','end');
        createPanel('d','');  setFlex('d','align_items','flex-end');
        createPanel('e','');  setFlex('e','align_items','center');
        createPanel('f','');  setFlex('f','align_items','stretch');
    )");

    auto al = [&](const std::string& id) { return bridge.widget(id)->flex().align_items; };
    REQUIRE(al("a") == FlexAlign::start);
    REQUIRE(al("b") == FlexAlign::start);
    REQUIRE(al("c") == FlexAlign::end);
    REQUIRE(al("d") == FlexAlign::end);
    REQUIRE(al("e") == FlexAlign::center);
    REQUIRE(al("f") == FlexAlign::stretch);
}

TEST_CASE("setFlex align_self accepts the alias set",
          "[view][bridge][css][issue-1434-rn-batch-b]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','align_self','start');
        createPanel('b','');  setFlex('b','align_self','flex-start');
        createPanel('c','');  setFlex('c','align_self','end');
        createPanel('d','');  setFlex('d','align_self','flex-end');
        createPanel('e','');  setFlex('e','align_self','auto');
    )");
    auto sl = [&](const std::string& id) { return bridge.widget(id)->flex().align_self; };
    REQUIRE(sl("a") == FlexAlign::start);
    REQUIRE(sl("b") == FlexAlign::start);
    REQUIRE(sl("c") == FlexAlign::end);
    REQUIRE(sl("d") == FlexAlign::end);
    REQUIRE(sl("e") == FlexAlign::auto_);
}

TEST_CASE("setFlex justify_content accepts the alias set",
          "[view][bridge][css][issue-1434-rn-batch-b]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  setFlex('a','justify_content','start');
        createPanel('b','');  setFlex('b','justify_content','flex-start');
        createPanel('c','');  setFlex('c','justify_content','end');
        createPanel('d','');  setFlex('d','justify_content','flex-end');
        createPanel('e','');  setFlex('e','justify_content','center');
        createPanel('f','');  setFlex('f','justify_content','space-between');
    )");
    auto jc = [&](const std::string& id) { return bridge.widget(id)->flex().justify_content; };
    REQUIRE(jc("a") == FlexJustify::start);
    REQUIRE(jc("b") == FlexJustify::start);
    REQUIRE(jc("c") == FlexJustify::end_);
    REQUIRE(jc("d") == FlexJustify::end_);
    REQUIRE(jc("e") == FlexJustify::center);
    REQUIRE(jc("f") == FlexJustify::space_between);
}

// pulp #1434 Tier 1 (css/alignItems) — `first baseline` is a CSS-spec
// alternate form of `baseline`. The baseline-set "first" selector is
// the default Yoga `YGAlignBaseline` already computes, so collapsing it
// is observable-behavior-preserving.
//
// `last baseline` is NOT aliased because Yoga has no last-baseline
// support — aliasing would silently misrender multi-line flex
// containers that depend on bottom-baseline alignment.
// `last baseline` stays in compat.json/unsupportedValues with a note.
// This test pins both the supported alias AND the "last baseline"
// fall-through (becomes FlexAlign::stretch via the default branch) so
// a future change that re-introduces the silent alias fails first.
TEST_CASE("setFlex align_items: first baseline aliases to baseline; last baseline stays unsupported",
          "[view][bridge][css][issue-1434]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');  setFlex('a', 'align_items', 'baseline');
        createPanel('b', '');  setFlex('b', 'align_items', 'first baseline');
        createPanel('c', '');  setFlex('c', 'align_items', 'last baseline');
    )");

    auto al = [&](const std::string& id) { return bridge.widget(id)->flex().align_items; };
    REQUIRE(al("a") == FlexAlign::baseline);
    REQUIRE(al("b") == FlexAlign::baseline);
    // `last baseline` falls through to the default (stretch) — it is
    // documented unsupported. Asserting `!= baseline` is the contract:
    // we do NOT silently alias to baseline.
    REQUIRE(al("c") != FlexAlign::baseline);
    REQUIRE(al("c") == FlexAlign::stretch);
}

// pulp #1434 (css/justifyContent) — direction-dependent keywords are
// documented unsupported; this leaves only the safe sanity pins:
//
//   `left` / `right`  — direction-context-dependent. CSS spec: on a row
//                       container, `right` ≡ flex-end (LTR); on a column
//                       container, BOTH `left` and `right` behave as
//                       `start`. A direction-agnostic alias would
//                       silently misrender vertical flex containers.
//                       Documented unsupported until direction-aware
//                       aliasing lands.
//   `stretch`         — grows AUTO-sized items equally; FlexJustify has
//                       no equivalent. Documented unsupported.
//   `normal`          — per spec, "behaves as `stretch`" on flex
//                       containers. Documented unsupported.
//
// All four fall through to the dispatcher's default branch, which sets
// `FlexJustify::start`. The test pins that contract so any future
// re-introduction of a silent alias fails first.
TEST_CASE("setFlex justify_content: left/right/stretch/normal stay unsupported (direction-dep + arch)",
          "[view][bridge][css][issue-1434]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');  setFlex('a', 'justify_content', 'left');
        createPanel('b', '');  setFlex('b', 'justify_content', 'right');
        createPanel('c', '');  setFlex('c', 'justify_content', 'stretch');
        createPanel('d', '');  setFlex('d', 'justify_content', 'normal');
        // Sanity-pin: existing aliases still resolve to the same enum
        // value so we don't accidentally weaken the spec mapping.
        createPanel('e', '');  setFlex('e', 'justify_content', 'flex-start');
        createPanel('f', '');  setFlex('f', 'justify_content', 'flex-end');
    )");

    auto jc = [&](const std::string& id) { return bridge.widget(id)->flex().justify_content; };
    // All four direction-dep / arch-unsupported values fall through to
    // the safe default (start). Asserting equality with the default AND
    // inequality with the previously-claimed end_ direction would have
    // caught the original silent overclaim.
    REQUIRE(jc("a") == FlexJustify::start);
    REQUIRE(jc("b") == FlexJustify::start);
    REQUIRE(jc("b") != FlexJustify::end_);
    REQUIRE(jc("c") == FlexJustify::start);
    REQUIRE(jc("d") == FlexJustify::start);
    REQUIRE(jc("e") == FlexJustify::start);        // sanity: existing alias unchanged
    REQUIRE(jc("f") == FlexJustify::end_);         // sanity: existing alias unchanged
}

TEST_CASE("CSSStyleDeclaration forwards flex-direction reverse modes verbatim",
          "[view][bridge][css][issue-1434-rn-batch-b]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a','');  createPanel('b','');
        var sa = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        var sb = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        sa._applyProperty('flexDirection', 'row-reverse');
        sb._applyProperty('flexDirection', 'column-reverse');
    )");

    REQUIRE(bridge.widget("a")->flex().direction == FlexDirection::row_reverse);
    REQUIRE(bridge.widget("b")->flex().direction == FlexDirection::column_reverse);
}

// ── fix/knob-ring-offcenter — sub-pixel layout opt-in ───────────────────────
//
// Yoga's default pointScaleFactor of 1.0 rounds every solved box to the
// whole-pixel grid. A replayed Figma design arrives with SOLVED fractional
// geometry, and per-node rounding moves siblings RELATIVE to each other:
// the knob-body ELLIPSE at (7.51, 7.51, 22.53²) rounded to (8, 8, 23²)
// while its value-ring arc's vector box kept 2-decimal placement — every
// ring in "A Channel FX" rendered visibly off-center of its body.
// setSubpixelLayout opts the whole pass out of the pixel grid so both
// siblings keep the centers Figma solved.

TEST_CASE("default layout keeps Yoga's whole-pixel grid rounding",
          "[view][bridge][layout][subpixel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 100, 100});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('body', '');
        setPosition('body', 'absolute');
        setLeft('body', 7.51); setTop('body', 7.51);
        setFlex('body', 'width', 22.53); setFlex('body', 'height', 22.53);
    )");
    root.layout_children();

    auto b = bridge.widget("body")->bounds();
    // Stock behavior unchanged: edges land on the whole-pixel grid.
    REQUIRE(b.x == std::round(b.x));
    REQUIRE(b.y == std::round(b.y));
    REQUIRE(b.width == std::round(b.width));
    REQUIRE(b.height == std::round(b.height));
}

TEST_CASE("setSubpixelLayout preserves fractional solved geometry pass-wide",
          "[view][bridge][layout][subpixel]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 100, 100});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // The imported-design shape: the opt-in rides the design root ('' = the
    // bridge's host root here), the body ellipse box and the ring's vector
    // box are concentric only at sub-pixel precision (centers 18.775 vs
    // 18.775 — but rounded boxes give 19.5 vs 18.775).
    bridge.load_script(R"(
        setSubpixelLayout('', true);
        createPanel('body', '');
        setPosition('body', 'absolute');
        setLeft('body', 7.51); setTop('body', 7.51);
        setFlex('body', 'width', 22.53); setFlex('body', 'height', 22.53);
        createPanel('ring', '');
        setPosition('ring', 'absolute');
        setLeft('ring', 1.88); setTop('ring', 1.88);
        setFlex('ring', 'width', 33.8); setFlex('ring', 'height', 33.8);
    )");
    root.layout_children();

    auto body = bridge.widget("body")->bounds();
    REQUIRE_THAT(body.x, WithinAbs(7.51f, 0.001f));
    REQUIRE_THAT(body.y, WithinAbs(7.51f, 0.001f));
    REQUIRE_THAT(body.width, WithinAbs(22.53f, 0.001f));
    REQUIRE_THAT(body.height, WithinAbs(22.53f, 0.001f));

    auto ring = bridge.widget("ring")->bounds();
    REQUIRE_THAT(ring.x, WithinAbs(1.88f, 0.001f));
    REQUIRE_THAT(ring.width, WithinAbs(33.8f, 0.001f));

    // The regression's observable: both boxes share one center.
    const float body_cx = body.x + body.width / 2;
    const float ring_cx = ring.x + ring.width / 2;
    REQUIRE_THAT(body_cx, WithinAbs(ring_cx, 0.005f));
}

// ── Absolutely positioned auto-width boxes shrink to fit their text ─────────
//
// A tooltip-shaped box -- `position: absolute`, a `left`, padding, a border,
// no width -- holding one `<span>` of text must take the text's width plus its
// padding and border, as a browser lays it out (shrink-to-fit, CSS 2.2
// §10.3.7). @pulp/react lowers the span to a Label with `white-space: normal`
// (CSS's initial value) while the `nowrap` sits on the div. `white-space` is
// inherited, so the span stays on one line; natively the Label ignored its
// ancestor and soft-wrapped inside the room its containing block offered,
// collapsing the box (in Spectr's header, to padding + border alone, with the
// text painting outside it). The bridge calls below are the ones @pulp/react
// makes for
//   <div style={{position:'absolute', left, top, whiteSpace:'nowrap',
//                padding:'6px 9px', border:'1px solid ...'}}><span>text</span></div>
TEST_CASE("an absolutely positioned auto-width box shrink-fits the nowrap text it inherits",
          "[view][bridge][css][layout][absolute]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 800, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"js(
        createRow('row', '');
        setFlex('row', 'width', 800); setFlex('row', 'height', 40);
        createPanel('knob', 'row');
        setFlex('knob', 'width', 30); setFlex('knob', 'height', 30);
        // Narrower than the tip's own left offset, as a header cluster is.
        createPanel('cluster', 'row');
        setPosition('cluster', 'relative');
        setFlex('cluster', 'width', 60); setFlex('cluster', 'height', 30);
        createPanel('tip', 'cluster');
        setPosition('tip', 'absolute');
        setLeft('tip', 80); setTop('tip', 32);
        setWhiteSpace('tip', 'nowrap');
        setFlex('tip', 'padding_top', 6); setFlex('tip', 'padding_bottom', 6);
        setFlex('tip', 'padding_left', 9); setFlex('tip', 'padding_right', 9);
        setBorder('tip', 'rgba(255,255,255,0.12)', 1, 4);
        createLabel('tip-text', 'Intensity: how strong the effect is. 0% is flat.', 'tip');
        setWhiteSpace('tip-text', 'normal');
        setFontSize('tip-text', 10);
    )js");
    root.layout_children();
    auto* tip = bridge.widget("tip");
    auto* text = dynamic_cast<Label*>(bridge.widget("tip-text"));
    REQUIRE(tip != nullptr);
    REQUIRE(text != nullptr);
    const float natural = text->max_content_width();
    INFO("tip " << tip->bounds().width << "x" << tip->bounds().height
         << " text " << text->bounds().width << " natural " << natural);
    // Control: the text has a real natural width, so a zero box is a defect,
    // not an empty string.
    REQUIRE(natural > 100.0f);
    // The text lays out on one line at its natural width...
    CHECK(text->bounds().width == Catch::Approx(natural).margin(1.0f));
    // ...and the box wraps it: text + 2 x 9 padding + 2 x 1 border.
    CHECK(tip->bounds().width == Catch::Approx(natural + 20.0f).margin(1.0f));
    CHECK(tip->bounds().x == Catch::Approx(80.0f).margin(0.5f));
}

TEST_CASE("white-space nowrap reaches descendant text, and a Label's own mode wins",
          "[view][bridge][css][layout][white-space]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"js(
        createCol('box', '');
        setFlex('box', 'width', 80);
        createLabel('inherits', 'one two three four five six seven', 'box');
        setWhiteSpace('inherits', 'normal');
        createLabel('own', 'one two three four five six seven', 'box');
        setWhiteSpace('own', 'pre-wrap');
    )js");
    auto* inherits = dynamic_cast<Label*>(bridge.widget("inherits"));
    auto* own = dynamic_cast<Label*>(bridge.widget("own"));
    REQUIRE(inherits != nullptr);
    REQUIRE(own != nullptr);
    root.layout_children();
    // Control: with no nowrap anywhere both wrap inside 80 px.
    CHECK(inherits->soft_wraps());
    const float wrapped_height = inherits->bounds().height;
    CHECK(wrapped_height > inherits->intrinsic_height() * 1.5f);
    // nowrap on the container: the Label that kept CSS's initial value
    // follows it onto one line; the one that set pre-wrap itself still wraps.
    bridge.load_script("setWhiteSpace('box', 'nowrap');");
    root.layout_children();
    CHECK_FALSE(inherits->soft_wraps());
    CHECK(inherits->bounds().height < wrapped_height);
    CHECK(own->soft_wraps());
    // And back: normal on the container restores wrapping.
    bridge.load_script("setWhiteSpace('box', 'normal');");
    root.layout_children();
    CHECK(inherits->soft_wraps());
    CHECK(inherits->bounds().height == Catch::Approx(wrapped_height));
}
