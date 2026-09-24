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
#include <pulp/view/overlay_dismissal.hpp>
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

// ── 7. Outside-click consumption parity with the JSX `overlay` prop ──────
//
// `claimOverlay(id)` takes an optional second argument that sets
// `View::overlay_consumes_outside_click()`. Omitting it means consume=false,
// so the press that dismisses the overlay ALSO reaches the control beneath
// it. The @pulp/react prop-applier and the web-compat `<select>` popup both
// pass `true`; the CSS-shape auto-claim passed nothing.
//
// The split below is deliberate. `data-overlay="true"` is an explicit author
// statement that the element is a popover — the same statement `<View
// overlay>` makes — so it consumes, matching that path. The CSS-shape branch
// is an inference, and a false positive there would swallow a real click
// outright, so it stays click-through.

TEST_CASE("auto-overlay: data-overlay=\"true\" consumes the outside click",
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
    REQUIRE(View::active_overlay_->overlay_consumes_outside_click());
}

TEST_CASE("auto-overlay: CSS-shape inference does NOT consume the outside "
          "click", "[view][web-compat][issue-1148][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.style.position = 'absolute';
        d.style.zIndex = '100';
    )");
    // Positive control for the negative assertion below: the claim really did
    // fire, so `overlay_consumes_outside_click() == false` is a statement
    // about the consume flag and not about an overlay that never existed.
    REQUIRE(View::active_overlay_ != nullptr);
    REQUIRE_FALSE(View::active_overlay_->overlay_consumes_outside_click());
}

TEST_CASE("auto-overlay: adding the data-overlay hint upgrades an existing "
          "shape claim to consuming",
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
    REQUIRE_FALSE(View::active_overlay_->overlay_consumes_outside_click());

    // The element already holds the slot, so the claim-once guard would skip
    // the bridge call and strand the stale consume value unless the tracked
    // consume flag is part of the re-claim condition.
    h.eval(R"( d.setAttribute('data-overlay', 'true'); )");
    REQUIRE(View::active_overlay_ != nullptr);
    REQUIRE(View::active_overlay_->overlay_consumes_outside_click());
}

// ── data-overlay-trigger marks the control that OPENS an overlay ────────────
//
// Separate from the claim: the trigger is the dropdown FIELD, the claim is on
// the menu it opens. Marking it lets a press meant as "switch menus" reach the
// second dropdown instead of being spent closing the first. Never inferred
// from CSS shape — an inference that marked ordinary content would make
// clicking away from a menu also operate whatever sits under the click.

namespace {

// The bridge keys widgets by its own generated id, not the DOM `id`, so these
// count marks over the built tree instead of looking one up by name.
int count_views(const View& v) {
    int n = 1;
    for (size_t i = 0; i < v.child_count(); ++i) n += count_views(*v.child_at(i));
    return n;
}

int count_overlay_triggers(const View& v) {
    int n = v.overlay_trigger() ? 1 : 0;
    for (size_t i = 0; i < v.child_count(); ++i)
        n += count_overlay_triggers(*v.child_at(i));
    return n;
}

}  // namespace

TEST_CASE("data-overlay-trigger=\"true\" marks the view as an overlay trigger",
          "[view][web-compat][auto-overlay][trigger]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.setAttribute('data-overlay-trigger', 'true');
    )");
    REQUIRE(count_overlay_triggers(h.root) == 1);
}

TEST_CASE("an unmarked element is not an overlay trigger",
          "[view][web-compat][auto-overlay][trigger]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.style.position = 'absolute';
        d.style.zIndex = '100';
    )");
    // Positive control: the element really reached the bridge and built views,
    // so the zero below is about the trigger mark and not about an empty tree.
    REQUIRE(count_views(h.root) > 1);
    REQUIRE(count_overlay_triggers(h.root) == 0);
}

TEST_CASE("removing data-overlay-trigger clears the mark",
          "[view][web-compat][auto-overlay][trigger]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        d.id = 'toggling';
        document.body.appendChild(d);
        d.setAttribute('data-overlay-trigger', 'true');
    )");
    REQUIRE(count_overlay_triggers(h.root) == 1);

    h.eval("document.getElementById('toggling').removeAttribute('data-overlay-trigger');");
    REQUIRE(count_overlay_triggers(h.root) == 0);
}

// ── Escape must NOTIFY the overlay's owner, not just hide it ────────────────
//
// A JS-owned ARIA popup (web-compat-document.js) claims the generalized
// overlay slot on activate and releases it on dismiss, so it IS an open
// overlay as far as the native dismissal policy is concerned. Its React state
// is reconciled by a native `dismiss` event the JS side listens for: without
// that event the popup would be hidden natively while the app still believes
// its menu is open, and the next trigger press would toggle the state back to
// closed with nothing appearing to happen.
//
// `route_escape_to_active_overlay` therefore goes through the dismissal path,
// which fires `on_overlay_dismissed` — the callback `claimOverlay` installs to
// dispatch that event — and never hides anything itself.

TEST_CASE("Escape fires the overlay's native dismiss event for its JS owner",
          "[view][web-compat][auto-overlay][escape]") {
    OverlayGuard g;
    Harness h;
    // The dismiss handler marks a SECOND element as an overlay trigger, which
    // is an observable that already round-trips through the bridge, so the
    // assertion below is about the event actually reaching JS.
    h.eval(R"(
        var witness = document.createElement('div');
        document.body.appendChild(witness);
        var popup = document.createElement('div');
        document.body.appendChild(popup);
        popup.addEventListener('dismiss', function() {
            witness.setAttribute('data-overlay-trigger', 'true');
        });
        popup.setAttribute('data-overlay', 'true');
        popup.style.position = 'absolute';
    )");
    // Positive controls: the claim really fired and nothing has dismissed yet,
    // so the count below is about Escape and not about a listener that was
    // already run or an overlay that never existed.
    REQUIRE(h.root.interaction().active_overlay != nullptr);
    REQUIRE(count_overlay_triggers(h.root) == 0);

    REQUIRE(pulp::view::route_escape_to_active_overlay(h.root) ==
            pulp::view::OverlayEscapeResult::overlay);

    REQUIRE(h.root.interaction().active_overlay == nullptr);
    REQUIRE(count_overlay_triggers(h.root) == 1);
}

TEST_CASE("an outside press fires the overlay's native dismiss event too",
          "[view][web-compat][auto-overlay][pointer]") {
    // Same reconciliation obligation on the press path: the JS owner learns
    // about the dismissal through the event, never by observing a hidden view.
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var witness = document.createElement('div');
        document.body.appendChild(witness);
        var popup = document.createElement('div');
        document.body.appendChild(popup);
        popup.addEventListener('dismiss', function() {
            witness.setAttribute('data-overlay-trigger', 'true');
        });
        popup.setAttribute('data-overlay', 'true');
        popup.style.position = 'absolute';
        popup.style.left = '100px';
        popup.style.top = '100px';
        popup.style.width = '80px';
        popup.style.height = '60px';
    )");
    REQUIRE(h.root.interaction().active_overlay != nullptr);
    REQUIRE(count_overlay_triggers(h.root) == 0);

    const auto press = pulp::view::route_press_to_active_overlay(
        h.root, {10.0f, 10.0f});

    REQUIRE(press.routing == pulp::view::OverlayPressRouting::dismissed);
    REQUIRE(count_overlay_triggers(h.root) == 1);
}

// ── ARIA marks BOTH halves of the pair ─────────────────────────────────────
//
// Same marks, reached from the vocabulary a document that cares about
// assistive technology has already written. `role="menu"|"listbox"|"tree"|
// "grid"|"dialog"|"alertdialog"` and `aria-modal="true"` say "I AM a
// dismissable overlay"; `aria-haspopup` says "I OPEN one". Honouring only the
// trigger half made a correctly-authored app pay two presses to switch menus
// — the first spent closing, the second opening — and left the panel itself to
// the CSS-shape inference, which claims click-through by design, so a press
// outside the menu both closed it and operated whatever sat under the press.
//
// A STATEMENT, not an inference, so it claims with the same outside-click
// consumption `data-overlay="true"` does.

TEST_CASE("an ARIA overlay role claims and consumes its outside press",
          "[view][web-compat][auto-overlay]") {
    // Deliberately WITHOUT the CSS shape: no position:absolute, no z-index. A
    // pass here is about the role, not about the heuristic that already
    // claimed every high-z absolute box.
    for (const std::string role :
         {"menu", "listbox", "tree", "grid", "dialog", "alertdialog"}) {
        OverlayGuard g;
        Harness h;
        h.eval("var d = document.createElement('div');"
               "document.body.appendChild(d);"
               "d.setAttribute('role', '" + role + "');");
        INFO("role=" << role);
        REQUIRE(h.root.interaction().active_overlay != nullptr);
        REQUIRE(h.root.interaction().active_overlay
                    ->overlay_consumes_outside_click());
    }
}

TEST_CASE("a non-overlay ARIA role does not claim",
          "[view][web-compat][auto-overlay]") {
    // The control for the case above: if every role claimed, that test would
    // pass without reading the token at all.
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.setAttribute('role', 'button');
    )");
    REQUIRE(h.root.interaction().active_overlay == nullptr);
}

TEST_CASE("aria-modal=\"true\" claims and \"false\" does not",
          "[view][web-compat][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var yes = document.createElement('div');
        document.body.appendChild(yes);
        yes.setAttribute('aria-modal', 'true');
    )");
    REQUIRE(h.root.interaction().active_overlay != nullptr);

    OverlayGuard g2;
    Harness h2;
    h2.eval(R"(
        var no = document.createElement('div');
        document.body.appendChild(no);
        no.setAttribute('aria-modal', 'false');
    )");
    REQUIRE(h2.root.interaction().active_overlay == nullptr);
}

TEST_CASE("an ARIA overlay role claims when the role is written BEFORE mount",
          "[view][web-compat][auto-overlay]") {
    // React's commit order is setAttribute, THEN appendChild, so the attribute
    // lands while `_nativeCreated` is still false and the immediate
    // re-evaluation is a no-op. Every materialized menu in a React document is
    // in exactly that state, so a claim that only works post-mount does not
    // work at all on the real path.
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        d.setAttribute('role', 'menu');
        document.body.appendChild(d);
    )");
    REQUIRE(h.root.interaction().active_overlay != nullptr);
}

TEST_CASE("removing the ARIA overlay role releases the claim",
          "[view][web-compat][auto-overlay]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.setAttribute('role', 'menu');
    )");
    REQUIRE(h.root.interaction().active_overlay != nullptr);
    h.eval("document.body.children[0].removeAttribute('role');");
    REQUIRE(h.root.interaction().active_overlay == nullptr);
}

TEST_CASE("aria-haspopup marks the view as an overlay trigger",
          "[view][web-compat][auto-overlay][trigger]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        document.body.appendChild(d);
        d.setAttribute('aria-haspopup', 'menu');
    )");
    REQUIRE(count_overlay_triggers(h.root) == 1);
}

TEST_CASE("every aria-haspopup token except \"false\" marks",
          "[view][web-compat][auto-overlay][trigger]") {
    OverlayGuard g;
    Harness h;
    // The ARIA token set, plus the one value that must NOT mark. Built in one
    // tree so the counts are about the tokens and not about six harnesses.
    h.eval(R"(
        ['true', 'menu', 'listbox', 'tree', 'grid', 'dialog', 'false']
            .forEach(function(token) {
                var d = document.createElement('div');
                document.body.appendChild(d);
                d.setAttribute('aria-haspopup', token);
            });
    )");
    // Positive control: seven elements really reached the bridge, so the six
    // below is about the token set and not about a tree that never built.
    REQUIRE(count_views(h.root) > 7);
    REQUIRE(count_overlay_triggers(h.root) == 6);
}

// React commits `setAttribute` BEFORE `appendChild`, so the re-evaluation that
// `setAttribute` triggers runs while the element has no native widget and is a
// no-op. Every freshly mounted button arrives in exactly that state, so without
// a replay at mount the mark would only land for an author who happened to
// write the attribute afterwards — which is nobody using React.
TEST_CASE("aria-haspopup set BEFORE mount still marks (React commit order)",
          "[view][web-compat][auto-overlay][trigger]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        d.setAttribute('aria-haspopup', 'listbox');
        document.body.appendChild(d);
    )");
    REQUIRE(count_views(h.root) > 1);
    REQUIRE(count_overlay_triggers(h.root) == 1);
}

TEST_CASE("removing aria-haspopup clears the mark",
          "[view][web-compat][auto-overlay][trigger]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var d = document.createElement('div');
        d.id = 'menubutton';
        document.body.appendChild(d);
        d.setAttribute('aria-haspopup', 'listbox');
    )");
    REQUIRE(count_overlay_triggers(h.root) == 1);

    h.eval("document.getElementById('menubutton').removeAttribute('aria-haspopup');");
    REQUIRE(count_overlay_triggers(h.root) == 0);
}

// The behaviour the mark exists for, measured through the same policy verb
// every host calls.
//
// `layout_children()` is not optional here and is the whole reason this case
// is worth writing. The style writes below reach Yoga, but nothing lays the
// tree out until asked, so before that call every child measures `0x0` at the
// origin and `hit_test` answers with the body for any point at all. A press
// case written without it cannot distinguish "the trigger was not marked"
// from "the press hit nothing", and reads as a product failure either way.
TEST_CASE("a press on an aria-haspopup trigger switches menus in one press",
          "[view][web-compat][auto-overlay][trigger][pointer]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var trigger = document.createElement('div');
        document.body.appendChild(trigger);
        trigger.setAttribute('aria-haspopup', 'menu');
        trigger.style.position = 'absolute';
        trigger.style.left = '10px';
        trigger.style.top = '10px';
        trigger.style.width = '60px';
        trigger.style.height = '20px';

        var popup = document.createElement('div');
        document.body.appendChild(popup);
        popup.setAttribute('data-overlay', 'true');
        popup.style.position = 'absolute';
        popup.style.left = '200px';
        popup.style.top = '150px';
        popup.style.width = '120px';
        popup.style.height = '80px';
    )");
    h.root.layout_children();

    // Positive controls: an overlay that really claimed, and really consumes.
    // A press that "passed through" an overlay which never consumed anything
    // would prove nothing about the trigger mark.
    REQUIRE(h.root.interaction().active_overlay != nullptr);
    REQUIRE(h.root.interaction().active_overlay->overlay_consumes_outside_click());

    // And a positive control on the press point itself: it lands on the marked
    // control, outside the overlay. Without this the assertion below could
    // pass or fail for reasons that have nothing to do with the mark.
    //
    // Non-fatal on purpose. These say WHY the press below is consumed when it
    // is; aborting the case here would hide the consumed press — the thing the
    // user actually reports — behind its own cause.
    CHECK(count_overlay_triggers(h.root) == 1);
    const Point press_point{40.0f, 20.0f};
    const auto* hit = h.root.hit_test(press_point);
    REQUIRE(hit != nullptr);
    bool hit_is_trigger = false;
    for (const View* node = hit; node != nullptr; node = node->parent())
        hit_is_trigger = hit_is_trigger || node->overlay_trigger();
    CHECK(hit_is_trigger);
    REQUIRE_FALSE(
        h.root.interaction().active_overlay->overlay_contains(press_point));

    // The press dismisses WITHOUT consuming, so the host falls through to the
    // ordinary hit test and the trigger opens its own menu on this same press.
    const auto on_trigger =
        pulp::view::route_press_to_active_overlay(h.root, press_point);
    CHECK(on_trigger.routing == pulp::view::OverlayPressRouting::dismissed);
    CHECK_FALSE(on_trigger.consume_press);
}

TEST_CASE("a press on ordinary content still consumes the dismissal (web-compat)",
          "[view][web-compat][auto-overlay][trigger][pointer]") {
    // The negative control for the case above, in the direction that matters:
    // the pass-through is scoped to triggers. Widened to every dismissing
    // press, closing a menu would also operate whatever sits under the click.
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var plain = document.createElement('div');
        document.body.appendChild(plain);
        plain.style.position = 'absolute';
        plain.style.left = '10px';
        plain.style.top = '10px';
        plain.style.width = '60px';
        plain.style.height = '20px';

        var popup = document.createElement('div');
        document.body.appendChild(popup);
        popup.setAttribute('data-overlay', 'true');
        popup.style.position = 'absolute';
        popup.style.left = '200px';
        popup.style.top = '150px';
        popup.style.width = '120px';
        popup.style.height = '80px';
    )");
    h.root.layout_children();

    REQUIRE(h.root.interaction().active_overlay != nullptr);
    REQUIRE(h.root.interaction().active_overlay->overlay_consumes_outside_click());
    REQUIRE(count_overlay_triggers(h.root) == 0);

    // Same point, same geometry, same instrument as the case above — the ONLY
    // difference is that this element declares no `aria-haspopup`. That is
    // what makes the two a matched pair rather than two unrelated readings.
    const Point press_point{40.0f, 20.0f};
    const auto* hit = h.root.hit_test(press_point);
    REQUIRE(hit != nullptr);
    for (const View* node = hit; node != nullptr; node = node->parent())
        CHECK_FALSE(node->overlay_trigger());

    const auto on_plain =
        pulp::view::route_press_to_active_overlay(h.root, press_point);
    CHECK(on_plain.routing == pulp::view::OverlayPressRouting::dismissed);
    CHECK(on_plain.consume_press);
}

// ── A lifted submenu declares the overlay it stacks on ────────────────────
//
// `role="menu"` is an author STATEMENT and claims for it, which is what makes
// a correctly-described menu dismissable without any Pulp-specific attribute.
// But the statement is equally true of a menu and of its submenu, and a
// submenu placed to escape its menu's box is emitted as a SIBLING of that menu
// (`position: fixed`, a portal, a returned fragment), so the native
// parent-chain test reads it as a rival: opening the submenu dismisses the
// menu underneath, taking every row on both with it.
//
// The missing fact is which overlay the submenu belongs to, and nothing in the
// markup that already claims can supply it. So it is declared — in Pulp's own
// vocabulary with `data-overlay-parent` on the submenu, or in ARIA's with
// `aria-owns` on the menu, which is the attribute ARIA provides for exactly
// this case: a parent/child relationship the DOM hierarchy cannot represent.

TEST_CASE("a lifted role=menu panel that declares data-overlay-parent stacks",
          "[view][web-compat][auto-overlay][lifted]") {
    OverlayGuard g;
    Harness h;
    h.eval(R"(
        var menu = document.createElement('div');
        menu.id = 'band-menu';
        menu.setAttribute('role', 'menu');
        document.body.appendChild(menu);

        var sub = document.createElement('div');
        sub.setAttribute('role', 'menu');
        sub.setAttribute('data-overlay-parent', 'band-menu');
        document.body.appendChild(sub);
    )");
    // Both are children of body, so neither is an ancestor of the other.
    REQUIRE(h.root.overlay_depth() == 2);
    REQUIRE(h.root.interaction().active_overlay != nullptr);
    REQUIRE(h.root.interaction().active_overlay->overlay_consumes_outside_click());
}

TEST_CASE("a lifted role=menu panel that declares nothing replaces the menu",
          "[view][web-compat][auto-overlay][lifted]") {
    OverlayGuard g;
    Harness h;
    // The control. Identical markup minus the declaration — so the case above
    // cannot pass by claiming twice, by never claiming, or by the ARIA role
    // having stopped claiming at all. This is also the exact shape of the
    // reported bug: two sibling `role="menu"` panels, and the first is gone.
    h.eval(R"(
        var menu = document.createElement('div');
        menu.id = 'band-menu';
        menu.setAttribute('role', 'menu');
        document.body.appendChild(menu);

        var sub = document.createElement('div');
        sub.setAttribute('role', 'menu');
        document.body.appendChild(sub);
    )");
    REQUIRE(h.root.overlay_depth() == 1);
}

TEST_CASE("a declared overlay parent that names no element does not stack",
          "[view][web-compat][auto-overlay][lifted]") {
    OverlayGuard g;
    Harness h;
    // A typo, or an id that has since unmounted, resolves to no widget on the
    // native side. That is the undeclared claim, never a licence to sit on top
    // of whatever happened to be open.
    h.eval(R"(
        var menu = document.createElement('div');
        menu.id = 'band-menu';
        menu.setAttribute('role', 'menu');
        document.body.appendChild(menu);

        var sub = document.createElement('div');
        sub.setAttribute('role', 'menu');
        sub.setAttribute('data-overlay-parent', 'no-such-menu');
        document.body.appendChild(sub);
    )");
    REQUIRE(h.root.overlay_depth() == 1);
}

TEST_CASE("aria-owns on the menu is enough for a lifted submenu to stack",
          "[view][web-compat][auto-overlay][lifted]") {
    OverlayGuard g;
    Harness h;
    // No Pulp-specific attribute anywhere: a document that lifted its submenu
    // out of the menu's subtree AND repaired that for assistive technology has
    // already said everything the stacking rule needs.
    h.eval(R"(
        var menu = document.createElement('div');
        menu.setAttribute('role', 'menu');
        menu.setAttribute('aria-owns', 'mod-panel');
        document.body.appendChild(menu);

        var sub = document.createElement('div');
        sub.id = 'mod-panel';
        sub.setAttribute('role', 'menu');
        document.body.appendChild(sub);
    )");
    REQUIRE(h.root.overlay_depth() == 2);
}

TEST_CASE("aria-owns naming a different id does not stack",
          "[view][web-compat][auto-overlay][lifted]") {
    OverlayGuard g;
    Harness h;
    // The control for the case above: if the reverse lookup matched any
    // element carrying `aria-owns` at all, that test would pass without ever
    // comparing the token to the submenu's id.
    h.eval(R"(
        var menu = document.createElement('div');
        menu.setAttribute('role', 'menu');
        menu.setAttribute('aria-owns', 'some-other-panel');
        document.body.appendChild(menu);

        var sub = document.createElement('div');
        sub.id = 'mod-panel';
        sub.setAttribute('role', 'menu');
        document.body.appendChild(sub);
    )");
    REQUIRE(h.root.overlay_depth() == 1);
}

TEST_CASE("declaring a parent is not itself a claim and survives to one",
          "[view][web-compat][auto-overlay][lifted]") {
    OverlayGuard g;
    Harness h;
    // Two things at once, both about the declaration being inert on its own.
    // It must not claim: a panel that names its menu before it is an overlay
    // would start routing clicks for a box the author has not opened yet. And
    // it must still be readable by the claim that eventually arrives, on the
    // post-mount `setAttribute` path rather than the pre-mount replay.
    h.eval(R"(
        var menu = document.createElement('div');
        menu.id = 'band-menu';
        menu.setAttribute('role', 'menu');
        document.body.appendChild(menu);

        var sub = document.createElement('div');
        sub.setAttribute('data-overlay-parent', 'band-menu');
        document.body.appendChild(sub);
    )");
    REQUIRE(h.root.overlay_depth() == 1);
    REQUIRE(h.root.interaction().active_overlay != nullptr);

    h.eval("document.body.children[1].setAttribute('role', 'menu');");
    REQUIRE(h.root.overlay_depth() == 2);
}

TEST_CASE("aria-owns recovers a lifted submenu that mounted before its menu",
          "[view][web-compat][auto-overlay][lifted]") {
    OverlayGuard g;
    Harness h;
    // `aria-owns` lands on the MENU and names the panel, so the claim it
    // changes belongs to a DIFFERENT element. Re-evaluating only the element
    // that received the attribute leaves the panel still claiming as a rival,
    // which is why mounting the menu re-evaluates everything it owns.
    //
    // Honest about what this recovers: the panel's dismiss callback does fire
    // once as the menu claims, before the re-evaluation puts the nest back. The
    // native stack ends correct, which is strictly better than ending wrong,
    // but it is not a substitute for mounting the menu first.
    h.eval(R"(
        var sub = document.createElement('div');
        sub.id = 'mod-panel';
        sub.setAttribute('role', 'menu');
        document.body.appendChild(sub);
    )");
    REQUIRE(h.root.overlay_depth() == 1);

    h.eval(R"(
        var menu = document.createElement('div');
        menu.setAttribute('role', 'menu');
        menu.setAttribute('aria-owns', 'mod-panel');
        document.body.appendChild(menu);
    )");
    REQUIRE(h.root.overlay_depth() == 2);
}

TEST_CASE("a menu mounted after its panel and owning nothing replaces it",
          "[view][web-compat][auto-overlay][lifted]") {
    OverlayGuard g;
    Harness h;
    // The control for the case above, same order and same markup minus the
    // `aria-owns`. Without it there is nothing to recover from, so the later
    // menu is just a rival and the panel is gone.
    h.eval(R"(
        var sub = document.createElement('div');
        sub.id = 'mod-panel';
        sub.setAttribute('role', 'menu');
        document.body.appendChild(sub);
    )");
    REQUIRE(h.root.overlay_depth() == 1);

    h.eval(R"(
        var menu = document.createElement('div');
        menu.setAttribute('role', 'menu');
        document.body.appendChild(menu);
    )");
    REQUIRE(h.root.overlay_depth() == 1);
}
