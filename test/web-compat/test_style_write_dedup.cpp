// Style-write dedup — a write that reproduces the value already applied must
// not cross the bridge, and every path that changes widget state underneath the
// cache must drop it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

#include <chrono>
#include <iostream>

using namespace pulp::test;
using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

// `_applyProperty` runs the dedup guard and then dispatches to the domain
// handlers, of which `_applyLayoutProp` is always the first. So it is called
// exactly once per property that got past the guard, whatever domain the
// property belongs to — which makes it an exact counter for "reached the
// bridge" without wrapping any native global.
constexpr const char* kInstrument = R"JS(
var __applies = 0;
var __origLayout = _applyLayoutProp;
_applyLayoutProp = function(decl, id, key, resolved, value) {
    __applies++;
    return __origLayout(decl, id, key, resolved, value);
};
)JS";

constexpr const char* kMakeElement = R"JS(
var __el = document.createElement('div');
document.body.appendChild(__el);
)JS";

int applies(TestEnvironment& env) {
    return static_cast<int>(env.engine.evaluate("__applies").getWithDefault<double>(-1.0));
}

View* element(TestEnvironment& env) {
    auto id = std::string(env.engine.evaluate("__el._id").getWithDefault<std::string_view>(""));
    REQUIRE_FALSE(id.empty());
    return env.widget(id);
}

} // namespace

TEST_CASE("StyleDedup: repeating a value does not reach the bridge", "[style][dedup]") {
    TestEnvironment env;
    env.run(kMakeElement);
    env.eval(kInstrument);

    env.eval("__el.style.opacity = '0.5';");
    REQUIRE(applies(env) == 1);

    // Same value, three more times. A stylesheet re-application pass writes
    // every matched declaration whether or not anything changed; this is the
    // case that dominates during an interaction.
    env.eval("__el.style.opacity = '0.5'; __el.style.opacity = '0.5'; __el.style.opacity = '0.5';");
    REQUIRE(applies(env) == 1);

    auto* v = element(env);
    REQUIRE(v != nullptr);
    REQUIRE_THAT(v->opacity(), WithinAbs(0.5f, 0.001f));
}

TEST_CASE("StyleDedup: distinct values all land", "[style][dedup]") {
    TestEnvironment env;
    env.run(kMakeElement);
    env.eval(kInstrument);

    env.eval("__el.style.opacity = '0.5';");
    env.eval("__el.style.opacity = '0.25';");
    REQUIRE(applies(env) == 2);

    auto* v = element(env);
    REQUIRE(v != nullptr);
    REQUIRE_THAT(v->opacity(), WithinAbs(0.25f, 0.001f));
}

TEST_CASE("StyleDedup: an aliasing property invalidates the cache", "[style][dedup]") {
    TestEnvironment env;
    env.run(kMakeElement);

    // `visibility` and `opacity` are different CSS properties that drive the
    // same widget state (setVisibility maps hidden -> opacity 0). Caching them
    // independently would let the second `opacity: 0.5` be skipped as
    // unchanged while the widget actually sits at opacity 0.
    env.eval("__el.style.opacity = '0.5';");
    env.eval("__el.style.visibility = 'hidden';");

    auto* v = element(env);
    REQUIRE(v != nullptr);
    REQUIRE_THAT(v->opacity(), WithinAbs(0.0f, 0.001f));

    env.eval("__el.style.opacity = '0.5';");
    REQUIRE_THAT(v->opacity(), WithinAbs(0.5f, 0.001f));
}

TEST_CASE("StyleDedup: var() values are never deduped", "[style][dedup]") {
    TestEnvironment env;
    env.run(kMakeElement);
    env.eval(kInstrument);

    // The same raw string resolves against live theme tokens, so it can
    // produce a different applied value with no write to observe.
    env.eval("__el.style.opacity = 'var(--fade)';");
    env.eval("__el.style.opacity = 'var(--fade)';");
    REQUIRE(applies(env) == 2);
}

TEST_CASE("StyleDedup: a recreated widget drops the cache", "[style][dedup]") {
    TestEnvironment env;
    env.run(kMakeElement);

    env.eval("__el.style.opacity = '0.5';");
    REQUIRE_THAT(element(env)->opacity(), WithinAbs(0.5f, 0.001f));

    // Detach and re-attach: the native widget is destroyed and rebuilt at
    // default opacity, so the cached "0.5 is already applied" no longer
    // describes anything real.
    env.run("document.body.removeChild(__el); document.body.appendChild(__el);");

    env.eval("__el.style.opacity = '0.5';");
    auto* v = element(env);
    REQUIRE(v != nullptr);
    REQUIRE_THAT(v->opacity(), WithinAbs(0.5f, 0.001f));
}

TEST_CASE("StyleDedup: clearing a property is not deduped away", "[style][dedup]") {
    TestEnvironment env;
    env.run(kMakeElement);
    env.eval(kInstrument);

    env.eval("__el.style.opacity = '0.5';");
    env.eval("__el.style.opacity = '';");
    REQUIRE(applies(env) == 2);

    // removeProperty drops the cache entry too, so re-setting the value it
    // removed is treated as a fresh write rather than matched against it.
    env.eval("__el.style.opacity = '0.5'; __el.style.removeProperty('opacity'); __el.style.opacity = '0.5';");
    REQUIRE(applies(env) == 4);
}

// ── Measurement ──────────────────────────────────────────────────────────────
//
// A/B inside one binary, one machine, one moment: the "before" arm neutralises
// the cache from JS (a `_applied` accessor that always reads empty and discards
// writes) so every write takes the pre-change path, while the "after" arm uses
// the real cache. Comparing two builds instead would put the two arms minutes
// apart on a machine running other agents' builds, where absolute times swing
// with load; the ratio here is measured under one load sample. The cost the
// baseline arm adds over true pre-change code is one empty-object allocation
// per write, which is small next to the var() resolution, per-property parsing
// and bridge call it stands in for -- so it understates the real speedup
// slightly rather than flattering it.

namespace {

// Mirrors what a stylesheet re-application pass does during a drag: every
// matched declaration is rewritten on every frame, while almost nothing about
// the frame actually changed.
constexpr const char* kBenchSetup = R"JS(
var __els = [];
for (var i = 0; i < 40; i++) {
    var e = document.createElement('div');
    document.body.appendChild(e);
    __els.push(e);
}
var __props = ['color', 'backgroundColor', 'fontFamily', 'fontSize', 'fontWeight',
               'opacity', 'width', 'height', 'paddingTop', 'marginTop',
               'borderRadius', 'textAlign'];
var __vals = ['#ff8800', '#101418', 'Inter', '13px', '600',
              '1', '120px', '24px', '4px', '2px',
              '3px', 'left'];
function __frame(n) {
    for (var i = 0; i < __els.length; i++) {
        var s = __els[i].style;
        for (var p = 0; p < __props.length; p++) s[__props[p]] = __vals[p];
    }
    // One element genuinely moves, as the dragged band does.
    __els[0].style.left = (n % 50) + 'px';
}
function __neutraliseCache(on) {
    for (var i = 0; i < __els.length; i++) {
        if (on) {
            Object.defineProperty(__els[i].style, '_applied', {
                get: function() { return {}; },
                set: function(v) {},
                configurable: true
            });
        } else {
            delete __els[i].style._applied;
            __els[i].style._applied = {};
        }
    }
}
)JS";

double time_frames(TestEnvironment& env, int frames) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; i++)
        env.eval("__frame(" + std::to_string(i) + ");");
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

} // namespace

TEST_CASE("StyleDedup: repeated declarations stop reaching the bridge", "[style][dedup][benchmark]") {
    TestEnvironment env;
    env.run(kBenchSetup);
    env.eval(kInstrument);

    constexpr int kFrames = 40;

    // Warm first, and warm the arm that is measured second-to-none: without
    // this the baseline arm pays first-run costs (property-map growth, string
    // interning) that the dedup arm would not, flattering the ratio.
    env.eval("__neutraliseCache(true);");
    time_frames(env, 5);

    env.eval("__neutraliseCache(true); __applies = 0;");
    double baseline_ms = time_frames(env, kFrames);
    int baseline_applies = applies(env);

    env.eval("__neutraliseCache(false); __applies = 0;");
    double dedup_ms = time_frames(env, kFrames);
    int dedup_applies = applies(env);

    std::cout << "\n[style-dedup] frames=" << kFrames
              << "  baseline: " << baseline_applies << " applies, "
              << baseline_ms << " ms (" << baseline_ms / kFrames << " ms/frame)"
              << "  dedup: " << dedup_applies << " applies, "
              << dedup_ms << " ms (" << dedup_ms / kFrames << " ms/frame)"
              << "  applies ratio: "
              << (dedup_applies ? double(baseline_applies) / dedup_applies : 0.0)
              << "x\n";

    // The apply count is the load-independent claim: unchanged declarations
    // stop reaching the bridge entirely, leaving only the element that moved.
    // 40 elements x 12 declarations, plus the one element that also moves.
    REQUIRE(baseline_applies == (40 * 12 + 1) * kFrames);
    REQUIRE(dedup_applies < baseline_applies / 10);
}
