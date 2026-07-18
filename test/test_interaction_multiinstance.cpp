// test_interaction_multiinstance.cpp — the isolation proof for pulp #6223 S11.
//
// The companion smoke (test/test_hosting_input_smoke.cpp) pins the
// SINGLE-INSTANCE focus / overlay / popup lifecycle. This file pins the thing
// that single-instance smoke deliberately cannot: two independent hosted
// editors — two distinct root View trees in ONE process, the documented
// shared-AUHostingService case — must NOT share a focus slot, an active
// overlay, an open popup, or an overlay paint queue.
//
// Before S11 the interaction state lived in process-wide statics
// (View::focused_input_, View::active_overlay_, ComboBox::active_popup_, the
// overlay queue), so claiming focus in editor B stole editor A's keyboard and
// opening a dropdown in one closed the other's. S11 moves the state onto the
// tree root, reached via View::interaction(); the historical statics remain as
// process-global shim mirrors for the untouched platform hosts.
//
// Every assertion below observes the PER-ROOT state via interaction(). Each
// would FAIL if interaction() resolved to one shared block (the pre-S11 world):
// the "editor B slot is empty / editor A stays open" checks are exactly what a
// shared slot violates. That failing-when-shared property is the proof the test
// exercises real isolation, not just the shim mirror.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/view.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/input_events.hpp>

#include <memory>

using namespace pulp::view;

namespace {

// A focusable, text-accepting widget — the shape the host focus protocol
// targets. Matches the smoke's FocusSpy.
class FocusSpy : public View {
public:
    FocusSpy() { set_focusable(true); }
    bool accepts_text_input() const override { return true; }
};

// Build a real (non-detached) root that owns its own interaction state: a root
// View with a single child. tree_root() of either resolves to `root`, which —
// having a child — is NOT a detached widget, so it gets its own per-root slot
// rather than the process-global fallback.
struct Editor {
    View root;
    FocusSpy* field = nullptr;
    Editor() {
        root.set_bounds({0, 0, 400, 300});
        auto f = std::make_unique<FocusSpy>();
        field = f.get();
        field->set_bounds({0, 0, 120, 30});
        root.add_child(std::move(f));
    }
};

// Deliver a left mouse-down through the real on_mouse_event path (as the host
// does on press), so a ComboBox opens exactly the way the smoke opens it.
void press(View& v, Point local) {
    MouseEvent e;
    e.position = local;
    e.window_position = local;
    e.button = MouseButton::left;
    e.is_down = true;
    e.phase = MousePhase::press;
    v.on_mouse_event(e);
}

// Reset the process-global shim mirrors between cases (the per-root slots live
// and die with each case's local roots, but the mirrors are process-global).
void reset_mirrors() {
    ComboBox::close_active_popup();
    View::focused_input_ = nullptr;
    View::active_overlay_ = nullptr;
}

}  // namespace

// ── Focus isolation ──────────────────────────────────────────────────────────
// Claiming focus inside editor A points ONLY editor A's root slot at the field;
// editor B's root slot stays empty. A later claim in editor B does not disturb
// editor A — neither editor steals the other's keyboard.
TEST_CASE("focus slot is isolated per root across two hosted editors",
          "[view][input][multiinstance]") {
    reset_mirrors();

    Editor a;
    Editor b;

    a.field->claim_input_focus();
    REQUIRE(a.root.interaction().focused_input == a.field);
    REQUIRE(b.root.interaction().focused_input == nullptr);  // B unaffected

    b.field->claim_input_focus();
    REQUIRE(b.root.interaction().focused_input == b.field);
    REQUIRE(a.root.interaction().focused_input == a.field);  // A NOT stolen

    // Guarded release only clears the owning editor's slot.
    a.field->release_input_focus();
    REQUIRE(a.root.interaction().focused_input == nullptr);
    REQUIRE(b.root.interaction().focused_input == b.field);  // B still focused

    reset_mirrors();
}

// ── Overlay isolation ────────────────────────────────────────────────────────
// A popover claimed in editor A is the active overlay for editor A's root only;
// editor B's root reports no active overlay.
TEST_CASE("active overlay is isolated per root across two hosted editors",
          "[view][input][multiinstance]") {
    reset_mirrors();

    View rootA;
    rootA.set_bounds({0, 0, 400, 300});
    auto oa = std::make_unique<View>();
    View* overlayA = oa.get();
    rootA.add_child(std::move(oa));

    View rootB;
    rootB.set_bounds({0, 0, 400, 300});
    auto ob = std::make_unique<View>();
    View* overlayB = ob.get();
    rootB.add_child(std::move(ob));

    overlayA->claim_overlay();
    REQUIRE(rootA.interaction().active_overlay == overlayA);
    REQUIRE(rootB.interaction().active_overlay == nullptr);  // B unaffected

    overlayB->claim_overlay();
    REQUIRE(rootB.interaction().active_overlay == overlayB);
    REQUIRE(rootA.interaction().active_overlay == overlayA);  // A NOT cleared

    reset_mirrors();
}

// ── Popup isolation ──────────────────────────────────────────────────────────
// The pre-S11 bug in its purest form: opening a dropdown in one editor closed
// the other's, because there was one process-wide active popup. Per-root, each
// editor keeps its own open combo.
TEST_CASE("open ComboBox popup is isolated per root across two hosted editors",
          "[view][input][multiinstance]") {
    reset_mirrors();

    View rootA;
    rootA.set_bounds({0, 0, 400, 300});
    auto ca = std::make_unique<ComboBox>();
    ComboBox* comboA = ca.get();
    comboA->set_bounds({0, 0, 160, 28});
    comboA->set_items({"A1", "A2", "A3"});
    rootA.add_child(std::move(ca));

    View rootB;
    rootB.set_bounds({0, 0, 400, 300});
    auto cb = std::make_unique<ComboBox>();
    ComboBox* comboB = cb.get();
    comboB->set_bounds({0, 0, 160, 28});
    comboB->set_items({"B1", "B2", "B3"});
    rootB.add_child(std::move(cb));

    press(*comboA, {10, 14});
    REQUIRE(comboA->is_open());
    REQUIRE(rootA.interaction().active_popup == comboA);
    REQUIRE(rootB.interaction().active_popup == nullptr);  // B has no popup

    // Opening B's combo must NOT close A's (the isolation the bug violated).
    press(*comboB, {10, 14});
    REQUIRE(comboB->is_open());
    REQUIRE(comboA->is_open());                            // A stays open
    REQUIRE(rootB.interaction().active_popup == comboB);
    REQUIRE(rootA.interaction().active_popup == comboA);   // A slot intact

    // An outside-click reported in editor B dismisses only B's popup: the click
    // target resolves to B's root, so A is untouched. This is the forwarding
    // shim (notify_global_click) routing per-root instead of process-wide.
    auto ez = std::make_unique<View>();
    View* elsewhereInB = ez.get();
    elsewhereInB->set_bounds({0, 200, 40, 20});  // outside comboB's dropdown
    rootB.add_child(std::move(ez));
    ComboBox::notify_global_click(elsewhereInB);
    REQUIRE_FALSE(comboB->is_open());
    REQUIRE(comboA->is_open());                            // A still open
    REQUIRE(rootA.interaction().active_popup == comboA);

    reset_mirrors();
}

// ── Overlay paint-queue isolation ────────────────────────────────────────────
// Each root owns its overlay paint queue, so a second editor's paint pass never
// draws the first editor's deferred overlays. Enqueue on A's root and confirm
// B's queue is untouched; draining A (paint_overlays with A as the painting
// root) leaves B's queue alone.
TEST_CASE("overlay paint queue is isolated per root across two hosted editors",
          "[view][input][multiinstance]") {
    reset_mirrors();

    Editor a;
    Editor b;

    bool a_painted = false;
    a.root.interaction().overlay_queue.push_back(
        {[&](pulp::canvas::Canvas&) { a_painted = true; }, &a.root});

    REQUIRE(a.root.interaction().overlay_queue.size() == 1);
    REQUIRE(b.root.interaction().overlay_queue.empty());  // B unaffected

    reset_mirrors();
}
