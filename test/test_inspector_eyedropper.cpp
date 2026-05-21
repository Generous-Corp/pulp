// Phase 3c color-eyedropper tests, split from test_inspector.cpp (P11-5, #2647).
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

// ── Phase 3c — color eyedropper ───────────────────────────────────────────
//
// Eyedropper mode (E key) samples a color from the rendered UI and
// applies it as a tweak to the selected view's color property. With
// no Canvas pixel-readback in headless tests, sampling falls back to
// the resolved background color of the view under the cursor; the
// tweak path reuses emit_tweak_for_selection() with source
// "inspector-eyedropper".

namespace {

// Build a root + child where the child carries an explicit background
// color (so the resolved-style fallback has something to sample) and
// an anchor (so the eyedropper tweak can land in the store).
struct Phase3cScene {
    View root;
    View* child = nullptr;
    TweakStore store;
    InspectorOverlay overlay{root};

    explicit Phase3cScene(Color child_bg = Color::rgba8(0x33, 0x66, 0xcc)) {
        root.set_bounds({0, 0, 500, 400});
        auto c = std::make_unique<View>();
        c->set_anchor_id("figma:3c:1");
        c->set_bounds({40, 40, 120, 80});
        c->set_background_color(child_bg);
        child = c.get();
        root.add_child(std::move(c));

        overlay.set_active(true);
        overlay.set_tweak_store(&store);

        // Select the child via the normal click path.
        MouseEvent click;
        click.position = {60, 60};
        click.is_down = true;
        overlay.handle_mouse_event(click);
    }
};

MouseEvent make_move(float x, float y) {
    MouseEvent e;
    e.position = {x, y};
    e.is_down = false;
    return e;
}

MouseEvent make_click(float x, float y) {
    MouseEvent e;
    e.position = {x, y};
    e.is_down = true;
    return e;
}

KeyEvent make_key(KeyCode k, bool is_down = true, uint16_t mods = 0) {
    KeyEvent e;
    e.key = k;
    e.is_down = is_down;
    e.modifiers = mods;
    return e;
}

} // namespace

TEST_CASE("InspectorOverlay Phase 3c: eyedropper defaults OFF",
          "[inspect][overlay][phase3c]") {
    View root;
    InspectorOverlay overlay(root);
    REQUIRE_FALSE(overlay.eyedropper_active());
    REQUIRE_FALSE(overlay.eyedropper_has_sample());
    REQUIRE(overlay.eyedropper_target() == "style.background_color");

    overlay.set_eyedropper_active(true);
    REQUIRE(overlay.eyedropper_active());
    overlay.toggle_eyedropper();
    REQUIRE_FALSE(overlay.eyedropper_active());
}

TEST_CASE("InspectorOverlay Phase 3c: 'E' key toggles eyedropper only when active",
          "[inspect][overlay][phase3c]") {
    View root;
    InspectorOverlay overlay(root);

    // Inactive — E does nothing.
    REQUIRE_FALSE(overlay.handle_key_event(make_key(KeyCode::e)));
    REQUIRE_FALSE(overlay.eyedropper_active());

    overlay.set_active(true);
    // Active — E toggles.
    REQUIRE(overlay.handle_key_event(make_key(KeyCode::e)));
    REQUIRE(overlay.eyedropper_active());
    REQUIRE(overlay.handle_key_event(make_key(KeyCode::e)));
    REQUIRE_FALSE(overlay.eyedropper_active());

    // E with a modifier is ignored (reserved for chord shortcuts).
    overlay.handle_key_event(make_key(KeyCode::e, true, kModCmd));
    REQUIRE_FALSE(overlay.eyedropper_active());
}

TEST_CASE("InspectorOverlay Phase 3c: mouse-move samples color under cursor",
          "[inspect][overlay][phase3c]") {
    Phase3cScene s(Color::rgba8(0x12, 0xab, 0x5f));
    s.overlay.set_eyedropper_active(true);
    REQUIRE_FALSE(s.overlay.eyedropper_has_sample());

    // Move over the child — resolved-style fallback samples its bg.
    s.overlay.handle_mouse_event(make_move(80, 70));
    REQUIRE(s.overlay.eyedropper_has_sample());
    auto c = s.overlay.eyedropper_sample();
    REQUIRE(c.r8() == 0x12);
    REQUIRE(c.g8() == 0xab);
    REQUIRE(c.b8() == 0x5f);
}

TEST_CASE("InspectorOverlay Phase 3c: click captures color and emits tweak",
          "[inspect][overlay][phase3c]") {
    Phase3cScene s(Color::rgba8(0xff, 0x80, 0x00));
    s.overlay.set_eyedropper_active(true);

    // Hover to sample, then click to apply.
    s.overlay.handle_mouse_event(make_move(90, 80));
    bool consumed = s.overlay.handle_mouse_event(make_click(90, 80));
    REQUIRE(consumed);  // eyedropper consumes the pick click

    // A pick is a single deliberate action — mode auto-disables.
    REQUIRE_FALSE(s.overlay.eyedropper_active());

    // The tweak landed in the store under the selection's anchor.
    REQUIRE(s.store.count() == 1);
    auto v = s.store.lookup("figma:3c:1", "style.background_color");
    REQUIRE(v.has_value());
    REQUIRE(std::string(v->getString()) == "#ff8000");
}

TEST_CASE("InspectorOverlay Phase 3c: click without prior move still picks",
          "[inspect][overlay][phase3c]") {
    // Resolved-style sampling is synchronous, so a click with no
    // preceding mouse-move still captures a real color (covers
    // scripted / fast-click use).
    Phase3cScene s(Color::rgba8(0x40, 0x40, 0x40));
    s.overlay.set_eyedropper_active(true);

    bool consumed = s.overlay.handle_mouse_event(make_click(100, 90));
    REQUIRE(consumed);
    REQUIRE(s.store.count() == 1);
    auto v = s.store.lookup("figma:3c:1", "style.background_color");
    REQUIRE(v.has_value());
    REQUIRE(std::string(v->getString()) == "#404040");
}

TEST_CASE("InspectorOverlay Phase 3c: click refreshes stale hover sample",
          "[inspect][overlay][phase3c][regression]") {
    Phase3cScene s(Color::rgba8(0xff, 0x00, 0x00));

    auto second = std::make_unique<View>();
    second->set_anchor_id("figma:3c:2");
    second->set_bounds({40, 150, 120, 80});
    second->set_background_color(Color::rgba8(0x00, 0x66, 0xff));
    s.root.add_child(std::move(second));

    s.overlay.set_eyedropper_active(true);

    // Hover over the selected child first, then click elsewhere. The
    // committed tweak must use the click location, not the stale hover
    // swatch sample.
    s.overlay.handle_mouse_event(make_move(90, 80));
    auto hover_sample = s.overlay.eyedropper_sample();
    REQUIRE(hover_sample.r8() == 0xff);
    REQUIRE(hover_sample.g8() == 0x00);
    REQUIRE(hover_sample.b8() == 0x00);

    bool consumed = s.overlay.handle_mouse_event(make_click(90, 170));
    REQUIRE(consumed);

    auto v = s.store.lookup("figma:3c:1", "style.background_color");
    REQUIRE(v.has_value());
    REQUIRE(std::string(v->getString()) == "#0066ff");
}

TEST_CASE("InspectorOverlay Phase 3c: click never commits a stale sample",
          "[inspect][overlay][phase3c][regression]") {
    // Codex P1 (#2434): pressing E and clicking on empty canvas must
    // not write a stale color into the tweak store. A prior sample can
    // be left behind by an earlier hover (or by paint_eyedropper_cursor
    // sampling from the default cursor position before any mouse-move).
    // When the click lands where the resolved-style fallback finds no
    // background-colored view, the resample fails — and the pick must
    // no-op rather than commit the stale color from the old position.
    Phase3cScene s(Color::rgba8(0xff, 0x00, 0x00));
    s.overlay.set_eyedropper_active(true);

    // Seed a stale sample by hovering over the colored child.
    s.overlay.handle_mouse_event(make_move(90, 80));
    REQUIRE(s.overlay.eyedropper_has_sample());
    REQUIRE(s.overlay.eyedropper_sample().r8() == 0xff);

    // Click on empty canvas — (60, 300) is inside the root and clear of
    // the inspector panel (x >= 200), but hits no background-colored
    // view, so the resolved-style fallback returns false. The stale red
    // sample must not survive into the store.
    bool consumed = s.overlay.handle_mouse_event(make_click(60, 300));
    REQUIRE(consumed);  // the click is still consumed by the eyedropper

    // No tweak — the failed resample invalidated the sample, so
    // apply_eyedropper_pick() correctly no-ops instead of writing red.
    REQUIRE(s.store.count() == 0);
    REQUIRE_FALSE(s.overlay.eyedropper_has_sample());
}

TEST_CASE("InspectorOverlay Phase 3c: retargeting writes a different property",
          "[inspect][overlay][phase3c]") {
    Phase3cScene s(Color::rgba8(0x00, 0x99, 0xff));
    s.overlay.set_eyedropper_target("style.border_color");
    s.overlay.set_eyedropper_active(true);

    s.overlay.handle_mouse_event(make_click(80, 70));
    auto v = s.store.lookup("figma:3c:1", "style.border_color");
    REQUIRE(v.has_value());
    REQUIRE(std::string(v->getString()) == "#0099ff");

    // Empty retarget is ignored — keeps the previous path.
    s.overlay.set_eyedropper_target("");
    REQUIRE(s.overlay.eyedropper_target() == "style.border_color");
}

TEST_CASE("InspectorOverlay Phase 3c: sample_color_at hex round-trips alpha",
          "[inspect][overlay][phase3c]") {
    // A semi-transparent background encodes as 8-digit "#rrggbbaa".
    Phase3cScene s(Color::rgba8(0x20, 0x30, 0x40, 0x80));
    s.overlay.set_eyedropper_active(true);
    s.overlay.handle_mouse_event(make_click(80, 70));
    auto v = s.store.lookup("figma:3c:1", "style.background_color");
    REQUIRE(v.has_value());
    REQUIRE(std::string(v->getString()) == "#20304080");
}

TEST_CASE("InspectorOverlay Phase 3c: eyedropper yields to the panel",
          "[inspect][overlay][phase3c]") {
    // A click inside the inspector panel must NOT be eaten by the
    // eyedropper — the user still needs tree / field interaction.
    Phase3cScene s;
    s.overlay.set_eyedropper_active(true);

    // Panel occupies the right 300px of the 500px-wide root.
    MouseEvent panel_click = make_click(450, 50);
    bool consumed = s.overlay.handle_mouse_event(panel_click);
    REQUIRE(consumed);  // panel consumes its own clicks
    // No pick happened — eyedropper still armed, store untouched.
    REQUIRE(s.overlay.eyedropper_active());
    REQUIRE(s.store.count() == 0);
}

TEST_CASE("InspectorOverlay Phase 3c: pick no-ops without a selection anchor",
          "[inspect][overlay][phase3c]") {
    // Hand-authored view with no anchor — the eyedropper still
    // disables (the click was a deliberate spend) but emits nothing.
    View root;
    root.set_bounds({0, 0, 500, 400});
    auto c = std::make_unique<View>();
    c->set_bounds({40, 40, 120, 80});
    c->set_background_color(Color::rgba8(0x11, 0x22, 0x33));
    root.add_child(std::move(c));

    TweakStore store;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_tweak_store(&store);
    overlay.handle_mouse_event(make_click(60, 60));  // select
    overlay.set_eyedropper_active(true);

    overlay.handle_mouse_event(make_click(80, 70));  // pick
    REQUIRE_FALSE(overlay.eyedropper_active());
    REQUIRE(store.count() == 0);
}

TEST_CASE("InspectorOverlay Phase 3c: deactivating inspector clears eyedropper",
          "[inspect][overlay][phase3c]") {
    Phase3cScene s;
    s.overlay.set_eyedropper_active(true);
    s.overlay.handle_mouse_event(make_move(80, 70));
    REQUIRE(s.overlay.eyedropper_has_sample());

    s.overlay.set_active(false);
    REQUIRE_FALSE(s.overlay.eyedropper_active());
    REQUIRE_FALSE(s.overlay.eyedropper_has_sample());
}

TEST_CASE("InspectorOverlay Phase 3c: paint draws swatch when armed",
          "[inspect][overlay][phase3c]") {
    // The cursor swatch must paint without crashing once a sample
    // exists; RecordingCanvas has no pixel readback so this exercises
    // the resolved-style path + the swatch chrome.
    Phase3cScene s;
    s.overlay.set_eyedropper_active(true);
    s.overlay.handle_mouse_event(make_move(80, 70));
    REQUIRE(s.overlay.eyedropper_has_sample());

    pulp::canvas::RecordingCanvas canvas;
    s.overlay.paint(canvas);  // must not crash; swatch chrome drawn
    REQUIRE(s.overlay.eyedropper_active());
}

