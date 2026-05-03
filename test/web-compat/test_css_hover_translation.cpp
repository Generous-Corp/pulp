// CSS :hover translation tests — pulp #1323 / #1149 part b.
//
// Validates that setting CSS rules via a `<style>` element (the path
// React/Spectr's editor.js bundle uses) translates `:hover` selectors
// into mouseenter/mouseleave listeners that mutate `el.style` and
// arms the native dispatcher via registerHover(id).
//
// Path-1 (runtime) translator: `<style>` textContent is parsed into
// a StyleSheet inside web-compat-document.js, the same StyleSheet
// pseudo-class wiring the JS-API `new StyleSheet(...)` users have had
// since the prelude landed. The test entry point is the
// `<style>.textContent = "..."` assignment, exercising both the
// CSS-text parser and the live element matcher.

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;

namespace {

// Drive a hover-enter on the JS side so the native dispatcher fires
// `mouseenter` and the CSS-derived listener mutates `el.style`. We
// reach into the View via the bridge rather than dispatchEvent() so
// the test exercises the same path a real cursor would (registerHover
// installs `on_hover_enter` on the View, which `set_hovered(true)`
// invokes — see widget_bridge.cpp registerHover handler).
void hover_enter(TestEnvironment& env, const std::string& id) {
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    w->set_hovered(true);
}

void hover_leave(TestEnvironment& env, const std::string& id) {
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    w->set_hovered(false);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Basic translation: <style>.btn:hover { ... }</style>
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("CSS hover: <style> :hover rule mutates element on mouseenter", "[issue-1323][css-hover]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent = '.btn:hover { background: red; }';
        document.body.appendChild(__style);

        var __btn = document.createElement('div');
        __btn.className = 'btn';
        __btn.style.background = 'blue';
        document.body.appendChild(__btn);
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->has_background_color());
    auto baseline = w->background_color();

    hover_enter(env, id);

    // The hover rule sets background to red; baseline was blue.
    REQUIRE(w->has_background_color());
    auto hovered = w->background_color();
    REQUIRE(hovered.r > 0.5f);
    REQUIRE(hovered.g < 0.5f);
    REQUIRE(hovered.b < 0.5f);
    // Distinct from baseline.
    REQUIRE_FALSE(baseline.r > 0.5f);
}

TEST_CASE("CSS hover: mouseleave reverts to base style", "[issue-1323][css-hover]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent = '.btn:hover { background: red; }';
        document.body.appendChild(__style);

        var __btn = document.createElement('div');
        __btn.className = 'btn';
        __btn.style.background = 'blue';
        document.body.appendChild(__btn);
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    auto baseline = w->background_color();

    hover_enter(env, id);
    auto hovered = w->background_color();
    REQUIRE(hovered.r > 0.5f); // Confirm hover applied.

    hover_leave(env, id);
    auto reverted = w->background_color();
    // Reverted bg matches baseline within float tolerance.
    REQUIRE(std::abs(reverted.r - baseline.r) < 0.01f);
    REQUIRE(std::abs(reverted.g - baseline.g) < 0.01f);
    REQUIRE(std::abs(reverted.b - baseline.b) < 0.01f);
}

// ─────────────────────────────────────────────────────────────────────
// Co-existence with JSX onMouseEnter (#1173)
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("CSS hover: JS mouseenter listener fires alongside CSS :hover", "[issue-1323][css-hover]") {
    // Pulp #1173 wired registerHover when a JSX onMouseEnter is present;
    // adding a CSS `:hover` rule must not clobber that listener — both
    // the CSS-applied style change AND the JS callback should run.
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent = '.btn:hover { background: red; }';
        document.body.appendChild(__style);

        var __btn = document.createElement('div');
        __btn.className = 'btn';
        __btn.style.background = 'blue';
        document.body.appendChild(__btn);

        var __jsHandlerFired = 0;
        __btn.addEventListener('mouseenter', function() {
            __jsHandlerFired = __jsHandlerFired + 1;
        });
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);

    hover_enter(env, id);

    REQUIRE(w->background_color().r > 0.5f);
    REQUIRE(env.engine.evaluate("__jsHandlerFired").getWithDefault<double>(0.0) == 1.0);
}

// ─────────────────────────────────────────────────────────────────────
// Layered :hover rules — multiple rules on the same element
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("CSS hover: multiple :hover rules layer last-wins per property", "[issue-1323][css-hover]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        // Two rules touching the same element. The first sets background
        // and color, the second overrides background. Both apply on
        // hover; mouseleave reverts both.
        __style.textContent =
            '.btn:hover { background: red; color: white; }' +
            '.primary:hover { background: green; }';
        document.body.appendChild(__style);

        var __btn = document.createElement('div');
        __btn.className = 'btn primary';
        __btn.style.background = 'blue';
        __btn.style.color = 'black';
        document.body.appendChild(__btn);
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);

    hover_enter(env, id);

    // background: rule order is `.btn:hover` then `.primary:hover`, so
    // green wins. We only assert green dominates because the prelude's
    // CSS color path may resolve `green` to #008000 (g≈0.5).
    auto bg = w->background_color();
    REQUIRE(bg.g > 0.4f);
    REQUIRE(bg.r < 0.2f);
}

TEST_CASE("CSS hover: mouseleave reverts every layered property", "[issue-1323][css-hover]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent =
            '.btn:hover { background: red; }' +
            '.primary:hover { color: yellow; }';
        document.body.appendChild(__style);

        var __btn = document.createElement('div');
        __btn.className = 'btn primary';
        __btn.style.background = 'blue';
        __btn.style.color = 'black';
        document.body.appendChild(__btn);
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));

    hover_enter(env, id);
    hover_leave(env, id);

    // After leave, the JS-side `el.style.background` should be the
    // original value — this is what the styled-decl props store
    // observes, so it's the closest assertion we can make in headless.
    auto bgProp = std::string(
        env.engine.evaluate("__btn.style._props['background'] || __btn.style._props['backgroundColor'] || ''")
            .getWithDefault<std::string_view>(""));
    REQUIRE(bgProp == "blue");
}

// ─────────────────────────────────────────────────────────────────────
// No-match tolerance + idempotency
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("CSS hover: selector with no matching element is a no-op", "[issue-1323][css-hover]") {
    TestEnvironment env;
    // `.ghost:hover` matches nothing; `.btn:hover` matches one element.
    // The translator should not throw, and the matched element should
    // still receive its rule.
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent =
            '.ghost:hover { background: red; }' +
            '.btn:hover { background: red; }';
        document.body.appendChild(__style);

        var __btn = document.createElement('div');
        __btn.className = 'btn';
        __btn.style.background = 'blue';
        document.body.appendChild(__btn);
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);

    hover_enter(env, id);
    REQUIRE(w->background_color().r > 0.5f);
}

// ─────────────────────────────────────────────────────────────────────
// registerHover wiring — the native dispatcher must be armed
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("CSS hover: registerHover wires native on_hover_enter callback", "[issue-1323][css-hover]") {
    // The functional proof that registerHover ran: the View has a
    // non-null on_hover_enter callback after the rule attached. Without
    // registerHover, the native dispatcher never fires and CSS hover
    // would be invisible to the cursor. (The set_hovered(true) path
    // in earlier tests already exercises the callback firing; here we
    // assert the wiring is present *before* we trigger anything.)
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent = '.btn:hover { background: red; }';
        document.body.appendChild(__style);

        var __btn = document.createElement('div');
        __btn.className = 'btn';
        document.body.appendChild(__btn);
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    // registerHover() in widget_bridge.cpp installs both callbacks.
    REQUIRE(static_cast<bool>(w->on_hover_enter));
    REQUIRE(static_cast<bool>(w->on_hover_leave));
}

// ─────────────────────────────────────────────────────────────────────
// Tag and id selectors
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("CSS hover: tag selector :hover matches by tag name", "[issue-1323][css-hover]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent = 'button:hover { background: red; }';
        document.body.appendChild(__style);

        var __btn = document.createElement('button');
        __btn.style.background = 'blue';
        document.body.appendChild(__btn);
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);

    hover_enter(env, id);
    REQUIRE(w->background_color().r > 0.5f);
}

TEST_CASE("CSS hover: id selector :hover matches by id", "[issue-1323][css-hover]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent = '#go:hover { background: red; }';
        document.body.appendChild(__style);

        var __btn = document.createElement('div');
        __btn.id = 'go';
        __btn.style.background = 'blue';
        document.body.appendChild(__btn);
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);

    hover_enter(env, id);
    REQUIRE(w->background_color().r > 0.5f);
}

// ─────────────────────────────────────────────────────────────────────
// Parser robustness
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("CSS hover: comments in CSS source are stripped", "[issue-1323][css-hover]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent =
            '/* preface */ .btn:hover { /* inline */ background: red; } /* trailer */';
        document.body.appendChild(__style);

        var __btn = document.createElement('div');
        __btn.className = 'btn';
        __btn.style.background = 'blue';
        document.body.appendChild(__btn);
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);

    hover_enter(env, id);
    REQUIRE(w->background_color().r > 0.5f);
}

TEST_CASE("CSS hover: comma-separated selector list applies to each", "[issue-1323][css-hover]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent = '.a:hover, .b:hover { background: red; }';
        document.body.appendChild(__style);

        var __a = document.createElement('div');
        __a.className = 'a';
        __a.style.background = 'blue';
        document.body.appendChild(__a);

        var __b = document.createElement('div');
        __b.className = 'b';
        __b.style.background = 'blue';
        document.body.appendChild(__b);
    )JS");

    auto idA = std::string(env.engine.evaluate("__a._id").getWithDefault<std::string_view>(""));
    auto idB = std::string(env.engine.evaluate("__b._id").getWithDefault<std::string_view>(""));

    hover_enter(env, idA);
    REQUIRE(env.widget(idA)->background_color().r > 0.5f);
    hover_enter(env, idB);
    REQUIRE(env.widget(idB)->background_color().r > 0.5f);
}

TEST_CASE("CSS hover: non-:hover declarations in same sheet still apply", "[issue-1323][css-hover]") {
    // The translator should produce a working sheet whose non-hover
    // rules apply at attach time and whose hover rule gates on enter.
    TestEnvironment env;
    env.eval(R"JS(
        var __style = document.createElement('style');
        __style.textContent =
            '.btn { color: black; }' +
            '.btn:hover { background: red; }';
        document.body.appendChild(__style);

        var __btn = document.createElement('div');
        __btn.className = 'btn';
        __btn.style.background = 'blue';
        document.body.appendChild(__btn);
    )JS");

    auto id = std::string(env.engine.evaluate("__btn._id").getWithDefault<std::string_view>(""));
    // `.btn { color: black }` should have applied at attach.
    auto colorProp = std::string(
        env.engine.evaluate("__btn.style._props['color'] || ''")
            .getWithDefault<std::string_view>(""));
    REQUIRE(colorProp == "black");

    // Hover still independently activates the red background.
    hover_enter(env, id);
    REQUIRE(env.widget(id)->background_color().r > 0.5f);
}

// ─────────────────────────────────────────────────────────────────────
// CSS-text parser unit-style coverage
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("CSS hover: _parseCssText returns rule list", "[issue-1323][css-hover][parser]") {
    TestEnvironment env;
    auto count = env.engine.evaluate(
        "_parseCssText('.btn:hover { background: red; } .btn { color: black; }').length")
        .getWithDefault<double>(0.0);
    REQUIRE(count == 2.0);
}

TEST_CASE("CSS hover: _parseCssText kebab-case → camelCase", "[issue-1323][css-hover][parser]") {
    TestEnvironment env;
    auto key = std::string(env.engine.evaluate(
        "(function(){"
        "  var rs = _parseCssText('.x { background-color: red; }');"
        "  var props = rs[0].properties;"
        "  for (var k in props) return k;"
        "  return '';"
        "})()").getWithDefault<std::string_view>(""));
    REQUIRE(key == "backgroundColor");
}
