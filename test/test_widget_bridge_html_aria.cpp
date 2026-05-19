// test_widget_bridge_html_aria.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// Wave 3 html bundle — two coherent compat surfaces:
//
//   1. ARIA attribute routing (#1476 / html.2). aria-label and role
//      attributes flow through the html-compat shim into View's
//      accessibility slots that the macOS NSAccessibility bridge
//      consumes. setAttribute / removeAttribute round-trip;
//      mount-deferred attributes replay on appendChild; aria-pressed
//      / -checked / -disabled / -hidden route to View slots.
//
//   2. querySelector / querySelectorAll (#1476 / html.3). Selector
//      engine accepts tag / .class / #id / attribute selectors,
//      colons inside attribute brackets, descendant ( ) and child
//      (>) combinators, :hover / :disabled / :checked / :enabled /
//      :not() pseudo-classes, and structural :first-child / :last-child
//      / :nth-child / :only-child / :empty.

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

// ── pulp Wave 3 html bundle (ARIA + querySelector) ─────────────────────
//
// Wave 3 html.2 / #1476: aria-label / role attributes flow through the
// html-compat shim into View::access_label_ / View::access_role_ slots
// that the macOS NSAccessibility bridge already consumes.  Wave 3 html.3:
// document.querySelector accepts attribute selectors, compound selectors,
// and descendant / child combinators in addition to the previously
// supported tag/.class/#id forms.

TEST_CASE("HTML aria-label routes to View access_label",
          "[view][bridge][wave3-html][html-aria]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Direct bridge fn — exercises the C++ entry point JS-side
    // setAttribute('aria-label', ...) collapses onto.
    bridge.load_script(R"(
        createPanel('a', '');
        setAccessibilityLabel('a', 'Volume control');
    )");

    auto* a = bridge.widget("a");
    REQUIRE(a != nullptr);
    REQUIRE(a->access_label() == "Volume control");
}

TEST_CASE("HTML role attribute routes through ARIA->AccessRole bucket",
          "[view][bridge][wave3-html][html-aria]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Mirror the seven ARIA role buckets we collapse the spec onto.
    bridge.load_script(R"(
        createPanel('s', '');  setAccessibilityRole('s', 'slider');
        createPanel('cb', ''); setAccessibilityRole('cb', 'checkbox');
        createPanel('sw', ''); setAccessibilityRole('sw', 'switch');
        createPanel('im', ''); setAccessibilityRole('im', 'img');
        createPanel('pb', ''); setAccessibilityRole('pb', 'progressbar');
        createPanel('hd', ''); setAccessibilityRole('hd', 'heading');
        createPanel('bn', ''); setAccessibilityRole('bn', 'button');
        createPanel('un', ''); setAccessibilityRole('un', '');
    )");

    REQUIRE(bridge.widget("s")->access_role()  == View::AccessRole::slider);
    REQUIRE(bridge.widget("cb")->access_role() == View::AccessRole::toggle);
    REQUIRE(bridge.widget("sw")->access_role() == View::AccessRole::toggle);
    REQUIRE(bridge.widget("im")->access_role() == View::AccessRole::image);
    REQUIRE(bridge.widget("pb")->access_role() == View::AccessRole::meter);
    REQUIRE(bridge.widget("hd")->access_role() == View::AccessRole::label);
    // 'button' has no Pulp enum slot — collapses to `group`.
    REQUIRE(bridge.widget("bn")->access_role() == View::AccessRole::group);
    // Empty / unknown role clears to none.
    REQUIRE(bridge.widget("un")->access_role() == View::AccessRole::none);
}

TEST_CASE("HTML setAttribute(aria-label) flushes through web-compat shim",
          "[view][bridge][wave3-html][html-aria]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // End-to-end: createElement -> appendChild (so _nativeCreated is true)
    // -> setAttribute('aria-label', ...) goes through the shim's fast path
    // and reaches View::access_label_ via the bridge fn.
    bridge.load_script(R"(
        var d = document.createElement('div');
        d.id = 'a11y-target';
        document.body.appendChild(d);
        d.setAttribute('aria-label', 'Save preset');
        d.setAttribute('role', 'button');
    )");

    auto idVal = engine.evaluate("document.getElementById('a11y-target')._id");
    auto id = std::string(idVal.getWithDefault<std::string_view>(""));
    auto* v = bridge.widget(id);
    REQUIRE(v != nullptr);
    REQUIRE(v->access_label() == "Save preset");
    // 'button' -> group bucket.
    REQUIRE(v->access_role() == View::AccessRole::group);
}

TEST_CASE("HTML setAttribute before mount replays ARIA on appendChild",
          "[view][bridge][wave3-html][html-aria]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // React commits attributes BEFORE mounting in some commit orders, so
    // setAttribute('aria-label', ...) sees _nativeCreated === false.  The
    // shim must replay through __replayAriaAttributes__ once the native
    // node lands via appendChild.
    bridge.load_script(R"(
        var d = document.createElement('div');
        d.id = 'a11y-replay';
        // NOTE — appendChild has not run yet, so the element isn't native.
        // Force the pre-mount path by clearing the flag the createElement
        // helper sets after the createCol call.
        d._nativeCreated = false;
        d.setAttribute('aria-label', 'Filter cutoff');
        d.setAttribute('role', 'slider');
        // Now mount.  appendChild -> _ensureNative -> __replayAriaAttributes__
        document.body.appendChild(d);
    )");

    auto idVal = engine.evaluate("document.getElementById('a11y-replay')._id");
    auto id = std::string(idVal.getWithDefault<std::string_view>(""));
    auto* v = bridge.widget(id);
    REQUIRE(v != nullptr);
    REQUIRE(v->access_label() == "Filter cutoff");
    REQUIRE(v->access_role() == View::AccessRole::slider);
}

// pulp #1641 followup — `removeAttribute('role')` /
// `removeAttribute('aria-label')` must reset View::access_role_ /
// access_label_. The earlier shim only deleted the JS-side
// `_attributes[name]` entry, leaving the bridge slot stale (a
// user-observable bug for assistive tech that reads stale state).
TEST_CASE("HTML removeAttribute resets View accessibility slots",
          "[view][bridge][html][issue-1641-followup-aria-removeattribute]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var d = document.createElement('div');
        d.id = 'aria-rm';
        document.body.appendChild(d);
        d.setAttribute('aria-label', 'Mute toggle');
        d.setAttribute('role', 'switch');
    )");
    auto idVal = engine.evaluate("document.getElementById('aria-rm')._id");
    auto id = std::string(idVal.getWithDefault<std::string_view>(""));
    auto* v = bridge.widget(id);
    REQUIRE(v != nullptr);
    REQUIRE(v->access_label() == "Mute toggle");
    REQUIRE(v->access_role() == View::AccessRole::toggle);

    // removeAttribute should reset the bridge slot (was the bug).
    bridge.load_script(R"(
        var d = document.getElementById('aria-rm');
        d.removeAttribute('aria-label');
        d.removeAttribute('role');
    )");
    REQUIRE(v->access_label().empty());
    REQUIRE(v->access_role() == View::AccessRole::none);
}

// pulp #1737 — ARIA state attributes (aria-pressed, aria-checked,
// aria-disabled, aria-hidden) round-trip through the new
// setAccessibilityState bridge fn into View::access_pressed_ /
// access_checked_ / access_disabled_ / access_hidden_ slots. macOS
// NSAccessibility reads them automatically; Linux AT-SPI / Windows UIA
// read the same slots when those bridges land (pulp #217).
//
// Test exercises both code paths: setAttribute on a mounted element
// (fast-path through the bridge) and setAttribute before mount
// followed by appendChild (replay through __replayAriaAttributes__).
TEST_CASE("HTML aria-pressed / -checked / -disabled / -hidden route to View slots",
          "[view][bridge][wave3-html][html-aria][issue-1737]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        // Direct path: mounted element + setAttribute fast-path.
        var btn = document.createElement('button');
        btn.id = 'mute-btn';
        document.body.appendChild(btn);
        btn.setAttribute('aria-pressed', 'true');
        btn.setAttribute('aria-disabled', 'false');

        // Replay path: setAttribute before mount, appendChild flushes.
        var chk = document.createElement('div');
        chk.id = 'tristate-chk';
        chk._nativeCreated = false;
        chk.setAttribute('aria-checked', 'mixed');
        chk.setAttribute('aria-hidden', 'true');
        document.body.appendChild(chk);
    )");

    auto btn_id = std::string(engine.evaluate(
        "document.getElementById('mute-btn')._id"
    ).getWithDefault<std::string_view>(""));
    auto chk_id = std::string(engine.evaluate(
        "document.getElementById('tristate-chk')._id"
    ).getWithDefault<std::string_view>(""));

    auto* btn = bridge.widget(btn_id);
    auto* chk = bridge.widget(chk_id);
    REQUIRE(btn != nullptr);
    REQUIRE(chk != nullptr);

    // Fast-path values.
    REQUIRE(btn->access_pressed()  == "true");
    REQUIRE(btn->access_disabled() == "false");
    // Replay-path values (incl. tri-state `mixed` per ARIA 1.2).
    REQUIRE(chk->access_checked() == "mixed");
    REQUIRE(chk->access_hidden()  == "true");

    // removeAttribute clears the slot.
    bridge.load_script(R"(
        document.getElementById('mute-btn').removeAttribute('aria-pressed');
        document.getElementById('tristate-chk').removeAttribute('aria-checked');
    )");
    REQUIRE(btn->access_pressed().empty());
    REQUIRE(chk->access_checked().empty());
    // Untouched slots remain populated.
    REQUIRE(btn->access_disabled() == "false");
    REQUIRE(chk->access_hidden()   == "true");
}

TEST_CASE("querySelector matches tag / .class / #id forms",
          "[view][bridge][wave3-html][html-querySelector]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var p = document.createElement('div');
        p.id = 'qs-root';
        document.body.appendChild(p);
        var a = document.createElement('span'); a.id = 'a'; a.className = 'foo';
        var b = document.createElement('span'); b.id = 'b'; b.className = 'bar baz';
        var c = document.createElement('p');    c.id = 'c'; c.className = 'foo qux';
        p.appendChild(a); p.appendChild(b); p.appendChild(c);

        globalThis.__byTag    = document.querySelector('p') !== null;
        globalThis.__byId     = document.querySelector('#b') !== null;
        globalThis.__byCls    = document.querySelectorAll('.foo').length;
        globalThis.__compound = document.querySelector('span.foo')   !== null;
        globalThis.__missing  = document.querySelector('.nope');
    )");

    REQUIRE(engine.evaluate("__byTag").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("__byId").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("__byCls").getWithDefault<int64_t>(0) == 2);
    REQUIRE(engine.evaluate("__compound").getWithDefault<bool>(false));
    // Missing match returns null, which the value bridge marshals as
    // "is null" — encode as a JS boolean for the test assertion.
    auto missingIsNull = engine.evaluate("__missing === null").getWithDefault<bool>(false);
    REQUIRE(missingIsNull);
}

TEST_CASE("querySelector matches attribute selectors",
          "[view][bridge][wave3-html][html-querySelector]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var d1 = document.createElement('div'); d1.id='d1';
        d1.setAttribute('data-kind', 'preset');
        var d2 = document.createElement('div'); d2.id='d2';
        d2.setAttribute('data-kind', 'preset-named');
        var d3 = document.createElement('div'); d3.id='d3';
        d3.setAttribute('data-kind', 'cooked');
        var d4 = document.createElement('div'); d4.id='d4';
        d4.setAttribute('aria-label', 'X');
        document.body.appendChild(d1);
        document.body.appendChild(d2);
        document.body.appendChild(d3);
        document.body.appendChild(d4);

        globalThis.__hasAttr = document.querySelectorAll('[data-kind]').length;
        globalThis.__eqAttr  = document.querySelector('[data-kind="preset"]') !== null;
        globalThis.__eqId    = document.querySelector('[data-kind="preset"]').id;
        globalThis.__prefix  = document.querySelectorAll('[data-kind^="preset"]').length;
        globalThis.__contain = document.querySelectorAll('[data-kind*="ook"]').length;
        globalThis.__suffix  = document.querySelectorAll('[data-kind$="ed"]').length;
        globalThis.__withAria= document.querySelector('[aria-label]').id;
    )");

    REQUIRE(engine.evaluate("__hasAttr").getWithDefault<int64_t>(0)  == 3);
    REQUIRE(engine.evaluate("__eqAttr").getWithDefault<bool>(false));
    REQUIRE(std::string(engine.evaluate("__eqId").getWithDefault<std::string_view>("")) == "d1");
    REQUIRE(engine.evaluate("__prefix").getWithDefault<int64_t>(0)   == 2);
    REQUIRE(engine.evaluate("__contain").getWithDefault<int64_t>(0)  == 1);
    REQUIRE(engine.evaluate("__suffix").getWithDefault<int64_t>(0)   == 2);
    REQUIRE(std::string(engine.evaluate("__withAria").getWithDefault<std::string_view>("")) == "d4");
}

// pulp #1641 followup — _parseSelector colon-strip bug. Selectors like
// `[href="http://x"]` and `[data-time="12:30"]` contain `:` inside the
// attribute brackets. The earlier scanner used `str.search(/:/)` which
// found the first colon anywhere — truncating the selector mid-bracket.
// Fix: scan for `:` at bracket depth 0 only.
TEST_CASE("querySelector handles colons inside attribute brackets",
          "[view][bridge][html][issue-1641-followup-querySelector-colon]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var a1 = document.createElement('a'); a1.id = 'a1';
        a1.setAttribute('href', 'http://example.com/page');
        var a2 = document.createElement('a'); a2.id = 'a2';
        a2.setAttribute('href', 'https://example.com/page');
        var d1 = document.createElement('div'); d1.id = 'd1';
        d1.setAttribute('data-time', '12:30');
        document.body.appendChild(a1);
        document.body.appendChild(a2);
        document.body.appendChild(d1);

        // Resolve to id strings (or "MISS") so the C++ side gets clean values.
        globalThis.__byHttp  = (document.querySelector('[href="http://example.com/page"]')  || {id:'MISS'}).id;
        globalThis.__byHttps = (document.querySelector('[href="https://example.com/page"]') || {id:'MISS'}).id;
        globalThis.__byTime  = (document.querySelector('[data-time="12:30"]')               || {id:'MISS'}).id;
        // pulp #1737 — a:hover now strictly matches only currently-hovered
        // anchors. Pre-fix the matcher returned a1 (broader match — any <a>
        // after stripping). Post-fix it returns null because no <a> is
        // currently hovered. After we flag a1 as hovered, the matcher finds it.
        globalThis.__pseudoNull = document.querySelector('a:hover') === null;
        a1._isHovered = true;
        globalThis.__pseudo = (document.querySelector('a:hover') || {id:'MISS'}).id;
    )");

    REQUIRE(std::string(engine.evaluate("__byHttp" ).getWithDefault<std::string_view>("")) == "a1");
    REQUIRE(std::string(engine.evaluate("__byHttps").getWithDefault<std::string_view>("")) == "a2");
    REQUIRE(std::string(engine.evaluate("__byTime" ).getWithDefault<std::string_view>("")) == "d1");
    REQUIRE(engine.evaluate("__pseudoNull").getWithDefault<bool>(false));
    REQUIRE(std::string(engine.evaluate("__pseudo" ).getWithDefault<std::string_view>("")) == "a1");
}

TEST_CASE("querySelector descendant and child combinators",
          "[view][bridge][wave3-html][html-querySelector]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var outer = document.createElement('section');
        outer.className = 'panel';
        var mid   = document.createElement('div');
        mid.className   = 'mid';
        var inner = document.createElement('span'); inner.id = 'inner';
        var sibling = document.createElement('span'); sibling.id = 'sib';
        // tree: section.panel > div.mid > span#inner ; section.panel > span#sib
        document.body.appendChild(outer);
        outer.appendChild(mid);
        mid.appendChild(inner);
        outer.appendChild(sibling);

        globalThis.__desc        = document.querySelector('section span') !== null;
        globalThis.__descId      = document.querySelector('section.panel span').id;
        globalThis.__directHit   = document.querySelector('section.panel > span').id;
        globalThis.__directMiss  = document.querySelector('section.panel > p');
        globalThis.__deepDescAll = document.querySelectorAll('.panel span').length;
    )");

    REQUIRE(engine.evaluate("__desc").getWithDefault<bool>(false));
    // descendant `section.panel span` finds the deepest match first per
    // BFS — `span#inner` (or `span#sib` — both match; the first BFS hit
    // wins).  We only require that the result IS one of the two, which
    // it must be when the matcher works.
    auto descId = std::string(engine.evaluate("__descId").getWithDefault<std::string_view>(""));
    REQUIRE((descId == "inner" || descId == "sib"));
    // child `section.panel > span` matches only `sib` (mid is the
    // immediate parent of `inner`, not section).
    REQUIRE(std::string(engine.evaluate("__directHit").getWithDefault<std::string_view>("")) == "sib");
    REQUIRE(engine.evaluate("__directMiss === null").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("__deepDescAll").getWithDefault<int64_t>(0) == 2);
}

// pulp #1737 — querySelector now evaluates pseudo-classes spec-correctly
// (used to "tolerate by stripping" — every selector that included
// `:hover` matched all elements regardless of hover state). The new
// matcher honours element state for runtime-state pseudo-classes
// (`:hover`, `:focus`, `:active`, `:disabled`, `:checked`, `:enabled`),
// DOM-position pseudo-classes (`:first-child`, `:last-child`, `:nth-child`,
// `:nth-last-child`, `:only-child`, `:empty`, `:root`), and the
// functional `:not(<simple>)` form.
//
// This first test asserts the contract change: `:hover` no longer
// matches every `div.foo` — it correctly returns null when no element
// is currently hovered. Pre-fix the matcher returned the div; post-fix
// it returns null (no widget is hovered in this synthetic environment).
// Importantly, the call MUST NOT throw — unknown / state-false
// pseudo-classes return no-match per CSS Selectors Level 4 forward-
// compat, which is what the test now asserts.
TEST_CASE("querySelector evaluates :hover pseudo-class against element state",
          "[view][bridge][wave3-html][html-querySelector][issue-1737]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var d = document.createElement('div');
        d.id = 'pseudo'; d.className = 'foo';
        document.body.appendChild(d);

        // Selector parses without throwing.
        globalThis.__noThrow = (function() {
            try { document.querySelector('div.foo:hover'); return true; }
            catch (e) { return false; }
        })();
        // Returns null because nothing is hovered.
        globalThis.__hoverNull = document.querySelector('div.foo:hover') === null;
        // After flagging the element as hovered, the matcher finds it.
        d._isHovered = true;
        globalThis.__hoverHit = document.querySelector('div.foo:hover');
    )");

    REQUIRE(engine.evaluate("__noThrow").getWithDefault<bool>(false));
    REQUIRE(engine.evaluate("__hoverNull").getWithDefault<bool>(false));
    REQUIRE(std::string(
        engine.evaluate("__hoverHit ? __hoverHit.id : ''")
            .getWithDefault<std::string_view>("")
    ) == "pseudo");
}

// pulp #1737 — :disabled, :checked, :enabled — state-on-element pseudo
// classes that read the bridge-maintained el._disabled / el._checked
// slots. Comprehensive coverage including the negation pseudo (:not()).
TEST_CASE("querySelector :disabled / :checked / :enabled / :not()",
          "[view][bridge][wave3-html][html-querySelector][issue-1737]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var b1 = document.createElement('button');
        b1.id = 'b1'; b1.disabled = true;
        var b2 = document.createElement('button');
        b2.id = 'b2'; // not disabled
        var b3 = document.createElement('button');
        b3.id = 'b3'; b3._checked = true;
        document.body.appendChild(b1);
        document.body.appendChild(b2);
        document.body.appendChild(b3);

        globalThis.__disabledHit = document.querySelector('button:disabled').id;
        globalThis.__enabledMatches = document.querySelectorAll('button:enabled').length;
        globalThis.__checkedHit = document.querySelector('button:checked').id;
        // :not(<simple>) — the negated-class form. b2 + b3 are not disabled.
        globalThis.__notDisabledMatches = document.querySelectorAll('button:not(:disabled)').length;
    )");

    REQUIRE(std::string(engine.evaluate("__disabledHit")
        .getWithDefault<std::string_view>("")) == "b1");
    REQUIRE(engine.evaluate("__enabledMatches").getWithDefault<int64_t>(0) == 2);
    REQUIRE(std::string(engine.evaluate("__checkedHit")
        .getWithDefault<std::string_view>("")) == "b3");
    REQUIRE(engine.evaluate("__notDisabledMatches").getWithDefault<int64_t>(0) == 2);
}

// pulp #1737 — DOM-position pseudo-classes: :first-child, :last-child,
// :nth-child(N), :nth-child(2n+1), :only-child, :empty, :root.
TEST_CASE("querySelector :first-child / :last-child / :nth-child / :only-child / :empty",
          "[view][bridge][wave3-html][html-querySelector][issue-1737]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var ul = document.createElement('ul');
        ul.id = 'list';
        document.body.appendChild(ul);
        for (var i = 0; i < 5; i++) {
            var li = document.createElement('li');
            li.id = 'li' + i;
            ul.appendChild(li);
        }
        // Lone-child sibling under a separate parent.
        var solo_parent = document.createElement('div');
        solo_parent.id = 'solo-parent';
        document.body.appendChild(solo_parent);
        var solo = document.createElement('span');
        solo.id = 'solo';
        solo_parent.appendChild(solo);

        // First / last / only.
        globalThis.__first = document.querySelector('li:first-child').id;
        globalThis.__last = document.querySelector('li:last-child').id;
        globalThis.__only = document.querySelector('span:only-child').id;
        // nth-child(2) — 1-based, so li1.
        globalThis.__nth2 = document.querySelector('li:nth-child(2)').id;
        // nth-child(2n+1) — odd children: li0, li2, li4 → 3 hits.
        globalThis.__nthOddCount = document.querySelectorAll('li:nth-child(2n+1)').length;
        // :empty — solo_parent is NOT empty (has solo); list is NOT empty;
        // each individual li IS empty (no children).
        globalThis.__emptyCount = document.querySelectorAll('li:empty').length;
    )");

    REQUIRE(std::string(engine.evaluate("__first")
        .getWithDefault<std::string_view>("")) == "li0");
    REQUIRE(std::string(engine.evaluate("__last")
        .getWithDefault<std::string_view>("")) == "li4");
    REQUIRE(std::string(engine.evaluate("__only")
        .getWithDefault<std::string_view>("")) == "solo");
    REQUIRE(std::string(engine.evaluate("__nth2")
        .getWithDefault<std::string_view>("")) == "li1");
    REQUIRE(engine.evaluate("__nthOddCount").getWithDefault<int64_t>(0) == 3);
    REQUIRE(engine.evaluate("__emptyCount").getWithDefault<int64_t>(0) == 5);
}
