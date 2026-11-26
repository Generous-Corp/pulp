#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/atlas_inventory.hpp>
#include <pulp/vellum_components/inspector_overlay.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/view.hpp>

#include "render_helpers.hpp"

#include <string>
#include <string_view>

using namespace pulp::vellum_components;
using namespace pulp::vellum_components::testing;
using overlay::AtlasInfo;
using overlay::AtlasInventory;
using overlay::AtlasKind;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;
using ::vellum::view::KeyCode;
using ::vellum::view::KeyEvent;

namespace {

// Anything with these four accessors can be snapshotted.
struct FakeAtlas {
    int w = 256;
    int h = 256;
    std::size_t entries = 0;
    float fill = 0.0f;
    int width() const {
        return w;
    }
    int height() const {
        return h;
    }
    std::size_t entry_count() const {
        return entries;
    }
    float occupancy() const {
        return fill;
    }
};

int count_text_containing(const RecordingCanvas& canvas, std::string_view needle) {
    int n = 0;
    for (const auto& cmd : canvas.commands())
        if (cmd.type == DrawCommand::Type::fill_text && cmd.text.find(needle) != std::string::npos)
            ++n;
    return n;
}

KeyEvent key(KeyCode code, std::uint16_t modifiers = 0) {
    KeyEvent e;
    e.key = code;
    e.modifiers = modifiers;
    e.is_down = true;
    return e;
}

// Two half-full atlases, one labelled and one by its kind.
AtlasInventory make_inventory() {
    AtlasInventory inventory;
    inventory.add_atlas(FakeAtlas{256, 256, 4, 0.5f}, AtlasKind::image, "images");
    inventory.add_atlas(FakeAtlas{512, 512, 9, 0.5f}, AtlasKind::glyph);
    return inventory;
}

} // namespace

TEST_CASE("an atlas snapshot copies the atlas and names it", "[pulp-vellum-components][atlas]") {
    const AtlasInfo labelled =
        AtlasInventory::snapshot(FakeAtlas{128, 64, 7, 0.25f}, AtlasKind::path, "paths", 2);
    REQUIRE(labelled.label == "paths");
    REQUIRE(labelled.kind == AtlasKind::path);
    REQUIRE(labelled.width == 128);
    REQUIRE(labelled.height == 64);
    REQUIRE(labelled.pages == 2);
    REQUIRE(labelled.entries == 7);
    REQUIRE(labelled.occupancy == 0.25f);

    // No label falls back to the kind's name; fewer than one page counts as one.
    const AtlasInfo plain = AtlasInventory::snapshot(FakeAtlas{}, AtlasKind::gradient, {}, 0);
    REQUIRE(plain.label == "gradient");
    REQUIRE(plain.pages == 1);
}

TEST_CASE("kind names", "[pulp-vellum-components][atlas]") {
    REQUIRE(std::string_view(overlay::atlas_kind_name(AtlasKind::glyph)) == "glyph");
    REQUIRE(std::string_view(overlay::atlas_kind_name(AtlasKind::image)) == "image");
    REQUIRE(std::string_view(overlay::atlas_kind_name(AtlasKind::gradient)) == "gradient");
    REQUIRE(std::string_view(overlay::atlas_kind_name(AtlasKind::path)) == "path");
}

TEST_CASE("capacity multiplies the page and ignores negatives", "[pulp-vellum-components][atlas]") {
    AtlasInfo info;
    info.width = 100;
    info.height = 50;
    info.pages = 3;
    REQUIRE(info.texel_capacity() == 15000);
    info.width = -5;
    REQUIRE(info.texel_capacity() == 0);
    info.width = 100;
    info.pages = 0;
    REQUIRE(info.texel_capacity() == 5000);
}

TEST_CASE("occupancy is clamped and rounded to a whole percentage",
          "[pulp-vellum-components][atlas]") {
    AtlasInfo info;
    info.occupancy = 0.504f;
    REQUIRE(info.occupancy_percent() == 50);
    info.occupancy = 0.505f;
    REQUIRE(info.occupancy_percent() == 51);
    info.occupancy = 1.7f;
    REQUIRE(info.occupancy_percent() == 100);
    info.occupancy = -0.4f;
    REQUIRE(info.occupancy_percent() == 0);
}

TEST_CASE("the inventory totals pages and entries and averages occupancy",
          "[pulp-vellum-components][atlas]") {
    AtlasInventory inventory;
    REQUIRE(inventory.empty());
    REQUIRE(inventory.average_occupancy() == 0.0f);
    inventory.add_atlas(FakeAtlas{256, 256, 4, 0.2f}, AtlasKind::image, "a", 2);
    inventory.add_atlas(FakeAtlas{256, 256, 6, 5.0f}, AtlasKind::glyph, "b", 3);
    REQUIRE_FALSE(inventory.empty());
    REQUIRE(inventory.size() == 2);
    REQUIRE(inventory.total_pages() == 5);
    REQUIRE(inventory.total_entries() == 10);
    // The second occupancy clamps to 1 before it is averaged.
    REQUIRE(inventory.average_occupancy() == 0.6f);
    inventory.clear();
    REQUIRE(inventory.empty());
}

TEST_CASE("an atlas added with no page count still counts as a page",
          "[pulp-vellum-components][atlas]") {
    AtlasInventory inventory;
    AtlasInfo info;
    info.pages = 0;
    inventory.add(info);
    REQUIRE(inventory.total_pages() == 1);
}

TEST_CASE("the panel grows by a fixed step for each atlas", "[pulp-vellum-components][atlas]") {
    REQUIRE(overlay::atlas_panel_height(2) - overlay::atlas_panel_height(1) == 36.0f);
    // No rows is a heading and a message, about one row.
    REQUIRE(overlay::atlas_panel_height(0) == overlay::atlas_panel_height(1) - 36.0f + 15.0f);
}

TEST_CASE("A toggles the atlas viewer while the overlay is on and only then",
          "[pulp-vellum-components][atlas]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    REQUIRE_FALSE(overlay.handle_key_event(key(KeyCode::a)));
    REQUIRE_FALSE(overlay.atlas_viewer_visible());

    overlay.set_active(true);
    REQUIRE(overlay.handle_key_event(key(KeyCode::a)));
    REQUIRE(overlay.atlas_viewer_visible());
    REQUIRE(overlay.handle_key_event(key(KeyCode::a)));
    REQUIRE_FALSE(overlay.atlas_viewer_visible());
    REQUIRE_FALSE(overlay.handle_key_event(key(KeyCode::a, ::vellum::view::kModShift)));
}

TEST_CASE("an open text edit keeps A for itself", "[pulp-vellum-components][atlas]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<::vellum::view::Label>("copy");
    label->set_bounds({10, 10, 80, 20});
    View* label_ptr = label.get();
    root.add_child(std::move(label));
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    REQUIRE(overlay.begin_text_edit(label_ptr));
    REQUIRE_FALSE(overlay.handle_key_event(key(KeyCode::a)));
    REQUIRE_FALSE(overlay.atlas_viewer_visible());
}

TEST_CASE("the viewer paints a row for each atlas", "[pulp-vellum-components][atlas]") {
    View root;
    root.set_bounds({0, 0, 500, 720});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    const AtlasInventory inventory = make_inventory();
    overlay.set_atlas_inventory(&inventory);
    overlay.set_atlas_viewer_visible(true);

    RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);
    REQUIRE(count_text_containing(canvas, "Texture Atlases") >= 1);
    REQUIRE(count_text_containing(canvas, "images") >= 1);
    REQUIRE(count_text_containing(canvas, "glyph") >= 1);
    REQUIRE(count_text_containing(canvas, "50%") >= 1);
    REQUIRE(count_text_containing(canvas, "2 pages") >= 1);
    REQUIRE(count_text_containing(canvas, "13 entries") >= 1);
    REQUIRE(overlay.atlas_row_count() == 2);
}

TEST_CASE("a multi-page atlas shows its page count", "[pulp-vellum-components][atlas]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    AtlasInventory inventory;
    inventory.add_atlas(FakeAtlas{1024, 512, 3, 0.4f}, AtlasKind::glyph, "glyphs", 2);
    inventory.add_atlas(FakeAtlas{256, 256, 1, 0.4f}, AtlasKind::image, "single", 1);
    overlay.set_atlas_inventory(&inventory);
    overlay.set_atlas_viewer_visible(true);
    RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "glyphs  1024x512 x2p") >= 1);
    // A single page shows no suffix.
    REQUIRE(count_text_containing(canvas, "single  256x256") >= 1);
    REQUIRE(count_text_containing(canvas, "x1p") == 0);
}

TEST_CASE("no inventory says none is attached, not that the GPU failed",
          "[pulp-vellum-components][atlas]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_atlas_viewer_visible(true);
    RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "no atlas inventory attached") >= 1);
    REQUIRE(count_text_containing(canvas, "GPU atlas unavailable") == 0);
    REQUIRE(overlay.atlas_row_count() == 0);
}

TEST_CASE("an empty inventory shows the unavailable state", "[pulp-vellum-components][atlas]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_atlas_viewer_visible(true);
    const AtlasInventory empty;
    overlay.set_atlas_inventory(&empty);
    RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "GPU atlas unavailable") >= 1);
    REQUIRE(overlay.atlas_row_count() == 0);
}

TEST_CASE("the viewer is silent while it is off", "[pulp-vellum-components][atlas]") {
    View root;
    root.set_bounds({0, 0, 500, 720});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    const AtlasInventory inventory = make_inventory();
    overlay.set_atlas_inventory(&inventory);
    RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "Texture Atlases") == 0);
    REQUIRE(overlay.atlas_row_count() == 0);
}

TEST_CASE("dismissing the overlay clears the row count and keeps the toggle",
          "[pulp-vellum-components][atlas]") {
    View root;
    root.set_bounds({0, 0, 500, 720});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    const AtlasInventory inventory = make_inventory();
    overlay.set_atlas_inventory(&inventory);
    overlay.set_atlas_viewer_visible(true);
    RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(overlay.atlas_row_count() == 2);

    overlay.set_active(false);
    REQUIRE(overlay.atlas_row_count() == 0);
    REQUIRE(overlay.atlas_viewer_visible());
}

TEST_CASE("the pass viewer takes the panel when both are on", "[pulp-vellum-components][atlas]") {
    View root;
    root.set_bounds({0, 0, 500, 720});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    const AtlasInventory inventory = make_inventory();
    overlay.set_atlas_inventory(&inventory);
    overlay.set_atlas_viewer_visible(true);
    overlay.set_pass_viewer_enabled(true);
    RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "Render Passes") >= 1);
    REQUIRE(count_text_containing(canvas, "Texture Atlases") == 0);
    REQUIRE(overlay.atlas_row_count() == 0);
}

TEST_CASE("occupancy bars turn amber at 60 percent and red at 85",
          "[pulp-vellum-components][atlas][pixels]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    AtlasInventory inventory;
    inventory.add_atlas(FakeAtlas{256, 256, 2, 0.59f}, AtlasKind::image, "low");
    inventory.add_atlas(FakeAtlas{256, 256, 5, 0.60f}, AtlasKind::glyph, "mid");
    inventory.add_atlas(FakeAtlas{256, 256, 8, 0.85f}, AtlasKind::path, "high");
    overlay.set_atlas_inventory(&inventory);
    overlay.set_atlas_viewer_visible(true);
    install_inspector_hooks(overlay);

    const auto frame = render(root, 500, 320);
    maybe_write_png(root, 500, 320, "inspector-overlay-atlas-viewer.png");
    uninstall_inspector_hooks();

    // Each row is 36 px tall under the 19 px heading; its bar is 15 px below the
    // row top and 8 px high, starting at the panel's left padding.
    const std::uint32_t panel_left = 500 - 12 - 260 + 8;
    const auto bar_pixel = [&](int row) {
        // Near the left end of the fill, clear of the rounded cap.
        return frame.at(panel_left + 6, 12 + 19 + 36 * row + 15 + 4);
    };
    // Green below 60 percent, amber from 60, red from 85.
    REQUIRE(near(bar_pixel(0), 89, 217, 115, 6));
    REQUIRE(near(bar_pixel(1), 242, 166, 64, 6));
    REQUIRE(near(bar_pixel(2), 242, 102, 97, 6));
}
