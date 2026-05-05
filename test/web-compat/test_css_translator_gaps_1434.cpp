// pulp #1434 (batch 3) — CSS translator gap fills.
//
// Verifies that the JS CSS shim (`web-compat-style-decl.js`) routes the
// three previously-dropped property families through to their bridge
// setters end-to-end:
//
//   1. `style.backdropFilter` — parses `blur(Npx)`, calls
//      setBackdropFilter (numeric form). Pre-fix: NOT-IMPL (silent
//      drop). Bridge from pulp #1366 was already wired.
//
//   2. `style.textDecorationLine` / `-Color` / `-Style` — three
//      longhands routed independently so a previously-set sibling is
//      preserved (mirrors PR #1166 finding #4 per-attribute border
//      pattern). Pre-fix: NOT-IMPL (no `case` block).
//
//   3. `style.fontWeight` — keyword forms (`normal` / `bold` /
//      `lighter` / `bolder`) translate to numeric weights before
//      reaching the bridge. Pre-fix: `parseInt('bold')` → NaN → 400
//      default, silently making `bold` look identical to `normal`.
//
// The DOM-shim path (document.createElement → appendChild → style.X = ...)
// is the canonical end-to-end exercise — same pattern as
// test_layout_aspect_ratio.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// Snapshot the native widget id (`_id`) so the C++ side can look the
// widget up by the same key the JS shim used. Same helper as
// test_layout_aspect_ratio.cpp — kept local so this file is self-
// contained for [issue-1434-batch-3] coverage.
static std::string native_id(TestEnvironment& env, const std::string& js_var) {
    auto v = env.engine.evaluate(js_var + "._id");
    return std::string(v.getWithDefault<std::string_view>(""));
}

// ═══════════════════════════════════════════════════════════════════════════════
// 1. backdrop-filter
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS backdropFilter: blur(10px) reaches setBackdropFilter as numeric",
          "[css][shim][backdropFilter][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var bdf = document.createElement('div');
        document.body.appendChild(bdf);
        bdf.style.backdropFilter = "blur(10px)";
    )JS");
    auto id = native_id(env, "bdf");
    REQUIRE_FALSE(id.empty());
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE_THAT(w->backdrop_blur(), WithinAbs(10.0f, 1e-5f));
}

TEST_CASE("CSS backdropFilter: blur(0) clears the slot",
          "[css][shim][backdropFilter][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var bdf2 = document.createElement('div');
        document.body.appendChild(bdf2);
        bdf2.style.backdropFilter = "blur(8px)";
        bdf2.style.backdropFilter = "blur(0)";
    )JS");
    auto* w = env.widget(native_id(env, "bdf2"));
    REQUIRE(w != nullptr);
    REQUIRE(w->backdrop_blur() == 0.0f);
}

TEST_CASE("CSS backdropFilter: 'none' clears the slot",
          "[css][shim][backdropFilter][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var bdf3 = document.createElement('div');
        document.body.appendChild(bdf3);
        bdf3.style.backdropFilter = "blur(6px)";
        bdf3.style.backdropFilter = "none";
    )JS");
    auto* w = env.widget(native_id(env, "bdf3"));
    REQUIRE(w != nullptr);
    REQUIRE(w->backdrop_blur() == 0.0f);
}

TEST_CASE("CSS backdropFilter: kebab-case via setProperty",
          "[css][shim][backdropFilter][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var bdf4 = document.createElement('div');
        document.body.appendChild(bdf4);
        bdf4.style.setProperty("backdrop-filter", "blur(15px)");
    )JS");
    auto* w = env.widget(native_id(env, "bdf4"));
    REQUIRE(w != nullptr);
    REQUIRE_THAT(w->backdrop_blur(), WithinAbs(15.0f, 1e-5f));
}

TEST_CASE("CSS backdropFilter: unsupported filter functions are ignored (no crash)",
          "[css][shim][backdropFilter][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var bdf5 = document.createElement('div');
        document.body.appendChild(bdf5);
        bdf5.style.backdropFilter = "brightness(0.5) contrast(1.2)";
    )JS");
    auto* w = env.widget(native_id(env, "bdf5"));
    REQUIRE(w != nullptr);
    // No blur() function → bridge is not called → backdrop_blur stays 0.
    REQUIRE(w->backdrop_blur() == 0.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 2. text-decoration longhands (line / color / style)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS textDecorationLine: routes 'underline' through setTextDecoration",
          "[css][shim][textDecoration][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var tdl = document.createElement('span');
        document.body.appendChild(tdl);
        tdl.style.textDecorationLine = "underline";
    )JS");
    auto* l = dynamic_cast<Label*>(env.widget(native_id(env, "tdl")));
    REQUIRE(l != nullptr);
    REQUIRE(l->text_decoration() == Label::TextDecoration::underline);
}

TEST_CASE("CSS textDecorationLine: 'line-through' / 'overline' / 'none' all route",
          "[css][shim][textDecoration][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var a = document.createElement('span');
        var b = document.createElement('span');
        var c = document.createElement('span');
        document.body.appendChild(a);
        document.body.appendChild(b);
        document.body.appendChild(c);
        a.style.textDecorationLine = "line-through";
        b.style.textDecorationLine = "overline";
        c.style.textDecorationLine = "underline";
        c.style.textDecorationLine = "none";
    )JS");
    auto* la = dynamic_cast<Label*>(env.widget(native_id(env, "a")));
    auto* lb = dynamic_cast<Label*>(env.widget(native_id(env, "b")));
    auto* lc = dynamic_cast<Label*>(env.widget(native_id(env, "c")));
    REQUIRE(la);
    REQUIRE(lb);
    REQUIRE(lc);
    REQUIRE(la->text_decoration() == Label::TextDecoration::line_through);
    REQUIRE(lb->text_decoration() == Label::TextDecoration::overline);
    REQUIRE(lc->text_decoration() == Label::TextDecoration::none);
}

TEST_CASE("CSS textDecorationColor: hex sets the decoration color",
          "[css][shim][textDecoration][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var tdc = document.createElement('span');
        document.body.appendChild(tdc);
        tdc.style.textDecorationLine = "underline";
        tdc.style.textDecorationColor = "#ff0000";
    )JS");
    auto* l = dynamic_cast<Label*>(env.widget(native_id(env, "tdc")));
    REQUIRE(l);
    REQUIRE(l->has_text_decoration_color());
    auto c = l->text_decoration_color();
    // Pure red. canvas::Color stores float channels [0,1]; r8/g8/b8 round
    // to the 8-bit form for hex parity.
    REQUIRE(c.r8() == 255);
    REQUIRE(c.g8() == 0);
    REQUIRE(c.b8() == 0);
}

TEST_CASE("CSS textDecorationStyle: all 5 spec values map to enum",
          "[css][shim][textDecoration][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var s1 = document.createElement('span');
        var s2 = document.createElement('span');
        var s3 = document.createElement('span');
        var s4 = document.createElement('span');
        var s5 = document.createElement('span');
        document.body.appendChild(s1);
        document.body.appendChild(s2);
        document.body.appendChild(s3);
        document.body.appendChild(s4);
        document.body.appendChild(s5);
        s1.style.textDecorationStyle = "solid";
        s2.style.textDecorationStyle = "double";
        s3.style.textDecorationStyle = "dotted";
        s4.style.textDecorationStyle = "dashed";
        s5.style.textDecorationStyle = "wavy";
    )JS");
    auto get_style = [&](const char* name) {
        auto* l = dynamic_cast<Label*>(env.widget(native_id(env, name)));
        REQUIRE(l);
        return l->text_decoration_style();
    };
    REQUIRE(get_style("s1") == Label::TextDecorationStyle::solid);
    REQUIRE(get_style("s2") == Label::TextDecorationStyle::double_);
    REQUIRE(get_style("s3") == Label::TextDecorationStyle::dotted);
    REQUIRE(get_style("s4") == Label::TextDecorationStyle::dashed);
    REQUIRE(get_style("s5") == Label::TextDecorationStyle::wavy);
}

TEST_CASE("CSS textDecoration longhands compose: line + color + style independently",
          "[css][shim][textDecoration][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var combo = document.createElement('span');
        document.body.appendChild(combo);
        combo.style.textDecorationLine = "underline";
        combo.style.textDecorationColor = "#00ff00";
        combo.style.textDecorationStyle = "dashed";
    )JS");
    auto* l = dynamic_cast<Label*>(env.widget(native_id(env, "combo")));
    REQUIRE(l);
    REQUIRE(l->text_decoration() == Label::TextDecoration::underline);
    REQUIRE(l->has_text_decoration_color());
    auto c = l->text_decoration_color();
    REQUIRE(c.r8() == 0);
    REQUIRE(c.g8() == 255);
    REQUIRE(c.b8() == 0);
    REQUIRE(l->text_decoration_style() == Label::TextDecorationStyle::dashed);
}

TEST_CASE("CSS textDecorationColor: setting color preserves previously-set line",
          "[css][shim][textDecoration][issue-1434-batch-3]") {
    // Per-attribute longhand parity test — same idea as the per-side
    // border fix from PR #1166 finding #4. Setting color must NOT clobber
    // line.
    TestEnvironment env;
    env.eval(R"JS(
        var preserve = document.createElement('span');
        document.body.appendChild(preserve);
        preserve.style.textDecorationLine = "line-through";
        preserve.style.textDecorationColor = "#0000ff";
    )JS");
    auto* l = dynamic_cast<Label*>(env.widget(native_id(env, "preserve")));
    REQUIRE(l);
    REQUIRE(l->text_decoration() == Label::TextDecoration::line_through);
    REQUIRE(l->has_text_decoration_color());
}

TEST_CASE("CSS textDecorationStyle: kebab-case via setProperty",
          "[css][shim][textDecoration][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var k = document.createElement('span');
        document.body.appendChild(k);
        k.style.setProperty("text-decoration-style", "wavy");
    )JS");
    auto* l = dynamic_cast<Label*>(env.widget(native_id(env, "k")));
    REQUIRE(l);
    REQUIRE(l->text_decoration_style() == Label::TextDecorationStyle::wavy);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 3. font-weight keyword translation
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CSS fontWeight: 'normal' keyword maps to 400",
          "[css][shim][fontWeight][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var fw = document.createElement('span');
        document.body.appendChild(fw);
        fw.style.fontWeight = "normal";
    )JS");
    auto* l = dynamic_cast<Label*>(env.widget(native_id(env, "fw")));
    REQUIRE(l);
    REQUIRE(l->font_weight() == 400);
}

TEST_CASE("CSS fontWeight: 'bold' keyword maps to 700",
          "[css][shim][fontWeight][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var fwb = document.createElement('span');
        document.body.appendChild(fwb);
        fwb.style.fontWeight = "bold";
    )JS");
    auto* l = dynamic_cast<Label*>(env.widget(native_id(env, "fwb")));
    REQUIRE(l);
    REQUIRE(l->font_weight() == 700);
}

TEST_CASE("CSS fontWeight: 'lighter' maps to 300, 'bolder' maps to 700",
          "[css][shim][fontWeight][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var lighter = document.createElement('span');
        var bolder = document.createElement('span');
        document.body.appendChild(lighter);
        document.body.appendChild(bolder);
        lighter.style.fontWeight = "lighter";
        bolder.style.fontWeight = "bolder";
    )JS");
    auto* a = dynamic_cast<Label*>(env.widget(native_id(env, "lighter")));
    auto* b = dynamic_cast<Label*>(env.widget(native_id(env, "bolder")));
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a->font_weight() == 300);
    REQUIRE(b->font_weight() == 700);
}

TEST_CASE("CSS fontWeight: numeric values still flow through (no regression)",
          "[css][shim][fontWeight][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var n100 = document.createElement('span');
        var n500 = document.createElement('span');
        var n900 = document.createElement('span');
        document.body.appendChild(n100);
        document.body.appendChild(n500);
        document.body.appendChild(n900);
        n100.style.fontWeight = 100;
        n500.style.fontWeight = "500";
        n900.style.fontWeight = 900;
    )JS");
    auto fw = [&](const char* name) {
        auto* l = dynamic_cast<Label*>(env.widget(native_id(env, name)));
        REQUIRE(l);
        return l->font_weight();
    };
    REQUIRE(fw("n100") == 100);
    REQUIRE(fw("n500") == 500);
    REQUIRE(fw("n900") == 900);
}

TEST_CASE("CSS fontWeight: case-insensitive keyword forms still translate",
          "[css][shim][fontWeight][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var upr = document.createElement('span');
        document.body.appendChild(upr);
        upr.style.fontWeight = "BOLD";
    )JS");
    auto* l = dynamic_cast<Label*>(env.widget(native_id(env, "upr")));
    REQUIRE(l);
    REQUIRE(l->font_weight() == 700);
}

TEST_CASE("CSS fontWeight: kebab-case via setProperty works",
          "[css][shim][fontWeight][issue-1434-batch-3]") {
    TestEnvironment env;
    env.eval(R"JS(
        var kbb = document.createElement('span');
        document.body.appendChild(kbb);
        kbb.style.setProperty("font-weight", "bold");
    )JS");
    auto* l = dynamic_cast<Label*>(env.widget(native_id(env, "kbb")));
    REQUIRE(l);
    REQUIRE(l->font_weight() == 700);
}
