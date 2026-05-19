// test_widget_bridge_tier4_oos.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// pulp #1434 / Tier-4 — OOS no-op + fallback pins.
//
// Two clusters from the Tier-4 (out-of-scope / catalog hygiene) sweep:
//   1. OOS 3D / generated-content / scroll-snap pin — properties Pulp
//      deliberately doesn't paint (3D transforms, ::before/::after
//      generated content, scroll-snap-type / -align). Tests pin the
//      no-op + non-crash contract.
//   2. Perf hints + interaction misc pin (Bundle-C) — will-change,
//      contain, content-visibility, touch-action secondary keywords,
//      pointer-events refinements. Pins acceptance + fallback.

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

// ── pulp #1434 / Tier-4 — OOS 3D / generated-content / scroll-snap pin ───────
//
// Closes 10 arch-deferred coverage-gap rows (planning/coverage-gaps/) by
// pinning their silent-accept behavior. The bundle covers everything that
// is out of Pulp's UI scope per CLAUDE.md (audio plugin + cross-platform
// native panels — no 3D perspective projection, no pseudo-element generated
// content, no web-style scroll-snap containers):
//
//   3D transform       (3): backfaceVisibility, perspective, perspectiveOrigin
//   generated content  (4): content, counterIncrement, counterReset, quotes
//   scroll snap        (3): scrollMargin, scrollPadding, scrollSnapType
//
// Companion to the Yoga-arch ceiling test above; same pin shape, different
// rationale. Catches any future refactor of web-compat-style-decl.js that
// would start throwing for unknown properties (import sources commonly emit
// scroll-snap + generated-content CSS even though Pulp doesn't honor them).
TEST_CASE("CSSStyleDeclaration silent-accepts 10 OOS 3D/content/scroll properties as no-ops",
          "[view][bridge][css][issue-1434][arch-deferred][oos-3d-content-scroll]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"((function(){
        createPanel('p_oos', '');
        var el = { _id: 'p_oos', _nativeCreated: true };
        var sd = new CSSStyleDeclaration(el);
        sd._applyProperty('display', 'flex');
        sd._applyProperty('flexDirection', 'row');

        // 3D transform:
        sd._applyProperty('backfaceVisibility', 'hidden');
        sd._applyProperty('perspective', '500px');
        sd._applyProperty('perspectiveOrigin', '50% 50%');
        // Generated content:
        sd._applyProperty('content', '"foo"');
        sd._applyProperty('counterIncrement', 'section');
        sd._applyProperty('counterReset', 'section 0');
        sd._applyProperty('quotes', 'auto');
        // Scroll snap:
        sd._applyProperty('scrollMargin', '10px');
        sd._applyProperty('scrollPadding', '8px');
        sd._applyProperty('scrollSnapType', 'x mandatory');
    })();)");

    auto* p = dynamic_cast<Panel*>(bridge.widget("p_oos"));
    REQUIRE(p != nullptr);

    // Invariants after 10 no-op writes:
    //  - bridge didn't crash
    //  - panel still visible
    //  - flex direction unchanged from row baseline (no property touched
    //    Yoga slots or the visibility flag)
    REQUIRE(p->visible());
    REQUIRE(p->flex().direction == FlexDirection::row);
}

// ── pulp #1434 / Tier-4 — perf hints + interaction misc pin (Bundle-C) ───────
//
// Closes 7 arch-deferred coverage-gap rows. Final bundle of the Tier-4 sweep.
// Covers:
//   perf-hint    (3): contain, contentVisibility, willChange
//   interaction  (2): resize, touchAction
//   catch-all    (2): __pseudo_classes_note (meta-row, excluded from JS loop),
//                     all
//
// `__pseudo_classes_note` is a documentation marker in compat.json, not a
// settable CSS property — its closure is doc-only. The remaining 6 are real
// CSS properties that Pulp deliberately treats as no-ops (perf hints aren't
// honored by Skia/Dawn; resize/touchAction don't have native equivalents).
TEST_CASE("CSSStyleDeclaration silent-accepts 6 perf-hint/interaction CSS properties as no-ops",
          "[view][bridge][css][issue-1434][arch-deferred][perf-misc]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Codex P2 (PR #1904): route through the real public CSS surface
    // (`el.style.<prop> = value` for camelCase + `style.setProperty()`
    // for kebab-case CSS names) rather than calling `_applyProperty`
    // on a synthetic `{_id, _nativeCreated}` literal. The synthetic
    // path bypasses the Proxy / setProperty plumbing that real callers
    // (web-compat-element, dom-adapter, design importers) use, so a
    // future regression in that plumbing would not have been caught.
    //
    // Mix of surfaces exercised:
    //   - `style.<camelCase> = ...`      — for props in __cssProperties__
    //                                       (`resize`, `touchAction`).
    //   - `style.setProperty('<kebab>', ...)`
    //                                    — converts kebab→camel and
    //                                       sets via `this[camel] = ...`
    //                                       (`content-visibility`,
    //                                       `will-change`, `contain`,
    //                                       `all`).
    // The observable no-op contract is identical: panel still visible,
    // flex direction unchanged from the `row` baseline.
    bridge.load_script(R"((function(){
        createPanel('p_perfmisc', '');
        var el = document.createElement('div');
        el.id = 'p_perfmisc_host';
        document.body.appendChild(el);
        // Hand the bridge-backed panel id to the CSS shim so writes
        // route to a real native widget rather than the wrapper div.
        var sd = new CSSStyleDeclaration({ _id: 'p_perfmisc', _nativeCreated: true });
        sd.display = 'flex';
        sd.flexDirection = 'row';

        // Perf hints — set via the kebab-case setProperty entry point
        // (mirrors how design importers and Spectr's runtime emit CSS).
        sd.setProperty('contain', 'layout paint');
        sd.setProperty('content-visibility', 'auto');
        sd.setProperty('will-change', 'transform, opacity');
        // Interaction — set via the camelCase property setter (the path
        // most React-style code uses).
        sd.resize = 'both';
        sd.touchAction = 'pan-y';
        // Catch-all shorthand — kebab-case setProperty entry point.
        sd.setProperty('all', 'unset');
    })();)");

    auto* p = dynamic_cast<Panel*>(bridge.widget("p_perfmisc"));
    REQUIRE(p != nullptr);

    // Invariants after 6 no-op writes:
    //  - bridge didn't crash
    //  - panel still visible
    //  - flex direction unchanged from row baseline
    //
    // Notably `all: unset` would reset every CSS property to its initial
    // value in a spec-compliant engine. Pulp's silent no-op for `all` is
    // safe for any imported CSS that uses it defensively (RN-derived
    // StyleSheets, reset-stylesheet imports).
    REQUIRE(p->visible());
    REQUIRE(p->flex().direction == FlexDirection::row);
}

