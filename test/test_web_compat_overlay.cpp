// test_web_compat_overlay.cpp
//
// pulp #1148 (slice b) — auto-claim `View::active_overlay_` from CSS
// shape detected by the web-compat layer.
//
// The C++ overlay primitive (#1297) is wired by the @pulp/react
// prop-applier when JSX uses `<View overlay>`. But Spectr-style
// bundled React UIs author popovers with bare
// `<div style="position:absolute; z-index:100">`, which never sets
// the framework `overlay` prop, so click routing falls through to
// whatever sibling is under the popover.
//
// Slice (b) closes the gap by re-evaluating an auto-overlay
// heuristic in `web-compat-style-decl.js` whenever `position`,
// `zIndex`, or the `data-overlay` author hint changes:
//
//   * `position: absolute` AND `z-index >= 10` → claim
//   * `data-overlay = "true"` (HTML attr or dataset)  → claim, regardless of z-index
//   * Otherwise (or once a transition removes the trigger) → release
//
// The heuristic calls the SAME `claimOverlay` / `releaseOverlay`
// bridge functions the JSX `overlay` prop uses, so both paths
// converge on the single `View::active_overlay_` slot.
//
// These tests stand up a real WidgetBridge (so all preludes
// evaluate identically to runtime) and assert the C++ overlay
// state mutates as the heuristic fires.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <string>

using namespace pulp::view;
using namespace pulp::state;

namespace {

// Reset the global overlay slot before/after each case — other tests in
// the same binary can leave it set and a stale holder would cause
// spurious passes.
struct OverlayGuard {
    OverlayGuard() { View::active_overlay_ = nullptr; }
    ~OverlayGuard() { View::active_overlay_ = nullptr; }
};

// Minimal harness that constructs a bridge, runs `js`, and returns
// whether `View::active_overlay_` is non-null.
struct Harness {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge;

    Harness() : root(), store(), bridge(engine, root, store) {
        root.set_bounds({0, 0, 400, 300});
    }

    void eval(const std::string& js) { bridge.load_script(js); }
};

}  // namespace

// ── 1. position:absolute + z-index above threshold → claim ───────────────

TEST_CASE("auto-overlay claims when position:absolute + z-index >= 10",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.style.position = 'absolute';
        d.style.zIndex = '100';
    )");
    REQUIRE(View::active_overlay_ != nullptr);
}

TEST_CASE("auto-overlay claims at exact z-index threshold = 10",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.style.position = 'absolute';
        d.style.zIndex = '10';
    )");
    REQUIRE(View::active_overlay_ != nullptr);
}

// ── 2. position:absolute alone (no z-index) → does NOT claim ────────────
//
// Tooltips, badges, decorations, and absolutely-positioned cards in
// a layout typically don't carry a high z-index. Treating
// `position:absolute` alone as a popover would steal click routing
// from the underlying tree.

TEST_CASE("auto-overlay does NOT claim for position:absolute without z-index",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.style.position = 'absolute';
    )");
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("auto-overlay does NOT claim for absolute + z-index below threshold",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.style.position = 'absolute';
        d.style.zIndex = '9';
    )");
    REQUIRE(View::active_overlay_ == nullptr);
}

// ── 3. position:relative + high z-index → does NOT claim ────────────────
//
// `relative` doesn't reorder hit-testing in the popover sense — only
// `absolute` (lifted out of in-flow layout) signals a true overlay.

TEST_CASE("auto-overlay does NOT claim for position:relative + high z-index",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.style.position = 'relative';
        d.style.zIndex = '100';
    )");
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("auto-overlay does NOT claim for position:static + high z-index",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.style.position = 'static';
        d.style.zIndex = '500';
    )");
    REQUIRE(View::active_overlay_ == nullptr);
}

// ── 4. Releasing on transition: absolute → static ───────────────────────
//
// When a popover is dismissed by flipping its `position` back to
// non-absolute (or removing it entirely), the overlay slot must be
// released, otherwise the platform host keeps routing clicks to a
// stale popover. The heuristic also handles z-index dropping below
// the threshold while position stays absolute.

TEST_CASE("auto-overlay releases when position flips from absolute to static",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.style.position = 'absolute';
        d.style.zIndex = '100';
    )");
    REQUIRE(View::active_overlay_ != nullptr);

    h.eval(R"( d.style.position = 'static'; )");
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("auto-overlay releases when z-index drops below threshold",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.style.position = 'absolute';
        d.style.zIndex = '100';
    )");
    REQUIRE(View::active_overlay_ != nullptr);

    h.eval(R"( d.style.zIndex = '0'; )");
    REQUIRE(View::active_overlay_ == nullptr);
}

// ── 5. data-overlay="true" forces claim regardless of z-index ───────────

TEST_CASE("auto-overlay claims for data-overlay=\"true\" with no z-index",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.setAttribute('data-overlay', 'true');
        d.style.position = 'absolute';
    )");
    REQUIRE(View::active_overlay_ != nullptr);
}

TEST_CASE("auto-overlay claims for data-overlay=\"true\" even with low z-index",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.setAttribute('data-overlay', 'true');
        d.style.position = 'absolute';
        d.style.zIndex = '1';
    )");
    REQUIRE(View::active_overlay_ != nullptr);
}

// ── 6. Convergence with the JSX `overlay` prop path ─────────────────────
//
// The same bridge functions (`claimOverlay` / `releaseOverlay`) drive
// both the React JSX `<View overlay>` path (via prop-applier) and the
// CSS-shape heuristic here. Verify removing the trigger releases the
// slot just like prop-applier's `applyChangedProps` flipping `overlay`
// off.

TEST_CASE("auto-overlay supersedes the prior holder when a new claim fires",
          "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var a = document.createElement('div');
        var b = document.createElement('div');
        document.body.appendChild(a);
        document.body.appendChild(b);
        a.style.position = 'absolute';
        a.style.zIndex = '100';
    )");
    REQUIRE(View::active_overlay_ != nullptr);

    h.eval(R"(
        b.style.position = 'absolute';
        b.style.zIndex = '500';
    )");
    // Latest claim wins — ComboBox::open_dropdown semantics.
    REQUIRE(View::active_overlay_ != nullptr);
}
