// test_canvas2d_dommatrix.cpp — direct unit tests for the
// _PulpCanvasMatrix DOMMatrix-compat helper extracted to
// web-compat-canvas-matrix.js in #2253 (P5-6 follow-up).
//
// Why this file exists: the matrix class is currently only exercised
// indirectly via CanvasRenderingContext2D.getTransform() in
// test_canvas2d_shim_late.cpp. Bugs in the matrix arithmetic itself
// (multiplySelf composition, scaleSelf invariants, rotateSelf
// angle handling, inverse() singular-input detection) would silently
// corrupt every getTransform/setTransform chain in the runtime —
// the indirect tests catch end-to-end regressions but not arithmetic
// drift like sign errors or off-by-one degree/radian confusion.
//
// These tests drive _PulpCanvasMatrix directly through QuickJS via
// the same WidgetBridge harness the rest of test_canvas2d_shim.cpp
// uses, so they assert the EXACT JS-runtime semantics (no manual
// C++ port of the arithmetic), and pin properties like:
//   - identity construction sets is2D=true, isIdentity=true
//   - scale * scale composes correctly
//   - rotate(180°) flips signs as expected (DOMMatrix spec: degrees)
//   - inverse of a singular matrix returns NaN-filled is2D=false
//   - multiplySelf chains accumulate (not last-write-wins)
//   - is2D stays true through 2D-only ops, flips to false on inverse failure

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <string>

using namespace pulp::view;
using namespace pulp::state;

namespace {

// Drive a JS expression through the bridge and return its String()
// coercion. Mirror of run_in_bridge from test_canvas2d_shim.cpp —
// duplicated here for split-self-containment.
std::string eval_in_bridge(const std::string& js) {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("globalThis.__test_result__ = (function(){\n"
                       + js +
                       "\n})();");
    auto val = engine.evaluate("String(globalThis.__test_result__)");
    if (val.isString()) return std::string(val.getString());
    return std::string{};
}

// Construct a fresh canvas + ctx and return a getTransform() snapshot
// to use as a _PulpCanvasMatrix instance. The class itself isn't
// directly exposed under a global name (it's file-local to
// web-compat-canvas-matrix.js), so getTransform is the legitimate
// way to instantiate one.
std::string ctx_and_matrix_setup() {
    return R"(
        var c = document.createElement('canvas');
        c.width = 200; c.height = 200;
        document.body.appendChild(c);
        globalThis.__test_canvas_el__ = c;
        var ctx = c.getContext('2d');
        // First identity getTransform()
        var m = ctx.getTransform();
    )";
}

} // namespace

// ── Construction / identity ───────────────────────────────────────

TEST_CASE("Canvas2D _PulpCanvasMatrix identity has is2D=true and isIdentity=true",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        return m.is2D + ':' + m.isIdentity + ':' + m.a + ':' + m.b
             + ':' + m.c + ':' + m.d + ':' + m.e + ':' + m.f;
    )");
    REQUIRE(out == "true:true:1:0:0:1:0:0");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix m11..m44 aliases mirror a/b/c/d/e/f",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        return [m.m11, m.m12, m.m21, m.m22, m.m41, m.m42,
                m.m13, m.m14, m.m23, m.m24,
                m.m31, m.m32, m.m33, m.m34,
                m.m43, m.m44].join(',');
    )");
    // 2D affine 1,0,0,1,0,0 + 3D padding (0,0,0,0,0,0,1,0,0,1)
    REQUIRE(out == "1,0,0,1,0,0,0,0,0,0,0,0,1,0,0,1");
}

// ── Mutator semantics ─────────────────────────────────────────────

TEST_CASE("Canvas2D _PulpCanvasMatrix scaleSelf composes uniformly",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        m.scaleSelf(2, 3);
        return m.a + ',' + m.d + ',' + m.isIdentity;
    )");
    // 2 * scaleX, 3 * scaleY, no longer identity
    REQUIRE(out == "2,3,false");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix scaleSelf chain accumulates (not last-write-wins)",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        m.scaleSelf(2, 3).scaleSelf(5, 7);
        return m.a + ',' + m.d;
    )");
    // 2*5=10, 3*7=21 — accumulates. Last-write-wins would yield 5,7.
    REQUIRE(out == "10,21");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix translateSelf preserves diagonal, updates e/f",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        m.translateSelf(10, 20);
        return [m.a, m.b, m.c, m.d, m.e, m.f].join(',');
    )");
    REQUIRE(out == "1,0,0,1,10,20");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix translateSelf chain accumulates",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        m.translateSelf(10, 20).translateSelf(5, 5);
        return m.e + ',' + m.f;
    )");
    REQUIRE(out == "15,25");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix rotateSelf(0) is a no-op",
          "[canvas2d][dommatrix][issue-1527][issue-1730]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        m.rotateSelf(0);
        // rotateSelf takes DEGREES per DOMMatrix spec.
        // Identity should survive a 0-degree rotation.
        var EPS = 1e-9;
        var ok = Math.abs(m.a - 1) < EPS && Math.abs(m.b) < EPS
              && Math.abs(m.c) < EPS && Math.abs(m.d - 1) < EPS;
        return ok ? 'identity' : (m.a + ',' + m.b + ',' + m.c + ',' + m.d);
    )");
    REQUIRE(out == "identity");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix rotateSelf(180) flips signs (degrees)",
          "[canvas2d][dommatrix][issue-1527][issue-1730]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        m.rotateSelf(180);
        // After 180° rotation, a/d should be ~-1, b/c should be ~0.
        // If the impl took radians by mistake, m.a would be cos(180)≈
        // -0.598 — checking a < -0.9 catches that.
        return (m.a < -0.99) + ',' + (Math.abs(m.b) < 0.01)
             + ',' + (Math.abs(m.c) < 0.01) + ',' + (m.d < -0.99);
    )");
    REQUIRE(out == "true,true,true,true");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix rotateSelf(90) maps 1,0,0,1 to 0,1,-1,0",
          "[canvas2d][dommatrix][issue-1527][issue-1730]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        m.rotateSelf(90);
        // cos(90°)=0, sin(90°)=1.
        var EPS = 1e-9;
        return [Math.abs(m.a) < EPS,
                Math.abs(m.b - 1) < EPS,
                Math.abs(m.c + 1) < EPS,
                Math.abs(m.d) < EPS].join(',');
    )");
    REQUIRE(out == "true,true,true,true");
}

// ── multiplySelf ──────────────────────────────────────────────────

TEST_CASE("Canvas2D _PulpCanvasMatrix multiplySelf composes two matrices",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        var n = ctx.getTransform();
        n.scaleSelf(2, 2);          // n = scale-2
        m.translateSelf(10, 0);     // m = translate-x10
        m.multiplySelf(n);          // m = translate-x10 * scale-2
        // Resulting 2D affine: a=2, b=0, c=0, d=2, e=10, f=0
        return [m.a, m.b, m.c, m.d, m.e, m.f].join(',');
    )");
    REQUIRE(out == "2,0,0,2,10,0");
}

// ── inverse() ─────────────────────────────────────────────────────

TEST_CASE("Canvas2D _PulpCanvasMatrix inverse of identity is identity",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        var inv = m.inverse();
        return [inv.a, inv.b, inv.c, inv.d, inv.e, inv.f, inv.is2D].join(',');
    )");
    REQUIRE(out == "1,0,0,1,0,0,true");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix inverse of singular matrix returns NaN with is2D=false",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        // Build a singular matrix via setTransform(0,0,0,0,0,0) —
        // determinant a*d - b*c = 0*0 - 0*0 = 0 ⇒ singular.
        ctx.setTransform(0, 0, 0, 0, 0, 0);
        var n = ctx.getTransform();
        var inv = n.inverse();
        return [isNaN(inv.a), isNaN(inv.b), isNaN(inv.c),
                isNaN(inv.d), isNaN(inv.e), isNaN(inv.f),
                inv.is2D].join(',');
    )");
    // All six fields NaN, is2D explicitly false (so callers can
    // distinguish "successful 2D inverse" from "singular failure" —
    // the Codex P2 follow-up on #1754).
    REQUIRE(out == "true,true,true,true,true,true,false");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix inverse of a 2x scale is 0.5x scale",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        ctx.setTransform(2, 0, 0, 2, 0, 0);
        var n = ctx.getTransform();
        var inv = n.inverse();
        return [inv.a, inv.d, inv.is2D].join(',');
    )");
    REQUIRE(out == "0.5,0.5,true");
}

// ── Serialization ─────────────────────────────────────────────────

TEST_CASE("Canvas2D _PulpCanvasMatrix toJSON honors actual is2D state (Codex P2 #1754)",
          "[canvas2d][dommatrix][issue-1527][issue-1754]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        ctx.setTransform(0, 0, 0, 0, 0, 0);
        var n = ctx.getTransform();
        var inv = n.inverse();
        var j = inv.toJSON();
        // The Codex P2 follow-up on #1754: toJSON must report is2D
        // false when the inverse was singular (not hard-code true).
        return j.is2D + ',' + j.isIdentity;
    )");
    REQUIRE(out == "false,false");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix toFloat32Array length is 16",
          "[canvas2d][dommatrix][issue-1527]") {
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        return String(m.toFloat32Array().length);
    )");
    REQUIRE(out == "16");
}

TEST_CASE("Canvas2D _PulpCanvasMatrix toFloat32Array serialization order "
          "(non-identity matrix detects transposition — Codex P2 on #2387)",
          "[canvas2d][dommatrix][issue-1527]") {
    // Codex P2: the identity matrix is symmetric, so row-major vs
    // column-major output looks identical. To make ordering bugs
    // observable we use a translate(10,20) × scale(2,3) where the
    // translation lives in m41/m42 and the scale on the diagonal.
    // DOMMatrix uses column-major (WebGL / CSS convention): the
    // expected layout is
    //   col 1: m11=2, m12=0, m13=0, m14=0
    //   col 2: m21=0, m22=3, m23=0, m24=0
    //   col 3: m31=0, m32=0, m33=1, m34=0   (3D padding identity)
    //   col 4: m41=10, m42=20, m43=0, m44=1 (translation)
    // If somebody accidentally outputs row-major, position 3 would be
    // 10 (not 0) and the test fails.
    auto out = eval_in_bridge(ctx_and_matrix_setup() + R"(
        ctx.setTransform(2, 0, 0, 3, 10, 20);
        var n = ctx.getTransform();
        return n.toFloat32Array().join(',');
    )");
    REQUIRE(out == "2,0,0,0,0,3,0,0,0,0,1,0,10,20,0,1");
}
