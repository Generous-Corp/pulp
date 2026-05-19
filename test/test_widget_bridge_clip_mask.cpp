// test_widget_bridge_clip_mask.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// pulp #1515 — clip-path + mask cluster.
//
// Round-trips the CSS clip-path + mask family through the JS shim,
// CSS translator, and bridge into View's mask slots:
//   * clip-path: inset(NN%) / circle(NN%) / polygon(...) / url(#id)
//   * clip-path: shape coordinate parsing + reference-line tests
//   * mask-image: url + linear-gradient + linear-gradient transforms
//   * mask-size: cover / contain / explicit dimensions / percentage
//   * mask-position / mask-repeat / mask-origin / mask-clip
//   * mask-composite (add / subtract / intersect / exclude)
//   * mask shorthand decomposition

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

// ── pulp #1515: clip-path + mask cluster ──────────────────────────────────────

TEST_CASE("WidgetBridge setClipPath stores SVG-path-d on the View",
          "[view][bridge][issue-1515]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createPanel('p', '')");
    auto* panel = bridge.widget("p");
    REQUIRE(panel != nullptr);
    REQUIRE_FALSE(panel->has_clip_path());
    REQUIRE(panel->clip_path().empty());

    bridge.load_script("setClipPath('p', 'M 0 0 L 100 0 L 100 100 Z')");
    REQUIRE(panel->has_clip_path());
    REQUIRE(panel->clip_path() == "M 0 0 L 100 0 L 100 100 Z");

    // Empty string clears the slot.
    bridge.load_script("setClipPath('p', '')");
    REQUIRE_FALSE(panel->has_clip_path());
}

// pulp #1656 Tier-2 follow-up — `setUserSelect` was a literal `(void)args`
// no-op; #1656 walked the catalog claim back to `partial`. This Tier-2
// PR wires the keyword to View::user_select_ for real, flips the catalog
// back to `supported` with this test as evidence, and exercises the
// new #1657 control #1 evidence gate end-to-end (a `supported` claim
// now requires real test coverage of the bridge fn).
TEST_CASE("WidgetBridge setUserSelect routes all 5 CSS keywords to View::user_select_",
          "[view][bridge][css][issue-1656-tier2-userSelect]") {
    using US = pulp::view::View::UserSelect;
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createPanel('p', '')");
    auto* panel = bridge.widget("p");
    REQUIRE(panel != nullptr);

    // Default (unset): auto.
    REQUIRE(panel->user_select() == US::auto_);

    // Each of the 5 CSS keywords routes to the matching enum value.
    bridge.load_script("setUserSelect('p', 'none')");
    REQUIRE(panel->user_select() == US::none);
    bridge.load_script("setUserSelect('p', 'text')");
    REQUIRE(panel->user_select() == US::text);
    bridge.load_script("setUserSelect('p', 'all')");
    REQUIRE(panel->user_select() == US::all);
    bridge.load_script("setUserSelect('p', 'contain')");
    REQUIRE(panel->user_select() == US::contain);
    bridge.load_script("setUserSelect('p', 'auto')");
    REQUIRE(panel->user_select() == US::auto_);

    // Unknown keyword resets to spec default (auto).
    bridge.load_script("setUserSelect('p', 'none')");   // not auto
    REQUIRE(panel->user_select() == US::none);
    bridge.load_script("setUserSelect('p', 'wat')");
    REQUIRE(panel->user_select() == US::auto_);

    // CSSStyleDeclaration JS path also dispatches end-to-end (matches
    // the user-facing `el.style.userSelect = '...'` surface).
    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('userSelect', 'none');
    )");
    REQUIRE(panel->user_select() == US::none);
    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('userSelect', 'text');
    )");
    REQUIRE(panel->user_select() == US::text);
}

TEST_CASE("WidgetBridge setMaskImage / setMask round-trip on the View",
          "[view][bridge][issue-1515]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createPanel('p', '')");
    auto* panel = bridge.widget("p");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->mask_image().empty());
    REQUIRE(panel->mask().empty());

    bridge.load_script("setMaskImage('p', 'url(#mask-id)')");
    REQUIRE(panel->mask_image() == "url(#mask-id)");

    bridge.load_script("setMask('p', 'url(#m) repeat')");
    REQUIRE(panel->mask() == "url(#m) repeat");

    // Empty string clears.
    bridge.load_script("setMaskImage('p', '')");
    REQUIRE(panel->mask_image().empty());
}

// pulp #1515 followup — `mask-size` pairs with mask-image. Storage-only;
// the slot round-trips through View::mask_size() so authors can set/get
// it and a future paint slice can honor it without a JS-side change.
TEST_CASE("WidgetBridge setMaskSize round-trips on the View",
          "[view][bridge][css][issue-1707-followup-maskSize]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createPanel('p', '')");
    auto* panel = bridge.widget("p");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->mask_size().empty());

    // Direct bridge call.
    bridge.load_script("setMaskSize('p', 'cover')");
    REQUIRE(panel->mask_size() == "cover");

    bridge.load_script("setMaskSize('p', '50% 100%')");
    REQUIRE(panel->mask_size() == "50% 100%");

    // CSSStyleDeclaration JS path also dispatches end-to-end.
    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('maskSize', 'contain');
    )");
    REQUIRE(panel->mask_size() == "contain");
}

// CSS `appearance` — Pulp paints all widgets custom (no native form
// rendering), so this is observably storage-only. The slot exists so
// authors who set `appearance: none` for reset-style consistency see
// a no-op (not an unsupported drop) and the value round-trips.
TEST_CASE("WidgetBridge setAppearance round-trips on the View",
          "[view][bridge][css][issue-1707-followup-appearance]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createPanel('p', '')");
    auto* panel = bridge.widget("p");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->appearance().empty());

    bridge.load_script("setAppearance('p', 'none')");
    REQUIRE(panel->appearance() == "none");

    bridge.load_script("setAppearance('p', 'auto')");
    REQUIRE(panel->appearance() == "auto");

    bridge.load_script("setAppearance('p', 'button')");
    REQUIRE(panel->appearance() == "button");

    // CSSStyleDeclaration JS path — including vendor-prefixed forms.
    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('appearance', 'none');
    )");
    REQUIRE(panel->appearance() == "none");

    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('WebkitAppearance', 'textfield');
    )");
    REQUIRE(panel->appearance() == "textfield");

    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('MozAppearance', 'menulist-button');
    )");
    REQUIRE(panel->appearance() == "menulist-button");
}

// CSS `object-fit` storage round-trip (paint-time consumption is a
// planned follow-up that needs ImageView access to decoded image
// natural size; status is `partial` until paint lands).
TEST_CASE("WidgetBridge setObjectFit round-trips on the View",
          "[view][bridge][css][issue-1707-followup-objectFit]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createPanel('p', '')");
    auto* panel = bridge.widget("p");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->object_fit().empty());

    for (auto kw : {"fill", "contain", "cover", "none", "scale-down"}) {
        bridge.load_script(std::string("setObjectFit('p', '") + kw + "')");
        REQUIRE(panel->object_fit() == kw);
    }

    // CSSStyleDeclaration JS path.
    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('objectFit', 'cover');
    )");
    REQUIRE(panel->object_fit() == "cover");
}

// CSS `object-position` storage round-trip (pairs with object-fit).
TEST_CASE("WidgetBridge setObjectPosition round-trips on the View",
          "[view][bridge][css][issue-1707-followup-objectPosition]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createPanel('p', '')");
    auto* panel = bridge.widget("p");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->object_position().empty());

    bridge.load_script("setObjectPosition('p', 'center')");
    REQUIRE(panel->object_position() == "center");

    bridge.load_script("setObjectPosition('p', '50% 50%')");
    REQUIRE(panel->object_position() == "50% 50%");

    bridge.load_script("setObjectPosition('p', '10px top')");
    REQUIRE(panel->object_position() == "10px top");

    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('objectPosition', '25% 75%');
    )");
    REQUIRE(panel->object_position() == "25% 75%");
}

// CSS `grid` shorthand — JS shim parses `<rows> / <cols>` form and
// fans out to setGrid(template_rows) + setGrid(template_columns).
// Full spec is deferred; this covers the common form.
TEST_CASE("CSSStyleDeclaration grid shorthand parses <rows> / <cols> form",
          "[view][bridge][css][issue-1707-followup-grid]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createPanel('p', '')");
    auto* panel = bridge.widget("p");
    REQUIRE(panel != nullptr);

    // <rows> / <cols> common form — verifies fan-out to both axes.
    // template_columns and template_rows are vector<GridTrack>; the
    // common form here yields 2 tracks each side.
    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('grid', '100px 1fr / 50% 50%');
    )");
    REQUIRE(panel->grid().template_rows.size() == 2);
    REQUIRE(panel->grid().template_columns.size() == 2);

    // 3-track rows on a single side
    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('grid', '1fr 1fr 1fr / 100%');
    )");
    REQUIRE(panel->grid().template_rows.size() == 3);
    REQUIRE(panel->grid().template_columns.size() == 1);

    // Single-track form — falls back to template_rows only
    bridge.load_script(R"(
        var s = new CSSStyleDeclaration({ _id: 'p', _nativeCreated: true });
        s._applyProperty('grid', '200px');
    )");
    REQUIRE(panel->grid().template_rows.size() == 1);
}

TEST_CASE("View::paint_all emits clip_path_svg when clip_path is set",
          "[view][canvas][issue-1515]") {
    using namespace pulp::canvas;

    View root;
    root.set_bounds({0, 0, 200, 120});
    root.set_clip_path("M 0 0 L 100 0 L 100 100 Z");

    RecordingCanvas canvas;
    root.paint_all(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::clip_path_svg) == 1);

    bool found = false;
    for (auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::clip_path_svg) {
            REQUIRE(cmd.text == "M 0 0 L 100 0 L 100 100 Z");
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("View::paint_all skips clip_path when slot is empty",
          "[view][canvas][issue-1515]") {
    using namespace pulp::canvas;

    View root;
    root.set_bounds({0, 0, 100, 50});

    RecordingCanvas canvas;
    root.paint_all(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::clip_path_svg) == 0);
}

TEST_CASE("Canvas::clip_path_svg base default is a no-op",
          "[canvas][issue-1515]") {
    using namespace pulp::canvas;
    RecordingCanvas canvas;

    // RecordingCanvas overrides — emits the dedicated command.
    canvas.clip_path_svg("M 0 0 L 50 0 L 50 50 Z");
    REQUIRE(canvas.count(DrawCommand::Type::clip_path_svg) == 1);
}

TEST_CASE("CSSStyleDeclaration shim forwards clipPath path() form to setClipPath",
          "[view][bridge][css][issue-1515]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // path("...") → extracted SVG-path-d on the View.
    bridge.load_script(R"(
        createPanel('a', '');
        var s = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        s._applyProperty('clipPath', 'path("M 0 0 L 100 0 L 100 100 Z")');
    )");
    REQUIRE(bridge.widget("a")->clip_path() == "M 0 0 L 100 0 L 100 100 Z");

    // 'none' clears the slot.
    bridge.load_script(R"(
        var s2 = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        s2._applyProperty('clipPath', 'none');
    )");
    REQUIRE(bridge.widget("a")->clip_path().empty());

    // Deferred forms (circle / url / inset / polygon) clear the slot
    // rather than installing a partial clip — honest partial coverage.
    bridge.load_script(R"(
        createPanel('b', '');
        var s3 = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        s3._applyProperty('clipPath', 'circle(50%)');
    )");
    REQUIRE(bridge.widget("b")->clip_path().empty());
}

TEST_CASE("CSSStyleDeclaration shim forwards maskImage / mask to bridge",
          "[view][bridge][css][issue-1515]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');
        var s = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        s._applyProperty('maskImage', 'url(#mask-id)');
    )");
    REQUIRE(bridge.widget("a")->mask_image() == "url(#mask-id)");

    // 'none' clears the slot.
    bridge.load_script(R"(
        var s2 = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        s2._applyProperty('maskImage', 'none');
    )");
    REQUIRE(bridge.widget("a")->mask_image().empty());

    // mask shorthand → both shorthand stored and image extracted.
    bridge.load_script(R"(
        createPanel('b', '');
        var s3 = new CSSStyleDeclaration({ _id: 'b', _nativeCreated: true });
        s3._applyProperty('mask', 'url(#m) repeat');
    )");
    auto* b = bridge.widget("b");
    REQUIRE(b->mask() == "url(#m) repeat");
    REQUIRE(b->mask_image() == "url(#m)");
}

// pulp #1516 — setBoxSizing routes to FlexStyle.box_sizing.
TEST_CASE("setBoxSizing border-box / content-box round-trips onto FlexStyle",
          "[view][bridge][css][issue-1516]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        createPanel('b', '');
        setBoxSizing('a', 'border-box');
        setBoxSizing('b', 'content-box');
    )");
    REQUIRE(bridge.widget("a")->flex().box_sizing == BoxSizing::border_box);
    REQUIRE(bridge.widget("b")->flex().box_sizing == BoxSizing::content_box);

    // Unknown keyword falls back to content-box. The default for an
    // unset slot is border-box (matches Yoga 3.x and pulp's implicit
    // pre-#1516 behavior), but `setBoxSizing` with an explicit unknown
    // keyword resolves to content-box rather than silently keeping the
    // prior value — that way `setBoxSizing('id', 'wat')` is a clear
    // observable rather than a quiet no-op.
    bridge.load_script("setBoxSizing('a', 'wat')");
    REQUIRE(bridge.widget("a")->flex().box_sizing == BoxSizing::content_box);
}

// Codex #1616 P1 — `box-sizing: inherit` must walk the parent chain
// instead of silently coercing to content-box. Reproduces the common
// reset pattern `html { box-sizing: border-box }` + descendants
// `* { box-sizing: inherit }` that gets imported from web designs.
TEST_CASE("setBoxSizing 'inherit' resolves to parent's box_sizing",
          "[view][bridge][css][issue-1538][codex-p1]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('parent', '');
        createPanel('child', 'parent');
        createPanel('grandchild', 'child');
        setBoxSizing('parent', 'border-box');
        setBoxSizing('child', 'inherit');
        setBoxSizing('grandchild', 'inherit');
    )");
    REQUIRE(bridge.widget("parent")->flex().box_sizing == BoxSizing::border_box);
    REQUIRE(bridge.widget("child")->flex().box_sizing == BoxSizing::border_box);
    REQUIRE(bridge.widget("grandchild")->flex().box_sizing == BoxSizing::border_box);

    // inherit on a detached/root node falls back to the CSS default content-box.
    bridge.load_script("setBoxSizing('', 'inherit');");
    REQUIRE(root.flex().box_sizing == BoxSizing::content_box);
}

// pulp #1516 — CSSStyleDeclaration shim forwards camelCase boxSizing.
TEST_CASE("CSSStyleDeclaration forwards box-sizing to bridge",
          "[view][bridge][css][issue-1516]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('a', '');
        var s = new CSSStyleDeclaration({ _id: 'a', _nativeCreated: true });
        s.boxSizing = 'border-box';
    )");
    REQUIRE(bridge.widget("a")->flex().box_sizing == BoxSizing::border_box);
}

// pulp #1516 — load-bearing test. Under border-box (pulp default),
// declared width=100 + padding=10 yields outer-bounds width=100
// (content area shrinks). Under content-box (CSS spec default), the
// same declaration produces outer width=120 (padding adds outside).
// Yoga 3.x's YGNodeStyleSetBoxSizing does the math.
TEST_CASE("border-box vs content-box layout math via Yoga",
          "[view][bridge][css][issue-1516]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('bb', '');
        createPanel('cb', '');
        setFlex('bb', 'width',  100);
        setFlex('bb', 'height', 100);
        setFlex('bb', 'padding', 10);
        // bb stays default border-box (matches pulp's pre-#1516 implicit
        // behavior and Yoga 3.x's own default).
        setFlex('cb', 'width',  100);
        setFlex('cb', 'height', 100);
        setFlex('cb', 'padding', 10);
        setBoxSizing('cb', 'content-box');
    )");
    root.layout_children();
    auto* bb = bridge.widget("bb");
    auto* cb = bridge.widget("cb");
    REQUIRE(bb != nullptr);
    REQUIRE(cb != nullptr);
    // border-box: outer == declared (100); content area shrinks.
    REQUIRE_THAT(bb->bounds().width,  WithinAbs(100.0f, 0.5f));
    REQUIRE_THAT(bb->bounds().height, WithinAbs(100.0f, 0.5f));
    // content-box: outer == declared + padding*2 (120).
    REQUIRE_THAT(cb->bounds().width,  WithinAbs(120.0f, 0.5f));
    REQUIRE_THAT(cb->bounds().height, WithinAbs(120.0f, 0.5f));
}

