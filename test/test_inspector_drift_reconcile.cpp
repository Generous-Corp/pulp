// Inspector — drift drawer (Phase 2) + reconciliation tab (Phase 5.2) tests.
//
// Split verbatim out of test/test_inspector.cpp (Phase-5 oversized-test-file
// refactor). The TEST_CASE blocks are byte-identical to their originals;
// only the file/binary they live in changed.
//
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md.

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/inspector.hpp>
#include <choc/containers/choc_Value.h>

#include <cstdint>
#include <string_view>

using namespace pulp::view;
using namespace pulp::inspect;

namespace {

// Verbatim copy of the KeyEvent factory from test_inspector.cpp; duplicated
// here so the drift/reconcile cluster is self-contained.
KeyEvent make_key(KeyCode k, bool is_down = true, uint16_t mods = 0) {
    KeyEvent e;
    e.key = k;
    e.is_down = is_down;
    e.modifiers = mods;
    return e;
}

}  // namespace

// ── Phase 2 — drift drawer ──────────────────────────────────────────────
//
// The drift drawer is a collapsible warning panel inside the inspector
// overlay that lists tweaks whose anchor_id no longer resolves to a live
// view. It is populated by refresh_drift(), which walks the live view
// tree's anchor set and diffs it against the attached TweakStore.
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md
// (Phase 2).
namespace {

// True if any fill_text command's text contains `needle`.
bool canvas_has_text(const pulp::canvas::RecordingCanvas& canvas,
                     std::string_view needle) {
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_text &&
            cmd.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("InspectorOverlay: refresh_drift finds tweaks with no live anchor",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 500, 400});

    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-still-here");
    root.add_child(std::move(child));

    TweakStore store;
    // One tweak whose anchor is live, one whose anchor is gone.
    store.apply_tweak("anchor-still-here", "layout.padding",
                      choc::value::createInt32(8));
    store.apply_tweak("anchor-removed-by-reimport", "paint.color",
                      choc::value::createString("#ff0000"));

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);
    overlay.refresh_drift();

    REQUIRE(overlay.drift_count() == 1);
    REQUIRE(overlay.drifted().front().anchor_id ==
            "anchor-removed-by-reimport");
    REQUIRE(overlay.drifted().front().reason ==
            TweakStore::DriftReason::anchor_not_found);
    // First drift detection auto-expands the drawer so it is not silent.
    REQUIRE(overlay.drift_drawer_open());
}

TEST_CASE("InspectorOverlay: no drift means no drawer and an empty list",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 500, 400});
    auto child = std::make_unique<View>();
    child->set_anchor_id("live-anchor");
    root.add_child(std::move(child));

    TweakStore store;
    store.apply_tweak("live-anchor", "layout.width",
                      choc::value::createInt32(120));

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);
    overlay.refresh_drift();

    REQUIRE(overlay.drift_count() == 0);
    REQUIRE(overlay.drifted().empty());

    // The drawer paints nothing on the happy path.
    overlay.set_active(true);
    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE_FALSE(canvas_has_text(canvas, "Drift"));
}

TEST_CASE("InspectorOverlay: refresh_drift with no TweakStore is a safe no-op",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    overlay.refresh_drift();  // no store wired
    REQUIRE(overlay.drift_count() == 0);
    REQUIRE(overlay.drifted().empty());
}

TEST_CASE("InspectorOverlay: drift drawer renders header and orphan rows",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 600, 500});
    auto child = std::make_unique<View>();
    child->set_anchor_id("kept");
    root.add_child(std::move(child));

    TweakStore store;
    store.apply_tweak("kept", "layout.width",
                      choc::value::createInt32(64));
    store.apply_tweak("gone-anchor", "layout.margin",
                      choc::value::createInt32(10));

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);
    overlay.set_active(true);

    // First paint auto-refreshes drift and auto-expands the drawer.
    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);

    REQUIRE(overlay.drift_count() == 1);
    REQUIRE(overlay.drift_drawer_open());
    // Header text + the orphaned anchor + its reason tag are painted.
    REQUIRE(canvas_has_text(canvas, "Drift"));
    REQUIRE(canvas_has_text(canvas, "gone-anchor"));
    REQUIRE(canvas_has_text(canvas, "anchor-not-found"));
    REQUIRE(canvas_has_text(canvas, "layout.margin"));
}

TEST_CASE("InspectorOverlay: clicking the drift header toggles the drawer",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 600, 500});
    auto child = std::make_unique<View>();
    child->set_anchor_id("present");
    root.add_child(std::move(child));

    TweakStore store;
    store.apply_tweak("present", "layout.width",
                      choc::value::createInt32(80));
    store.apply_tweak("missing", "paint.opacity",
                      choc::value::createFloat32(0.4f));

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);
    overlay.set_active(true);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);  // auto-expands; populates the header hit-rect
    REQUIRE(overlay.drift_drawer_open());

    // The drawer header sits just below the tree section (root_h * 0.5
    // = 250) on the panel side (panel_x = 600 - 300 = 300). A click in
    // that band collapses the drawer.
    MouseEvent click;
    click.position = {360, 258};
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE_FALSE(overlay.drift_drawer_open());

    // Re-paint so the (now-collapsed) header hit-rect is refreshed,
    // then click again to re-expand.
    canvas.clear();
    overlay.paint(canvas);
    MouseEvent click2;
    click2.position = {360, 258};
    click2.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click2));
    REQUIRE(overlay.drift_drawer_open());
}

TEST_CASE("InspectorOverlay: collapsed drawer hides rows but keeps the header",
          "[inspect][overlay][drift][phase2]") {
    View root;
    root.set_bounds({0, 0, 600, 500});

    TweakStore store;
    store.apply_tweak("orphan-a", "layout.width",
                      choc::value::createInt32(50));

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);
    overlay.set_active(true);
    overlay.refresh_drift();
    overlay.set_drift_drawer_open(false);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    // Header still shows the drift count; the orphan row does not.
    REQUIRE(canvas_has_text(canvas, "Drift"));
    REQUIRE_FALSE(canvas_has_text(canvas, "layout.width"));
}

// ── Phase 5.2 — reconciliation tab ────────────────────────────────────────
//
// The reconciliation tab (R-key) classifies every stored tweak into
// locked-to-source / drifted / unresolvable and renders a read-only
// report. It builds on the Phase 4a lock-to-source state (the
// TweakStore lock set) and the live view tree's anchor set. These
// tests drive it headless: build a scene, seed tweaks, toggle the tab
// with R, paint into a RecordingCanvas, and assert the classification.

namespace {

// A scene with two live anchored views plus one tweak whose anchor is
// NOT in the tree (orphaned). Locking one of the live anchors lets a
// test exercise all three reconciliation states at once.
struct ReconcileScene {
    View root;
    TweakStore store;
    InspectorOverlay overlay{root};

    ReconcileScene() {
        root.set_bounds({0, 0, 600, 600});

        auto a = std::make_unique<View>();
        a->set_anchor_id("figma:5:a");
        a->set_bounds({10, 10, 80, 40});
        root.add_child(std::move(a));

        auto b = std::make_unique<View>();
        b->set_anchor_id("figma:5:b");
        b->set_bounds({10, 60, 80, 40});
        root.add_child(std::move(b));

        overlay.set_active(true);
        overlay.set_tweak_store(&store);

        // figma:5:a — locked → "locked-to-source" once locked below.
        store.apply_tweak("figma:5:a", "layout.padding",
                          choc::value::createInt32(12), "drag");
        // figma:5:b — resolves but unlocked → "drifted".
        store.apply_tweak("figma:5:b", "layout.gap",
                          choc::value::createInt32(4), "drag");
        // figma:5:gone — no live view carries it → "unresolvable".
        store.apply_tweak("figma:5:gone", "paint.backgroundColor",
                          choc::value::createString("#abcdef"), "picker");
    }
};

}  // namespace

TEST_CASE("InspectorOverlay Phase 5.2: R toggles the reconciliation tab",
          "[inspect][overlay][reconcile][phase5.2]") {
    View root;
    root.set_bounds({0, 0, 600, 600});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    REQUIRE_FALSE(overlay.reconcile_tab_visible());

    KeyEvent r;
    r.key = KeyCode::r;
    r.is_down = true;
    REQUIRE(overlay.handle_key_event(r));
    REQUIRE(overlay.reconcile_tab_visible());

    REQUIRE(overlay.handle_key_event(r));
    REQUIRE_FALSE(overlay.reconcile_tab_visible());
}

TEST_CASE("InspectorOverlay Phase 5.2: R does nothing when inspector inactive",
          "[inspect][overlay][reconcile][phase5.2]") {
    View root;
    InspectorOverlay overlay(root);
    // Inspector not active — R must not flip the tab.
    REQUIRE_FALSE(overlay.handle_key_event(make_key(KeyCode::r)));
    REQUIRE_FALSE(overlay.reconcile_tab_visible());

    // A modifier-laden R is reserved for chord shortcuts — ignored.
    overlay.set_active(true);
    overlay.handle_key_event(make_key(KeyCode::r, true, kModCmd));
    REQUIRE_FALSE(overlay.reconcile_tab_visible());
}

TEST_CASE("InspectorOverlay Phase 5.2: reconcile_report classifies all states",
          "[inspect][overlay][reconcile][phase5.2]") {
    ReconcileScene scene;
    // Lock figma:5:a — promotes its tweak to "locked-to-source".
    scene.store.set_locked("figma:5:a", true);

    auto report = scene.overlay.reconcile_report();
    REQUIRE(report.total() == 3);
    REQUIRE(report.locked_count == 1);
    REQUIRE(report.drifted_count == 1);
    REQUIRE(report.unresolvable_count == 1);

    // Verify the per-row classification by anchor.
    auto status_for = [&](std::string_view anchor)
        -> InspectorOverlay::ReconcileStatus {
        for (const auto& row : report.rows)
            if (row.anchor_id == anchor) return row.status;
        FAIL("anchor not found in report");
        return InspectorOverlay::ReconcileStatus::unresolvable;
    };
    REQUIRE(status_for("figma:5:a") ==
            InspectorOverlay::ReconcileStatus::locked_to_source);
    REQUIRE(status_for("figma:5:b") ==
            InspectorOverlay::ReconcileStatus::drifted);
    REQUIRE(status_for("figma:5:gone") ==
            InspectorOverlay::ReconcileStatus::unresolvable);
}

TEST_CASE("InspectorOverlay Phase 5.2: reconcile_report preserves order within an anchor",
          "[inspect][overlay][reconcile][phase5.2]") {
    View root;
    root.set_bounds({0, 0, 600, 600});
    auto node = std::make_unique<View>();
    node->set_anchor_id("figma:5:a");
    root.add_child(std::move(node));

    TweakStore store;
    store.apply_tweak("figma:5:a", "layout.padding",
                      choc::value::createInt32(12), "drag");
    store.apply_tweak("figma:5:a", "layout.gap",
                      choc::value::createInt32(4), "drag");
    store.apply_tweak("figma:5:a", "paint.opacity",
                      choc::value::createFloat32(0.5f), "drag");

    InspectorOverlay overlay(root);
    overlay.set_tweak_store(&store);

    auto report = overlay.reconcile_report();
    REQUIRE(report.rows.size() == 3);
    REQUIRE(report.rows[0].property_path == "layout.padding");
    REQUIRE(report.rows[1].property_path == "layout.gap");
    REQUIRE(report.rows[2].property_path == "paint.opacity");
}

TEST_CASE("InspectorOverlay Phase 5.2: unlocked live tweak reads as drifted",
          "[inspect][overlay][reconcile][phase5.2]") {
    // Without an explicit lock, a tweak whose anchor resolves to a
    // live view is "drifted" — it lives only in the runtime layer and
    // would not survive a re-import.
    ReconcileScene scene;  // no set_locked() call
    auto report = scene.overlay.reconcile_report();
    REQUIRE(report.locked_count == 0);
    // figma:5:a + figma:5:b both resolve but are unlocked → drifted.
    REQUIRE(report.drifted_count == 2);
    REQUIRE(report.unresolvable_count == 1);
}

TEST_CASE("InspectorOverlay Phase 5.2: tab lays out a row per tweak",
          "[inspect][overlay][reconcile][phase5.2]") {
    ReconcileScene scene;
    scene.store.set_locked("figma:5:a", true);
    scene.overlay.toggle_reconcile_tab();
    REQUIRE(scene.overlay.reconcile_tab_visible());

    pulp::canvas::RecordingCanvas canvas;
    scene.overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    // Three tweaks → three classified rows laid out.
    REQUIRE(scene.overlay.reconcile_row_count() == 3);
    // The tab heading + status tags render as text.
    REQUIRE(canvas_has_text(canvas, "Reconcile"));
    REQUIRE(canvas_has_text(canvas, "lock"));
    REQUIRE(canvas_has_text(canvas, "drift"));
    REQUIRE(canvas_has_text(canvas, "orphan"));

    // Hiding the tab clears the laid-out rows.
    scene.overlay.toggle_reconcile_tab();
    pulp::canvas::RecordingCanvas hidden;
    scene.overlay.paint(hidden);
    REQUIRE(scene.overlay.reconcile_row_count() == 0);
}

TEST_CASE("InspectorOverlay Phase 5.2: tab renders cleanly with no tweaks",
          "[inspect][overlay][reconcile][phase5.2]") {
    View root;
    root.set_bounds({0, 0, 600, 600});
    TweakStore store;  // empty
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.toggle_reconcile_tab();

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);  // must not crash
    REQUIRE(canvas.command_count() > 0);
    REQUIRE(overlay.reconcile_row_count() == 0);
    REQUIRE(canvas_has_text(canvas, "No tweaks to reconcile"));

    auto report = overlay.reconcile_report();
    REQUIRE(report.total() == 0);
}

TEST_CASE("InspectorOverlay Phase 5.2: reconcile_report empty without a store",
          "[inspect][overlay][reconcile][phase5.2]") {
    // The inspector can run with no TweakStore wired (hand-authored
    // UIs). The reconciliation tab must degrade gracefully — empty
    // report, no crash, an explanatory line in the panel.
    View root;
    root.set_bounds({0, 0, 600, 600});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.toggle_reconcile_tab();
    // intentionally no set_tweak_store()

    auto report = overlay.reconcile_report();
    REQUIRE(report.total() == 0);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);
    REQUIRE(overlay.reconcile_row_count() == 0);
    REQUIRE(canvas_has_text(canvas, "No tweak store attached"));
}

TEST_CASE("InspectorOverlay Phase 5.2: orphaned tweak never crashes the tab",
          "[inspect][overlay][reconcile][phase5.2]") {
    // A tweak whose anchor has no live view (e.g. after a destructive
    // re-import) must classify as unresolvable — not crash, not guess.
    View root;
    root.set_bounds({0, 0, 600, 600});
    TweakStore store;
    store.apply_tweak("ghost-anchor", "layout.width",
                      choc::value::createInt32(99), "drag");

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.toggle_reconcile_tab();

    auto report = overlay.reconcile_report();
    REQUIRE(report.total() == 1);
    REQUIRE(report.unresolvable_count == 1);
    REQUIRE(report.rows.front().status ==
            InspectorOverlay::ReconcileStatus::unresolvable);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);  // must not crash
    REQUIRE(overlay.reconcile_row_count() == 1);
    REQUIRE(canvas_has_text(canvas, "orphan"));
}

TEST_CASE("InspectorOverlay Phase 5.2: reconcile_status_str maps every state",
          "[inspect][overlay][reconcile][phase5.2]") {
    using RS = InspectorOverlay::ReconcileStatus;
    REQUIRE(std::string(InspectorOverlay::reconcile_status_str(
                RS::locked_to_source)) == "locked-to-source");
    REQUIRE(std::string(InspectorOverlay::reconcile_status_str(
                RS::drifted)) == "drifted");
    REQUIRE(std::string(InspectorOverlay::reconcile_status_str(
                RS::unresolvable)) == "unresolvable");
}

TEST_CASE("InspectorOverlay Phase 5.2: dismissing the inspector clears tab rows",
          "[inspect][overlay][reconcile][phase5.2]") {
    ReconcileScene scene;
    scene.overlay.toggle_reconcile_tab();

    pulp::canvas::RecordingCanvas canvas;
    scene.overlay.paint(canvas);
    REQUIRE(scene.overlay.reconcile_row_count() == 3);

    // Closing the inspector drops the laid-out rows; the R-toggle
    // state itself survives so re-opening restores the tab.
    scene.overlay.set_active(false);
    REQUIRE(scene.overlay.reconcile_row_count() == 0);
    REQUIRE(scene.overlay.reconcile_tab_visible());
}

TEST_CASE("InspectorOverlay Phase 5.2: locking a tweak moves it out of drift",
          "[inspect][overlay][reconcile][phase5.2]") {
    // Locking an anchor is the explicit "promote to source" action.
    // The reconciliation tab must reflect that transition: a drifted
    // tweak becomes locked-to-source the moment its anchor is locked.
    ReconcileScene scene;
    auto before = scene.overlay.reconcile_report();
    REQUIRE(before.locked_count == 0);
    REQUIRE(before.drifted_count == 2);

    scene.store.set_locked("figma:5:b", true);
    auto after = scene.overlay.reconcile_report();
    REQUIRE(after.locked_count == 1);
    REQUIRE(after.drifted_count == 1);
    REQUIRE(after.unresolvable_count == 1);  // figma:5:gone unchanged
}
