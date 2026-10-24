// Theme-painted form controls in a browser capture lower onto the widgets'
// Chromium-refresh skins; every other control and every widget Pulp builds
// itself keeps the platform skin.
#include <pulp/view/buttons.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace pulp::view;

namespace {

IRNode captured_input(std::string id, std::string control, float left, float top, float width,
                      float height) {
    IRNode node;
    node.type = "frame";
    node.stable_anchor_id = std::move(id);
    node.style.position = "absolute";
    node.style.left = left;
    node.style.top = top;
    node.style.width = width;
    node.style.height = height;
    node.attributes["source_tag"] = "input";
    node.attributes["native_appearance"] = "auto";
    node.attributes["native_theme"] = "native";
    node.attributes["ua_control"] = std::move(control);
    return node;
}

DesignIR panel_with(std::initializer_list<IRNode> children) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.style.width = 1280.0f;
    ir.root.style.height = 1200.0f;
    for (const auto& child : children)
        ir.root.children.push_back(child);
    return ir;
}

} // namespace

TEST_CASE("a theme-painted range lowers to a Chromium-skinned RangeSlider",
          "[view][import][native-materializer][form-controls]") {
    auto node = captured_input("morph", "range", 771.59375f, 764.5f, 90.0f, 16.0f);
    node.attributes["ua_min"] = "0";
    node.attributes["ua_max"] = "1";
    node.attributes["ua_step"] = "0.001";
    node.attributes["ua_value"] = "0.25";
    node.attributes["ua_disabled"] = "1";
    node.attributes["ua_accent"] = "rgb(71, 180, 235)";
    node.attributes["ua_color_scheme"] = "normal";
    // Chromium's CSS-edge snap of that box, as the capture records it.
    node.attributes["browser_box_paint_left_px"] = "1544";
    node.attributes["browser_box_paint_top_px"] = "1530";
    node.attributes["browser_box_paint_right_px"] = "1724";
    node.attributes["browser_box_paint_bottom_px"] = "1562";
    node.attributes["browser_box_paint_dpr"] = "2";
    const auto ir = panel_with({node});

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    root->layout_children();
    REQUIRE(root->child_count() == 1);
    auto* slider = dynamic_cast<RangeSlider*>(root->child_at(0));
    REQUIRE(slider != nullptr);
    CHECK(slider->skin() == UaSkin::chromium_refresh);
    CHECK(slider->color_scheme() == UaColorScheme::light);
    CHECK(slider->min_value() == Catch::Approx(0.0f));
    CHECK(slider->max_value() == Catch::Approx(1.0f));
    CHECK(slider->step() == Catch::Approx(0.001f));
    CHECK(slider->value() == Catch::Approx(0.25f));
    CHECK_FALSE(slider->enabled());
    REQUIRE(slider->has_accent_color());
    CHECK(slider->accent_color().r == Catch::Approx(71.0f / 255.0f));

    // The skin paints in the snapped box Chromium painted, not the fractional
    // layout box: (772, 765) is (0.40625, 0.5) CSS px right of and below it.
    const Rect snapped = slider->self_paint_bounds();
    CHECK(snapped.x == Catch::Approx(0.40625f));
    CHECK(snapped.y == Catch::Approx(0.5f));
    CHECK(snapped.width == Catch::Approx(90.0f));
    CHECK(snapped.height == Catch::Approx(16.0f));
    const auto parts = slider->chromium_refresh_parts(2.0f);
    const auto expected =
        RangeSlider::chromium_refresh_parts(snapped, 0.25f, false, UaControlState::disabled,
                                            UaColorScheme::light, slider->accent_color(), 2.0f);
    CHECK(parts.track.x == Catch::Approx(expected.track.x));
    CHECK(parts.track.y == Catch::Approx(expected.track.y));
    CHECK(parts.thumb.x == Catch::Approx(expected.thumb.x));
}

TEST_CASE("HTML range defaults apply when the capture recorded no bounds or value",
          "[view][import][native-materializer][form-controls]") {
    auto node = captured_input("bare", "range", 10.0f, 10.0f, 129.0f, 16.0f);
    node.attributes["ua_color_scheme"] = "only dark";
    auto root = build_native_view_tree(panel_with({node}), {}, {});
    auto* slider = dynamic_cast<RangeSlider*>(root->child_at(0));
    REQUIRE(slider != nullptr);
    CHECK(slider->min_value() == Catch::Approx(0.0f));
    CHECK(slider->max_value() == Catch::Approx(100.0f));
    CHECK(slider->step() == Catch::Approx(1.0f));
    CHECK(slider->value() == Catch::Approx(50.0f));
    CHECK(slider->color_scheme() == UaColorScheme::dark);
    CHECK(slider->enabled());
    CHECK_FALSE(slider->has_accent_color());

    // `step="any"` means no stepping; an invalid step falls back to 1.
    node.attributes["ua_step"] = "any";
    node.attributes["ua_value"] = "33.3";
    slider =
        dynamic_cast<RangeSlider*>(build_native_view_tree(panel_with({node}), {}, {})->child_at(0));
    REQUIRE(slider != nullptr);
    CHECK(slider->step() == Catch::Approx(0.0f));
    CHECK(slider->value() == Catch::Approx(33.3f));
    node.attributes["ua_step"] = "-2";
    slider =
        dynamic_cast<RangeSlider*>(build_native_view_tree(panel_with({node}), {}, {})->child_at(0));
    CHECK(slider->step() == Catch::Approx(1.0f));
}

TEST_CASE("theme-painted checkboxes and radios lower to the Chromium skin with their state",
          "[view][import][native-materializer][form-controls]") {
    auto checked = captured_input("on", "checkbox", 10.0f, 10.0f, 13.0f, 13.0f);
    checked.attributes["ua_checked"] = "1";
    checked.attributes["ua_accent"] = "rgb(196, 98, 42)";
    auto off = captured_input("off", "checkbox", 30.0f, 10.0f, 13.0f, 13.0f);
    off.attributes["ua_checked"] = "0";
    off.attributes["ua_disabled"] = "1";
    auto first = captured_input("r1", "radio", 50.0f, 10.0f, 13.0f, 13.0f);
    first.attributes["ua_checked"] = "1";
    first.attributes["ua_radio_group"] = "mode";
    auto second = captured_input("r2", "radio", 70.0f, 10.0f, 13.0f, 13.0f);
    second.attributes["ua_checked"] = "0";
    second.attributes["ua_radio_group"] = "mode";
    auto other = captured_input("r3", "radio", 90.0f, 10.0f, 13.0f, 13.0f);
    other.attributes["ua_radio_group"] = "other";

    auto root = build_native_view_tree(panel_with({checked, off, first, second, other}), {}, {});
    REQUIRE(root->child_count() == 5);
    std::vector<Checkbox*> boxes;
    for (std::size_t i = 0; i < 5; ++i) {
        auto* box = dynamic_cast<Checkbox*>(root->child_at(i));
        REQUIRE(box != nullptr);
        CHECK(box->skin() == UaSkin::chromium_refresh);
        boxes.push_back(box);
    }
    CHECK(boxes[0]->kind() == Checkbox::Kind::checkbox);
    CHECK(boxes[0]->is_checked());
    REQUIRE(boxes[0]->accent_color().has_value());
    CHECK_FALSE(boxes[1]->is_checked());
    CHECK_FALSE(boxes[1]->enabled());
    CHECK(boxes[2]->kind() == Checkbox::Kind::radio);
    CHECK(boxes[2]->is_checked());
    // One `name` is one group, distinct from another name's group.
    CHECK(boxes[2]->radio_group() != 0);
    CHECK(boxes[2]->radio_group() == boxes[3]->radio_group());
    CHECK(boxes[4]->radio_group() != boxes[2]->radio_group());
}

namespace {

IRNode captured_button(std::string id, float top) {
    auto node = captured_input(std::move(id), "button", 10.0f, top, 80.0f, 24.0f);
    node.attributes["source_tag"] = "button";
    IRNode text;
    text.type = "text";
    text.text_content = "Save preset";
    text.style.position = "absolute";
    text.style.left = 8.0f;
    text.style.top = 4.0f;
    text.style.width = 64.0f;
    text.style.height = 16.0f;
    node.children.push_back(text);
    return node;
}

} // namespace

TEST_CASE("a theme-painted button lowers to a ButtonSurface that keeps its content",
          "[view][import][native-materializer][form-controls]") {
    auto node = captured_button("save", 10.25f);
    node.attributes["ua_color_scheme"] = "dark";
    node.attributes["ua_disabled"] = "1";
    node.attributes["browser_box_paint_left_px"] = "20";
    node.attributes["browser_box_paint_top_px"] = "20";
    node.attributes["browser_box_paint_right_px"] = "180";
    node.attributes["browser_box_paint_bottom_px"] = "69";
    node.attributes["browser_box_paint_dpr"] = "2";
    auto root = build_native_view_tree(panel_with({node, captured_button("plain", 50.0f)}), {}, {});
    REQUIRE(root != nullptr);
    root->layout_children();
    REQUIRE(root->child_count() == 2);
    auto* surface = dynamic_cast<ButtonSurface*>(root->child_at(0));
    REQUIRE(surface != nullptr);
    CHECK(surface->color_scheme() == UaColorScheme::dark);
    CHECK_FALSE(surface->enabled());
    CHECK(surface->control_state() == UaControlState::disabled);

    // The content stays the button's child, and the button takes the pointer
    // so hover and press land on the surface rather than its label.
    REQUIRE(surface->child_count() == 1);
    auto* label = dynamic_cast<Label*>(surface->child_at(0));
    REQUIRE(label != nullptr);
    CHECK(label->text() == "Save preset");
    CHECK_FALSE(label->hit_testable());

    // The surface paints in the snapped box: (10, 10) is 0.25 CSS px above the
    // fractional layout box.
    const Rect snapped = surface->self_paint_bounds();
    CHECK(snapped.y == Catch::Approx(-0.25f));
    CHECK(snapped.height == Catch::Approx(24.5f));

    // A button with no recorded scheme is light and enabled.
    auto* plain = dynamic_cast<ButtonSurface*>(root->child_at(1));
    REQUIRE(plain != nullptr);
    CHECK(plain->color_scheme() == UaColorScheme::light);
    CHECK(plain->enabled());
}

TEST_CASE("an author-styled button, and a button from another lane, do not get the browser skin",
          "[view][import][native-materializer][form-controls]") {
    // Author box styling dropped the theme, so the capture recorded no theme
    // state and kept the button's own paint.
    auto author = captured_button("styled", 10.0f);
    author.attributes["native_theme"] = "author";
    author.attributes.erase("ua_control");
    author.style.background_color = "rgb(200, 0, 0)";
    // A verdict without recorded state is not enough either.
    auto stateless = captured_button("bare", 50.0f);
    stateless.attributes.erase("ua_control");
    auto root = build_native_view_tree(panel_with({author, stateless}), {}, {});
    REQUIRE(root->child_count() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(dynamic_cast<ButtonSurface*>(root->child_at(i)) == nullptr);
        CHECK(root->child_at(i)->child_count() == 1);
    }

    // A designed button from another import lane stays Pulp's TextButton.
    IRNode figma_button;
    figma_button.type = "button";
    figma_button.stable_anchor_id = "figma:save";
    figma_button.text_content = "Save";
    figma_button.style.width = 80.0f;
    figma_button.style.height = 24.0f;
    root = build_native_view_tree(panel_with({figma_button}), {}, {});
    REQUIRE(root->child_count() == 1);
    CHECK(dynamic_cast<ButtonSurface*>(root->child_at(0)) == nullptr);
    CHECK(dynamic_cast<TextButton*>(root->child_at(0)) != nullptr);
}

TEST_CASE("a theme-painted button input lowers to a ButtonSurface labelled by its value",
          "[view][import][native-materializer][form-controls]") {
    // The capture draws a button input's value as a `#control-value` text
    // child, so the input carries its label the way a <button> carries its
    // content.
    const auto input_button = [](std::string id, float top) {
        auto node = captured_input(std::move(id), "button", 10.0f, top, 80.0f, 24.0f);
        node.attributes["type"] = "submit";
        IRNode value;
        value.type = "text";
        value.name = "#control-value";
        value.text_content = "Submit";
        value.style.position = "absolute";
        value.style.left = 7.0f;
        value.style.top = 2.0f;
        value.style.width = 66.0f;
        value.style.height = 20.0f;
        node.children.push_back(value);
        return node;
    };
    auto themed = input_button("send", 10.0f);
    themed.attributes["ua_disabled"] = "1";
    auto styled = input_button("styled", 50.0f);
    styled.attributes["native_theme"] = "author";
    styled.attributes.erase("ua_control");
    styled.style.background_color = "rgb(200, 0, 0)";
    auto root = build_native_view_tree(panel_with({themed, styled}), {}, {});
    REQUIRE(root->child_count() == 2);

    auto* surface = dynamic_cast<ButtonSurface*>(root->child_at(0));
    REQUIRE(surface != nullptr);
    CHECK_FALSE(surface->enabled());
    REQUIRE(surface->child_count() == 1);
    auto* label = dynamic_cast<Label*>(surface->child_at(0));
    REQUIRE(label != nullptr);
    CHECK(label->text() == "Submit");

    // Author styling dropped the theme: the input keeps its own paint.
    CHECK(dynamic_cast<ButtonSurface*>(root->child_at(1)) == nullptr);
    REQUIRE(root->child_at(1)->child_count() == 1);
    CHECK(dynamic_cast<Label*>(root->child_at(1)->child_at(0)) != nullptr);
}

TEST_CASE("controls the theme did not paint, and widgets Pulp builds, keep the platform skin",
          "[view][import][native-materializer][form-controls]") {
    // Author box styling dropped the theme: the capture painted it from CSS.
    auto author = captured_input("styled", "range", 10.0f, 10.0f, 129.0f, 16.0f);
    author.attributes["native_theme"] = "author";
    // No verdict at all is unknown, not native.
    auto unresolved = captured_input("unknown", "checkbox", 10.0f, 40.0f, 13.0f, 13.0f);
    unresolved.attributes["native_theme"] = "unresolved";
    auto root = build_native_view_tree(panel_with({author, unresolved}), {}, {});
    REQUIRE(root->child_count() == 2);
    CHECK(dynamic_cast<RangeSlider*>(root->child_at(0)) == nullptr);
    CHECK(dynamic_cast<Checkbox*>(root->child_at(1)) == nullptr);

    // A checkbox from another import lane carries no captured theme state.
    IRNode figma_checkbox;
    figma_checkbox.type = "input";
    figma_checkbox.stable_anchor_id = "figma:bypass";
    figma_checkbox.style.width = 20.0f;
    figma_checkbox.style.height = 20.0f;
    figma_checkbox.attributes["type"] = "checkbox";
    root = build_native_view_tree(panel_with({figma_checkbox}), {}, {});
    auto* checkbox = dynamic_cast<Checkbox*>(root->child_at(0));
    REQUIRE(checkbox != nullptr);
    CHECK(checkbox->skin() == UaSkin::platform);
    CHECK(checkbox->kind() == Checkbox::Kind::checkbox);

    // Default-constructed widgets are unchanged, and a view with no captured
    // paint rect paints in its own bounds.
    RangeSlider slider;
    Checkbox box;
    CHECK(slider.skin() == UaSkin::platform);
    CHECK(box.skin() == UaSkin::platform);
    slider.set_bounds({5.0f, 5.0f, 100.0f, 16.0f});
    const Rect own = slider.self_paint_bounds();
    CHECK(own.x == 0.0f);
    CHECK(own.y == 0.0f);
    CHECK(own.width == 100.0f);
    CHECK(own.height == 16.0f);
}

TEST_CASE("generated C++ builds the same theme-painted controls as the native lane",
          "[view][import][native-materializer][form-controls][cpp-codegen]") {
    auto range = captured_input("morph", "range", 10.0f, 10.0f, 90.0f, 16.0f);
    range.attributes["ua_min"] = "0";
    range.attributes["ua_max"] = "1";
    range.attributes["ua_value"] = "0.25";
    range.attributes["ua_disabled"] = "1";
    range.attributes["ua_color_scheme"] = "dark";
    auto radio = captured_input("r1", "radio", 10.0f, 40.0f, 13.0f, 13.0f);
    radio.attributes["ua_checked"] = "1";
    radio.attributes["ua_radio_group"] = "mode";
    const auto result = generate_pulp_cpp(panel_with({range, radio}), {}, {});
    const auto has = [&](const std::string& text) {
        return result.source.find(text) != std::string::npos;
    };
    CHECK(has("std::make_unique<pulp::view::RangeSlider>()"));
    CHECK(has("->set_skin(pulp::view::UaSkin::chromium_refresh);"));
    CHECK(has("->set_color_scheme(pulp::view::UaColorScheme::dark);"));
    CHECK(has("->set_enabled(false);"));
    CHECK(has("->set_kind(pulp::view::Checkbox::Kind::radio);"));
    CHECK(has("->set_checked(true);"));
    // The baked group id is the native lane's, so baked radios exclude each other too.
    auto root = build_native_view_tree(panel_with({range, radio}), {}, {});
    auto* native_radio = dynamic_cast<Checkbox*>(root->child_at(1));
    REQUIRE(native_radio != nullptr);
    CHECK(has("->set_radio_group(" + std::to_string(native_radio->radio_group()) + ");"));
}

TEST_CASE("generated C++ builds the same ButtonSurface as the native lane",
          "[view][import][native-materializer][form-controls][cpp-codegen]") {
    auto button = captured_button("save", 10.0f);
    button.attributes["ua_color_scheme"] = "dark";
    button.attributes["ua_disabled"] = "1";
    // An accent the page sets never reaches a push button.
    button.attributes["ua_accent"] = "rgb(71, 180, 235)";
    const auto result = generate_pulp_cpp(panel_with({button}), {}, {});
    const auto has = [&](const std::string& text) {
        return result.source.find(text) != std::string::npos;
    };
    CHECK(has("std::make_unique<pulp::view::ButtonSurface>()"));
    CHECK(has("->set_color_scheme(pulp::view::UaColorScheme::dark);"));
    CHECK(has("->set_enabled(false);"));
    CHECK(has("Save preset"));
    CHECK_FALSE(has("set_skin("));
    CHECK_FALSE(has("set_accent_color("));
    CHECK_FALSE(has("pulp::view::TextButton"));
}
