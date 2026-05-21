// Phase 3b live-editable field-edit tests, split from test_inspector.cpp (P11-5, #2647).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/inspect/inspector_window.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/inspect/console_capture.hpp>
#include <pulp/inspect/audio_inspector.hpp>
#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/render/atlas_inventory.hpp>
#include <pulp/render/render_pass.hpp>
#include <pulp/render/dirty_tracker.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/live_constant_editor.hpp>
#include <pulp/state/store.hpp>
#include <choc/containers/choc_Value.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace pulp::view;
using namespace pulp::inspect;

// ── Phase 3b — live-editable box-model fields ─────────────────────────────
//
// Click a numeric value in the property panel → enter edit mode →
// type / arrow-nudge / Enter to commit (tweak emitted) / Esc to
// cancel. The tweak path reuses Phase 0b PR-C-1's
// emit_tweak_for_selection() so persistence (Phase 1 disk write)
// gets keyboard edits for free.
//
// Test surface uses begin_field_edit() / handle_key_event() directly
// to avoid having to know the panel's exact pixel layout (it's
// recomputed each paint pass based on root bounds + panel_width_).
// The click-into-edit path is exercised by an integration test at
// the bottom that paints first to populate editable_fields_, then
// clicks at a known panel y-offset.

namespace {

// Build a small root + child with an anchor for tweak-emission tests.
struct Phase3bScene {
    View root;
    View* child = nullptr;
    TweakStore store;
    InspectorOverlay overlay{root};

    Phase3bScene() {
        root.set_bounds({0, 0, 500, 400});
        auto c = std::make_unique<View>();
        c->set_anchor_id("figma:3b:1");
        c->set_bounds({10, 10, 80, 40});
        c->flex().padding = 8;
        c->flex().margin = 4;
        c->flex().preferred_width = 80;
        c->flex().preferred_height = 40;
        child = c.get();
        root.add_child(std::move(c));

        overlay.set_active(true);
        overlay.set_tweak_store(&store);

        // Select the child via the normal click path.
        MouseEvent click;
        click.position = {20, 20};
        click.is_down = true;
        overlay.handle_mouse_event(click);
    }
};

KeyEvent make_key(KeyCode k, bool is_down = true, uint16_t mods = 0) {
    KeyEvent e;
    e.key = k;
    e.is_down = is_down;
    e.modifiers = mods;
    return e;
}

} // namespace

TEST_CASE("InspectorOverlay Phase 3b: begin_field_edit sets editing state",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    REQUIRE(s.overlay.selected_view() == s.child);
    REQUIRE_FALSE(s.overlay.is_editing());

    bool ok = s.overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE(ok);
    REQUIRE(s.overlay.is_editing());
    REQUIRE(s.overlay.editing_field() == "layout.padding");
    REQUIRE(s.overlay.edit_buffer() == "8");
    REQUIRE(s.overlay.edit_caret_pos() == 1);  // caret at end of "8"
}

TEST_CASE("InspectorOverlay Phase 3b: begin_field_edit refuses without selection",
          "[inspect][overlay][phase3b]") {
    View root;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    // No selection set.
    bool ok = overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(overlay.is_editing());
}

TEST_CASE("InspectorOverlay Phase 3b: typing digits extends edit buffer",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    REQUIRE(s.overlay.begin_field_edit("layout.padding", 8.0f));
    REQUIRE(s.overlay.edit_buffer() == "8");

    // Type "5" → buffer becomes "85"
    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::num5)));
    REQUIRE(s.overlay.edit_buffer() == "85");
    REQUIRE(s.overlay.edit_caret_pos() == 2);

    // Type "0" → "850"
    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::num0)));
    REQUIRE(s.overlay.edit_buffer() == "850");

    // Real-time preview: View padding should already reflect 850.
    REQUIRE(s.child->flex().padding == 850.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: backspace trims buffer",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(make_key(KeyCode::num5));
    REQUIRE(s.overlay.edit_buffer() == "85");

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::backspace)));
    REQUIRE(s.overlay.edit_buffer() == "8");
    REQUIRE(s.overlay.edit_caret_pos() == 1);
}

TEST_CASE("InspectorOverlay Phase 3b: Enter commits tweak to TweakStore",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    // Type "5" → buffer is "85"
    s.overlay.handle_key_event(make_key(KeyCode::num5));

    REQUIRE(s.store.count() == 0);
    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::enter)));

    // Edit mode exited, tweak persisted, View reflects committed value.
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.store.count() == 1);
    auto v = s.store.lookup("figma:3b:1", "layout.padding");
    REQUIRE(v.has_value());
    // Value stored as float32 — round-trip equality at integer values
    // is safe.
    REQUIRE(v->getFloat32() == 85.0f);
    REQUIRE(s.child->flex().padding == 85.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: Esc cancels and reverts",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    // Bump to "85" — real-time preview mutates the View.
    s.overlay.handle_key_event(make_key(KeyCode::num5));
    REQUIRE(s.child->flex().padding == 85.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::escape)));
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.store.count() == 0);                 // no tweak emitted
    REQUIRE(s.child->flex().padding == 8.0f);      // reverted
}

TEST_CASE("InspectorOverlay Phase 3b: Up arrow increments by 1",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::up)));
    REQUIRE(s.overlay.edit_buffer() == "9");
    REQUIRE(s.child->flex().padding == 9.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::up)));
    REQUIRE(s.overlay.edit_buffer() == "10");
    REQUIRE(s.child->flex().padding == 10.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: Down arrow decrements by 1",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::down)));
    REQUIRE(s.overlay.edit_buffer() == "7");
    REQUIRE(s.child->flex().padding == 7.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: Shift+Up nudges by 10",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::up, true, kModShift)));
    REQUIRE(s.overlay.edit_buffer() == "18");
    REQUIRE(s.child->flex().padding == 18.0f);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::down, true, kModShift)));
    REQUIRE(s.overlay.edit_buffer() == "8");
}

TEST_CASE("InspectorOverlay Phase 3b: Cmd+Up nudges by 100 (Figma semantics)",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);

#ifdef __APPLE__
    auto main_mod = kModCmd;
#else
    auto main_mod = kModCtrl;
#endif
    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::up, true, main_mod)));
    REQUIRE(s.overlay.edit_buffer() == "108");
    REQUIRE(s.child->flex().padding == 108.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: Tab commits and moves to next field",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    // Paint once so editable_fields_ is populated with the panel's
    // tab order.
    pulp::canvas::RecordingCanvas canvas;
    s.overlay.paint(canvas);

    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(make_key(KeyCode::num5));  // "85"

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::tab)));
    // The previous field's edit was committed (tweak in store).
    REQUIRE(s.store.count() == 1);
    REQUIRE(s.store.lookup("figma:3b:1", "layout.padding").has_value());
    // We're now editing a different field (whatever follows padding
    // in the panel's draw order).
    REQUIRE(s.overlay.is_editing());
    REQUIRE(s.overlay.editing_field() != "layout.padding");
}

TEST_CASE("InspectorOverlay Phase 3b: clicking a numeric value enters edit mode",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    // Paint to populate editable_fields_ (the panel-side hit rects).
    pulp::canvas::RecordingCanvas canvas;
    s.overlay.paint(canvas);

    // The panel sits at the right edge of root (500). panel_width_
    // defaults to 300, so panel starts at x = 200. paint_props_section
    // is called with x = panel_x + 8 = 208, and editable values live
    // at x + 80 = 288 → click at x = 290 lands on the value column.
    // The Y coordinate just needs to be inside the props area; the
    // tree section is the top half (root_h * 0.5 = 200), so click at
    // y = 210 lands on the first heading. Use the first editable
    // field's reported rect to pick a guaranteed-correct y.
    //
    // Use overlay's public state via paint + then a click that lands
    // within a row. We don't have direct access to editable_fields_
    // from tests, so click at a coordinate that the first row covers
    // given the default panel layout. Tree height is root_h/2 = 200,
    // so props_y = 200; first prop row starts ~ y = 204 + heading
    // height. We pick a y of 270 which falls into the layout section
    // ("padding" row given the order width/height/padding/margin).
    //
    // To make this robust regardless of exact row arithmetic, scan a
    // band of y values clicking at the value column until edit mode
    // is entered. Bounded by the visible props area.
    bool entered = false;
    for (float y = 210.0f; y < 380.0f && !entered; y += 5.0f) {
        MouseEvent click;
        click.position = {290.0f, y};
        click.is_down = true;
        s.overlay.handle_mouse_event(click);
        entered = s.overlay.is_editing();
        if (entered) break;
        // Repaint after each scan to refresh hit rects.
        canvas.clear();
        s.overlay.paint(canvas);
    }
    REQUIRE(entered);
}

TEST_CASE("InspectorOverlay Phase 3b: editing layout.width writes to preferred_width",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.width", s.child->flex().preferred_width);
    REQUIRE(s.overlay.edit_buffer() == "80");

    // Type "5" → buffer becomes "805"
    s.overlay.handle_key_event(make_key(KeyCode::num5));
    REQUIRE(s.child->flex().preferred_width == 805.0f);

    s.overlay.handle_key_event(make_key(KeyCode::enter));
    auto v = s.store.lookup("figma:3b:1", "layout.width");
    REQUIRE(v.has_value());
    REQUIRE(v->getFloat32() == 805.0f);
}

TEST_CASE("InspectorOverlay Phase 3b: editing style.opacity round-trips",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.child->set_opacity(0.5f);
    s.overlay.begin_field_edit("style.opacity", 0.5f);

    // Buffer for 0.5 should be "0.50"
    REQUIRE(s.overlay.edit_buffer() == "0.50");

    // Nudge up: 0.50 + 1 = 1.50, then clamped to 1.0 by set_opacity.
    s.overlay.handle_key_event(make_key(KeyCode::up));
    // Underlying View is clamped.
    REQUIRE(s.child->opacity() == 1.0f);
    s.overlay.handle_key_event(make_key(KeyCode::enter));

    auto v = s.store.lookup("figma:3b:1", "style.opacity");
    REQUIRE(v.has_value());
}

TEST_CASE("InspectorOverlay Phase 3b: cancel_field_edit on inactive is no-op",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    REQUIRE_FALSE(s.overlay.is_editing());
    s.overlay.cancel_field_edit();  // must not crash
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.store.count() == 0);
}

TEST_CASE("InspectorOverlay Phase 3b: commit_field_edit with empty buffer "
          "exits edit without emitting tweak",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    // Backspace twice to empty the buffer ("8" → "")
    s.overlay.handle_key_event(make_key(KeyCode::backspace));
    REQUIRE(s.overlay.edit_buffer().empty());

    REQUIRE_FALSE(s.overlay.commit_field_edit());
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.store.count() == 0);
}

TEST_CASE("InspectorOverlay Phase 3b: caret moves with Left/Right/Home/End",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 80.0f);
    REQUIRE(s.overlay.edit_buffer() == "80");
    REQUIRE(s.overlay.edit_caret_pos() == 2);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::left)));
    REQUIRE(s.overlay.edit_caret_pos() == 1);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::home)));
    REQUIRE(s.overlay.edit_caret_pos() == 0);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::end_)));
    REQUIRE(s.overlay.edit_caret_pos() == 2);

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::right)));
    REQUIRE(s.overlay.edit_caret_pos() == 2);  // already at end
}

TEST_CASE("InspectorOverlay Phase 3b: deactivating inspector cancels active edit",
          "[inspect][overlay][phase3b]") {
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(make_key(KeyCode::num5));
    REQUIRE(s.child->flex().padding == 85.0f);
    REQUIRE(s.overlay.is_editing());

    s.overlay.set_active(false);
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.child->flex().padding == 8.0f);   // reverted via cancel
    REQUIRE(s.store.count() == 0);              // no tweak emitted
}

TEST_CASE("InspectorOverlay Phase 3b: Esc while editing does NOT exit inspector",
          "[inspect][overlay][phase3b]") {
    // Spec: Esc inside an edit cancels the edit; Esc with no edit
    // exits the inspector. The two semantics share a key, the order
    // matters.
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    REQUIRE(s.overlay.is_active());

    s.overlay.handle_key_event(make_key(KeyCode::escape));
    REQUIRE_FALSE(s.overlay.is_editing());
    REQUIRE(s.overlay.is_active());  // still active

    // Second Esc with no edit → exits the inspector.
    s.overlay.handle_key_event(make_key(KeyCode::escape));
    REQUIRE_FALSE(s.overlay.is_active());
}

TEST_CASE("InspectorOverlay Phase 3b: Tab without paint does nothing extra",
          "[inspect][overlay][phase3b]") {
    // Tab consults the editable_fields_ list populated by the last
    // paint. If we Tab without ever painting, commit happens but no
    // next-field move occurs (gracefully).
    Phase3bScene s;
    s.overlay.begin_field_edit("layout.padding", 8.0f);
    s.overlay.handle_key_event(make_key(KeyCode::num5));

    REQUIRE(s.overlay.handle_key_event(make_key(KeyCode::tab)));
    // First-field commit happened.
    REQUIRE(s.store.count() == 1);
    // Without a prior paint, editable_fields_ is empty, so Tab can't
    // find a next field — we're no longer editing.
    REQUIRE_FALSE(s.overlay.is_editing());
}

