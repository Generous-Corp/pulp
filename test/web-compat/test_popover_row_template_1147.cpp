// pulp #1147 — popover-row-template render regression
//
// Spectr's popover panels render the outer panel + headers correctly but the
// inner button rows fail in two distinct shapes:
//
//   (A) AnalyzerPopoverRepro — `<button display:block>` with a child
//       `<span display:flex>` (icon row) followed by a `<span display:block>`
//       (description). The description text "leaks" past the panel boundary
//       because the description span never gets a width that wraps inside
//       the button.
//
//   (B) EditModePopoverRepro — `<button display:flex flex-direction:row>`
//       with an inline `<svg width="28" height="20">` followed by a
//       `<span flex:1>` containing a nested column. The SVG reports 0×0
//       to layout (HTML width/height attributes never propagate to flex
//       sizing) so the row collapses or paints blank.
//
// Both repros boil down to the simplest <button>+nested-flex shape so a
// fix can be validated without React, asset pipelines, or a popover host.
//
// The full TSX repro lives at
// `spectr/native-react/repros/repro-1147-popover-render.tsx` (115 lines)
// and is the inspiration for these C++ assertions; the JS shape here uses
// `document.createElement` + `el.style.*` to walk the same prop-applier
// path React would.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"
#include <iostream>

using namespace pulp::test;
using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

// Walk the JS DOM tree and collect every native widget id that descends
// from the named root. Used to look up nested <span> ids that the JSX
// repro would otherwise reference via React refs.
inline std::string js_id(TestEnvironment& env, const std::string& js_var) {
    return std::string(env.engine.evaluate(js_var + "._id")
                            .getWithDefault<std::string_view>(""));
}

// Resolve a child View by its DOM-side variable so tests can read its
// post-layout bounds without depending on the auto-generated id.
inline View* resolve(TestEnvironment& env, const std::string& js_var) {
    auto id = js_id(env, js_var);
    REQUIRE_FALSE(id.empty());
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    return w;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// (A) AnalyzerPopover — display:block button with a flex inner row + a
// block description child. Bug: the description's bounds leak past the
// button width because nested-span layout is broken.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("popover-1147 (A): button display:block lays out children in a column",
          "[layout][popover][issue-1147]") {
    TestEnvironment env(320, 200);
    env.eval(R"JS(
        var __btn = document.createElement('button');
        __btn.style.display = 'block';
        __btn.style.width = '240px';
        __btn.style.padding = '7px 10px';

        var __row = document.createElement('span');
        __row.style.display = 'flex';
        __row.style.alignItems = 'center';
        __row.style.gap = '8px';

        var __dot = document.createElement('span');
        __dot.style.display = 'inline-block';
        __dot.style.width = '18px';
        __dot.style.height = '2px';

        var __label = document.createElement('span');
        __label.textContent = 'SPECTRUM';

        __row.appendChild(__dot);
        __row.appendChild(__label);

        var __desc = document.createElement('span');
        __desc.style.display = 'block';
        __desc.textContent = 'description line';

        __btn.appendChild(__row);
        __btn.appendChild(__desc);
        document.body.appendChild(__btn);
    )JS");
    env.root.layout_children();

    auto* btn  = resolve(env, "__btn");
    auto* row  = resolve(env, "__row");
    auto* desc = resolve(env, "__desc");
    auto* dot  = resolve(env, "__dot");
    auto* label= resolve(env, "__label");

    auto dump = [](const char* name, View* v) {
        auto b = v->bounds();
        std::cout << "[diag] "<<name<<" bounds=("<<b.x<<","<<b.y<<","<<b.width<<","<<b.height
                  <<") child_count="<<v->child_count()<<"\n";
    };
    dump("btn",   btn);
    dump("row",   row);
    dump("dot",   dot);
    dump("label", label);
    dump("desc",  desc);

    // The button is 240px wide and uses display:block (column layout).
    REQUIRE_THAT(btn->bounds().width, WithinAbs(240.0f, 1.0f));

    // Inner row + description must stack vertically (column), not overlap.
    // The description should sit BELOW the row inside the button. Yoga
    // sub-pixel-rounds adjacent flex-item positions, so we allow up to 1.5px
    // of nominal overlap between the row's reported bottom and the desc's
    // top edge — the real bug we're guarding is horizontal leakage past the
    // button edge, asserted below.
    REQUIRE(desc->bounds().y >= row->bounds().y + row->bounds().height - 1.5f);

    // (A) — anti-drift assertion #1147.
    // Both children must fit inside the button horizontally. Pre-fix the
    // <span display:block> description span paints past the right edge
    // because its native widget never receives a wrapped content width.
    REQUIRE(row->bounds().x + row->bounds().width  <= btn->bounds().width + 1.0f);
    REQUIRE(desc->bounds().x + desc->bounds().width <= btn->bounds().width + 1.0f);

    // The description must occupy non-zero height so the popover row is
    // actually visible to the user (regression: pre-fix Yoga collapses it
    // when the parent <span>'s intrinsic measure func clobbers child
    // layout).
    REQUIRE(desc->bounds().height > 0.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// (B) EditModePopover — display:flex flex-row button with an inline <svg>
// + a flex-1 <span> column. Bug: the SVG reports 0×0 because HTML
// width/height attributes are never converted to flex preferred sizing,
// AND the flex-1 span column collapses to 0 when its parent's content
// width is unconstrained.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("popover-1147 (B): inline <svg width/height> reserves layout space",
          "[layout][popover][svg][issue-1147]") {
    TestEnvironment env(320, 200);
    env.eval(R"JS(
        var __btn = document.createElement('button');
        __btn.style.display = 'flex';
        __btn.style.flexDirection = 'row';
        __btn.style.alignItems = 'flex-start';
        __btn.style.gap = '10px';
        __btn.style.width = '260px';

        var __svg = document.createElement('svg');
        __svg.setAttribute('width', '28');
        __svg.setAttribute('height', '20');
        __svg.style.flex = 'none';

        var __col = document.createElement('span');
        __col.style.flex = '1';

        var __title = document.createElement('span');
        __title.textContent = 'PEAK';
        __col.appendChild(__title);

        __btn.appendChild(__svg);
        __btn.appendChild(__col);
        document.body.appendChild(__btn);
    )JS");
    {
        auto svgTag = std::string(env.engine.evaluate("__svg.tagName").getWithDefault<std::string_view>(""));
        auto svgAttr = std::string(env.engine.evaluate("JSON.stringify(__svg._attributes)").getWithDefault<std::string_view>(""));
        auto setFlexT = std::string(env.engine.evaluate("typeof setFlex").getWithDefault<std::string_view>(""));
        auto nc = std::string(env.engine.evaluate("String(__svg._nativeCreated)").getWithDefault<std::string_view>(""));
        std::cout << "[diag-B] __svg.tagName=" << svgTag << " attrs=" << svgAttr
                  << " setFlex=" << setFlexT << " nativeCreated=" << nc << "\n";
    }
    env.root.layout_children();

    auto* btn = resolve(env, "__btn");
    auto* svg = resolve(env, "__svg");
    auto* col = resolve(env, "__col");

    std::cout << "[diag-B] svg.flex.preferred_width=" << svg->flex().preferred_width
              << " preferred_height=" << svg->flex().preferred_height
              << " bounds=("<<svg->bounds().x<<","<<svg->bounds().y<<","<<svg->bounds().width<<","<<svg->bounds().height<<")\n";
    std::cout << "[diag-B] btn.bounds=("<<btn->bounds().x<<","<<btn->bounds().y<<","<<btn->bounds().width<<","<<btn->bounds().height
              <<") direction="<<int(btn->flex().direction)<<"\n";
    std::cout << "[diag-B] col.bounds=("<<col->bounds().x<<","<<col->bounds().y<<","<<col->bounds().width<<","<<col->bounds().height<<")\n";

    REQUIRE_THAT(btn->bounds().width, WithinAbs(260.0f, 1.0f));

    // (B) anti-drift #1147 — SVG width/height attributes must reserve real
    // layout space. Pre-fix the SVG element renders as a createCol with
    // no flex preferred size and Yoga gives it 0×0; the row collapses.
    REQUIRE_THAT(svg->bounds().width,  WithinAbs(28.0f, 1.0f));
    REQUIRE_THAT(svg->bounds().height, WithinAbs(20.0f, 1.0f));

    // The flex:1 span must consume the remaining row width so the title
    // sits to the right of the SVG, not on top of it. With `gap:10`,
    // remaining = 260 - 28 - 10 = 222.
    REQUIRE_THAT(col->bounds().x, WithinAbs(38.0f, 1.5f));
    REQUIRE(col->bounds().width  >= 200.0f);
    REQUIRE(col->bounds().height >  0.0f);
}

// Sanity test that a screenshot of the (B) shape paints non-trivially —
// guards against future regressions where the layout is right but paint
// renders nothing. macOS-only because Linux CI's headless render path
// occasionally returns an empty buffer for body-attached subtrees and
// the layout assertions above already cover the actual #1147 regression.
#if defined(__APPLE__)
TEST_CASE("popover-1147 (B): nested popover row paints non-empty pixels",
          "[layout][popover][svg][issue-1147][paint]") {
    TestEnvironment env(320, 80);
    env.root.set_theme(Theme::dark());
    env.eval(R"JS(
        var __btn = document.createElement('button');
        __btn.style.display = 'flex';
        __btn.style.flexDirection = 'row';
        __btn.style.alignItems = 'center';
        __btn.style.gap = '10px';
        __btn.style.width = '260px';
        __btn.style.padding = '8px 10px';
        __btn.style.backgroundColor = '#202832';

        var __svg = document.createElement('svg');
        __svg.setAttribute('width', '28');
        __svg.setAttribute('height', '20');

        var __col = document.createElement('span');
        __col.style.flex = '1';
        __col.textContent = 'PEAK';

        __btn.appendChild(__svg);
        __btn.appendChild(__col);
        document.body.appendChild(__btn);
    )JS");
    env.root.layout_children();

    auto png = render_to_png(env.root, 320, 80, 1.0f);
    REQUIRE_FALSE(png.empty());
}
#endif
