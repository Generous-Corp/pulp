// Inspector texture-atlas viewer tests.

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/render/atlas_inventory.hpp>
#include <pulp/view/inspector.hpp>

#include <string>
#include <string_view>

using namespace pulp::view;
using namespace pulp::inspect;

namespace {

// Intentionally local: keeps atlas-viewer canvas helpers isolated to this test binary.
int count_text_containing(const pulp::canvas::RecordingCanvas& canvas,
                          std::string_view needle) {
    int n = 0;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_text &&
            cmd.text.find(needle) != std::string::npos)
            ++n;
    }
    return n;
}

}  // namespace

namespace {

using pulp::render::AtlasInfo;
using pulp::render::AtlasInventory;
using pulp::render::AtlasKind;
using pulp::render::GlyphAtlas;
using pulp::render::ImageAtlas;

// Build a small inventory snapshotting two real, partially-packed atlases
// so the viewer has honest dimensions + occupancy to render.
AtlasInventory make_atlas_inventory() {
    static ImageAtlas images(256);
    static GlyphAtlas glyphs(512);
    pulp::render::AtlasPacker::Region r{};
    images.allocate(1, 128, 256, r);   // 50% of the image atlas.
    glyphs.allocate(2, 512, 256, r);   // 50% of the glyph atlas.

    AtlasInventory inv;
    inv.add_atlas(images, AtlasKind::image, "images");
    inv.add_atlas(glyphs, AtlasKind::glyph);
    return inv;
}

} // namespace

TEST_CASE("InspectorOverlay atlas viewer: A key toggles the atlas viewer",
          "[inspect][overlay][atlas][atlas-viewer]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    REQUIRE_FALSE(overlay.atlas_viewer_visible());

    KeyEvent a;
    a.key = KeyCode::a;
    a.is_down = true;
    a.modifiers = 0;
    REQUIRE(overlay.handle_key_event(a));
    REQUIRE(overlay.atlas_viewer_visible());

    REQUIRE(overlay.handle_key_event(a));
    REQUIRE_FALSE(overlay.atlas_viewer_visible());
}

TEST_CASE("InspectorOverlay atlas viewer: A key ignored when inspector inactive",
          "[inspect][overlay][atlas][atlas-viewer]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    // Not active — the A hotkey must not fire so it can't collide with
    // a plain text field in the host UI.

    KeyEvent a;
    a.key = KeyCode::a;
    a.is_down = true;
    a.modifiers = 0;
    REQUIRE_FALSE(overlay.handle_key_event(a));
    REQUIRE_FALSE(overlay.atlas_viewer_visible());
}

TEST_CASE("InspectorOverlay atlas viewer: viewer renders a row per atlas",
          "[inspect][overlay][atlas][atlas-viewer]") {
    View root;
    root.set_id("root");
    // Tall enough that the panel's lower section fits both atlas rows.
    root.set_bounds({0, 0, 500, 720});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    AtlasInventory inv = make_atlas_inventory();
    overlay.set_atlas_inventory(&inv);
    overlay.set_atlas_viewer_visible(true);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    // The viewer heading and both atlas labels must appear.
    REQUIRE(count_text_containing(canvas, "Texture Atlases") >= 1);
    REQUIRE(count_text_containing(canvas, "images") >= 1);
    REQUIRE(count_text_containing(canvas, "glyph") >= 1);
    // Each atlas printed an occupancy percentage — both atlases are at
    // 50% so a "50%" readout is present.
    REQUIRE(count_text_containing(canvas, "50%") >= 1);
    // Two atlases were registered → two laid-out rows.
    REQUIRE(overlay.atlas_row_count() == 2);
}

TEST_CASE("InspectorOverlay atlas viewer: no inventory shows the not-wired state",
          "[inspect][overlay][atlas][atlas-viewer]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_atlas_viewer_visible(true);
    // No AtlasInventory attached — set_atlas_inventory() has no production
    // caller yet, so the tab is ALWAYS in this state. It must say so honestly
    // ("not wired") rather than imply a GPU fault ("GPU atlas unavailable").

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);  // must not crash.
    REQUIRE(count_text_containing(canvas, "not wired") >= 1);
    REQUIRE(count_text_containing(canvas, "GPU atlas unavailable") == 0);
    REQUIRE(overlay.atlas_row_count() == 0);
}

TEST_CASE("InspectorOverlay atlas viewer: empty inventory shows the unavailable state",
          "[inspect][overlay][atlas][atlas-viewer]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_atlas_viewer_visible(true);

    AtlasInventory empty;  // attached but holds no atlases.
    overlay.set_atlas_inventory(&empty);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "GPU atlas unavailable") >= 1);
    REQUIRE(overlay.atlas_row_count() == 0);
}

TEST_CASE("InspectorOverlay atlas viewer: dismissing the inspector clears atlas rows",
          "[inspect][overlay][atlas][atlas-viewer]") {
    View root;
    root.set_bounds({0, 0, 500, 720});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    AtlasInventory inv = make_atlas_inventory();
    overlay.set_atlas_inventory(&inv);
    overlay.set_atlas_viewer_visible(true);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(overlay.atlas_row_count() == 2);

    // Closing the inspector resets the laid-out row count but leaves the
    // A-key toggle intact so re-opening restores the same tab.
    overlay.set_active(false);
    REQUIRE(overlay.atlas_row_count() == 0);
    REQUIRE(overlay.atlas_viewer_visible());
}

TEST_CASE("InspectorOverlay atlas viewer: pass viewer takes precedence over atlas tab",
          "[inspect][overlay][atlas][atlas-viewer]") {
    View root;
    root.set_bounds({0, 0, 500, 720});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    AtlasInventory inv = make_atlas_inventory();
    overlay.set_atlas_inventory(&inv);
    // Both tabs toggled on — the pass viewer is the older surface and
    // wins; the atlas tab must not paint, and its row count resets.
    overlay.set_atlas_viewer_visible(true);
    overlay.set_pass_viewer_enabled(true);

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "Render Passes") >= 1);
    REQUIRE(count_text_containing(canvas, "Texture Atlases") == 0);
    REQUIRE(overlay.atlas_row_count() == 0);
}
