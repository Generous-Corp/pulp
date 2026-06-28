#include "design_import_generated_cpp_fixture.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <pulp/view/buttons.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>

namespace pulp::test::design_import_cpp_fixture {

namespace tokens {
inline constexpr auto kBgPrimary = pulp::view::Color::rgba8(17, 34, 51, 255);
inline constexpr float kPanelWidth = 320.0f;
inline constexpr float kPanelHeight = 140.0f;
}  // namespace tokens

namespace assets {
inline constexpr const char* kLogo = "logo";
}  // namespace assets

pulp::view::IRAssetManifest bake_asset_manifest() {
    pulp::view::IRAssetManifest manifest;
    manifest.version = 1;
    {
        pulp::view::IRAssetRef asset;
        asset.asset_id = assets::kLogo;
        asset.original_uri = "logo.svg";
        asset.content_hash = "sha256:fixture";
        asset.mime = "image/svg+xml";
        manifest.assets.push_back(std::move(asset));
    }
    return manifest;
}

// auto-extracted: structural name "Header"
std::unique_ptr<pulp::view::View> build_header() {
    auto node_0 = std::make_unique<pulp::view::View>();
    node_0->set_id("header");
    node_0->set_anchor_id("header");
    auto& flex = node_0->flex();
    flex.direction = pulp::view::FlexDirection::row;
    flex.justify_content = pulp::view::FlexJustify::start;
    flex.align_items = pulp::view::FlexAlign::stretch;
    flex.gap = 8.0f;
    flex.preferred_width = tokens::kPanelWidth;
    flex.dim_width = {tokens::kPanelWidth, pulp::view::DimensionUnit::px};
    flex.preferred_height = 32.0f;
    flex.dim_height = {32.0f, pulp::view::DimensionUnit::px};

    auto node_1 = std::make_unique<pulp::view::Label>("Cloud Chorus");
    node_1->set_id("title");
    node_1->set_anchor_id("title");
    node_1->set_access_label("Cloud Chorus");
    auto& title_flex = node_1->flex();
    title_flex.direction = pulp::view::FlexDirection::column;
    title_flex.justify_content = pulp::view::FlexJustify::start;
    title_flex.align_items = pulp::view::FlexAlign::stretch;
    title_flex.preferred_width = 144.0f;
    title_flex.dim_width = {144.0f, pulp::view::DimensionUnit::px};
    title_flex.preferred_height = 24.0f;
    title_flex.dim_height = {24.0f, pulp::view::DimensionUnit::px};
    node_0->add_child(std::move(node_1));

    auto node_2 = std::make_unique<pulp::view::Label>("Generated C++");
    node_2->set_id("badge");
    node_2->set_anchor_id("badge");
    node_2->set_access_label("Generated C++");
    auto& badge_flex = node_2->flex();
    badge_flex.direction = pulp::view::FlexDirection::column;
    badge_flex.justify_content = pulp::view::FlexJustify::start;
    badge_flex.align_items = pulp::view::FlexAlign::stretch;
    badge_flex.preferred_width = 120.0f;
    badge_flex.dim_width = {120.0f, pulp::view::DimensionUnit::px};
    badge_flex.preferred_height = 24.0f;
    badge_flex.dim_height = {24.0f, pulp::view::DimensionUnit::px};
    node_0->add_child(std::move(node_2));

    return node_0;
}

std::unique_ptr<pulp::view::View> build_imported_ui() {
    auto node_0 = std::make_unique<pulp::view::View>();
    node_0->set_id("root");
    node_0->set_anchor_id("root");
    auto& flex = node_0->flex();
    flex.direction = pulp::view::FlexDirection::column;
    flex.justify_content = pulp::view::FlexJustify::start;
    flex.align_items = pulp::view::FlexAlign::stretch;
    flex.gap = 10.0f;
    flex.preferred_width = tokens::kPanelWidth;
    flex.dim_width = {tokens::kPanelWidth, pulp::view::DimensionUnit::px};
    flex.preferred_height = tokens::kPanelHeight;
    flex.dim_height = {tokens::kPanelHeight, pulp::view::DimensionUnit::px};
    node_0->set_background_color(tokens::kBgPrimary);

    node_0->add_child(build_header());

    auto node_1 = std::make_unique<pulp::view::Knob>();
    node_1->set_id("drive");
    node_1->set_anchor_id("drive");
    auto& drive_flex = node_1->flex();
    drive_flex.direction = pulp::view::FlexDirection::column;
    drive_flex.justify_content = pulp::view::FlexJustify::start;
    drive_flex.align_items = pulp::view::FlexAlign::stretch;
    drive_flex.preferred_width = 72.0f;
    drive_flex.dim_width = {72.0f, pulp::view::DimensionUnit::px};
    drive_flex.preferred_height = 72.0f;
    drive_flex.dim_height = {72.0f, pulp::view::DimensionUnit::px};
    node_1->set_label("Drive");
    node_1->set_value(/* imported static param value */ 0.5f);
    node_1->set_default_value(0.5f);
    node_0->add_child(std::move(node_1));

    return node_0;
}

}  // namespace pulp::test::design_import_cpp_fixture
