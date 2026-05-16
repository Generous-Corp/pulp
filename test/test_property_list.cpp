#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/property_list.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace pulp::view;
using namespace pulp::canvas;

namespace {

bool has_text(const RecordingCanvas& canvas, const std::string& text) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const DrawCommand& cmd) {
                           return cmd.type == DrawCommand::Type::fill_text &&
                                  cmd.text == text;
                       });
}

size_t text_count(const RecordingCanvas& canvas, const std::string& text) {
    return static_cast<size_t>(std::count_if(canvas.commands().begin(),
                                            canvas.commands().end(),
                                            [&](const DrawCommand& cmd) {
                                                return cmd.type == DrawCommand::Type::fill_text &&
                                                       cmd.text == text;
                                            }));
}

} // namespace

TEST_CASE("PropertyList paints value variants and categories", "[view][property_list]") {
    PropertyList list;
    list.set_bounds({0, 0, 240, 180});
    list.set_row_height(24.0f);
    list.set_label_width_fraction(0.5f);
    list.set_properties({
        {"name", "Name", std::string("Pulp"), false, "General"},
        {"gain", "Gain", 0.375f, false, "General"},
        {"voices", "Voices", 8, false, "Synth"},
        {"enabled", "Enabled", true, true, "Synth"},
        {"color", "Color", Color::rgba8(128, 64, 16), false, "Visual"},
    });

    RecordingCanvas canvas;
    list.paint(canvas);

    REQUIRE(text_count(canvas, "General") == 1);
    REQUIRE(text_count(canvas, "Synth") == 1);
    REQUIRE(text_count(canvas, "Visual") == 1);
    REQUIRE(has_text(canvas, "Name"));
    REQUIRE(has_text(canvas, "Pulp"));
    REQUIRE(has_text(canvas, "0.38"));
    REQUIRE(has_text(canvas, "8"));
    REQUIRE(has_text(canvas, "true"));
    REQUIRE(has_text(canvas, "#804010"));
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 5);
}

TEST_CASE("PropertyList intrinsic height accounts for category display", "[view][property_list]") {
    PropertyList list;
    list.set_row_height(10.0f);
    list.set_properties({
        {"a", "A", std::string("one"), false, "First"},
        {"b", "B", std::string("two"), false, "First"},
        {"c", "C", std::string("three"), false, "Second"},
        {"d", "D", std::string("four"), false, ""},
    });

    REQUIRE(list.intrinsic_height() == 56.0f);

    list.set_show_categories(false);
    REQUIRE(list.intrinsic_height() == 40.0f);
}

TEST_CASE("PropertyList mouse toggles editable bools and skips read-only values",
          "[view][property_list]") {
    PropertyList list;
    list.set_bounds({0, 0, 180, 120});
    list.set_row_height(20.0f);
    list.set_properties({
        {"title", "Title", std::string("Main"), false, "General"},
        {"enabled", "Enabled", false, false, "Flags"},
        {"locked", "Locked", true, true, "Flags"},
    });

    std::vector<std::string> changed_keys;
    list.on_change = [&](const std::string& key, PropertyList::PropertyValue value) {
        changed_keys.push_back(key);
        REQUIRE(std::holds_alternative<bool>(value));
    };

    list.on_mouse_down({10.0f, 62.0f});
    auto* enabled = list.find_property("enabled");
    REQUIRE(enabled != nullptr);
    REQUIRE(std::get<bool>(enabled->value));
    REQUIRE(changed_keys == std::vector<std::string>{"enabled"});

    list.on_mouse_down({10.0f, 82.0f});
    auto* locked = list.find_property("locked");
    REQUIRE(locked != nullptr);
    REQUIRE(std::get<bool>(locked->value));
    REQUIRE(changed_keys.size() == 1);

    list.on_mouse_down({10.0f, 300.0f});
    REQUIRE(changed_keys.size() == 1);
}

TEST_CASE("PropertyList set_value only mutates existing keys", "[view][property_list]") {
    PropertyList list;
    list.set_properties({
        {"gain", "Gain", 0.25f, false, ""},
    });

    list.set_value("missing", 4);
    REQUIRE(list.properties().size() == 1);
    REQUIRE(list.find_property("missing") == nullptr);

    list.set_value("gain", 0.75f);
    auto* gain = list.find_property("gain");
    REQUIRE(gain != nullptr);
    REQUIRE(std::get<float>(gain->value) == 0.75f);
}

TEST_CASE("PropertyList category visibility and label fallback paint paths",
          "[view][property_list][coverage][issue-653]") {
    PropertyList list;
    list.set_bounds({0, 0, 220, 100});
    list.set_row_height(20.0f);
    list.set_label_width_fraction(0.25f);
    list.set_show_categories(false);
    list.set_properties({
        {"key_only", "", std::string("value"), false, "Hidden"},
        {"readonly", "Read Only", true, true, "Hidden"},
    });

    RecordingCanvas canvas;
    list.paint(canvas);

    REQUIRE_FALSE(has_text(canvas, "Hidden"));
    REQUIRE(has_text(canvas, "key_only"));
    REQUIRE(has_text(canvas, "value"));
    REQUIRE(has_text(canvas, "Read Only"));
    REQUIRE(has_text(canvas, "true"));
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 2);
    REQUIRE(list.intrinsic_height() == 40.0f);
}

TEST_CASE("PropertyList selection highlight covers non-bool rows and misses",
          "[view][property_list][coverage][issue-653]") {
    PropertyList list;
    list.set_bounds({0, 0, 180, 100});
    list.set_row_height(20.0f);
    list.set_properties({
        {"name", "Name", std::string("Main"), false, ""},
        {"bypass", "Bypass", false, false, ""},
    });

    int changes = 0;
    list.on_change = [&](const std::string&, PropertyList::PropertyValue) {
        ++changes;
    };

    list.on_mouse_down({5.0f, 5.0f});
    RecordingCanvas selected_text;
    list.paint(selected_text);
    REQUIRE(selected_text.count(DrawCommand::Type::fill_rect) == 2);
    REQUIRE(changes == 0);

    list.on_mouse_down({5.0f, 200.0f});
    RecordingCanvas after_miss;
    list.paint(after_miss);
    REQUIRE(after_miss.count(DrawCommand::Type::fill_rect) == 1);

    list.on_mouse_down({5.0f, 25.0f});
    REQUIRE(changes == 1);
    auto* bypass = list.find_property("bypass");
    REQUIRE(bypass != nullptr);
    REQUIRE(std::get<bool>(bypass->value));
}
