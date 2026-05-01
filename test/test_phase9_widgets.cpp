#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/eq_curve_view.hpp>
#include <pulp/view/midi_keyboard.hpp>
#include <pulp/view/color_picker.hpp>
#include <pulp/view/file_drop_zone.hpp>
#include <pulp/view/split_view.hpp>
#include <pulp/view/property_list.hpp>
#include <pulp/view/breadcrumb.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// ── EqCurveView ─────────────────────────────────────────────────────────────

TEST_CASE("EqCurveView band management", "[view][eq_curve]") {
    EqCurveView eq;

    SECTION("Add and remove bands") {
        eq.add_band({1000.0f, 3.0f, 1.0f, EqCurveView::FilterType::peak, true});
        eq.add_band({200.0f, -6.0f, 0.7f, EqCurveView::FilterType::low_shelf, true});
        REQUIRE(eq.band_count() == 2);

        eq.remove_band(0);
        REQUIRE(eq.band_count() == 1);
        REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(200.0, 0.1));
    }

    SECTION("Set bands replaces all") {
        std::vector<EqCurveView::Band> bands = {
            {100.0f, 0.0f, 1.0f, EqCurveView::FilterType::high_pass, true},
            {5000.0f, -3.0f, 2.0f, EqCurveView::FilterType::peak, true}
        };
        eq.set_bands(bands);
        REQUIRE(eq.band_count() == 2);
    }

    SECTION("Frequency and gain ranges") {
        eq.set_frequency_range(10.0f, 24000.0f);
        REQUIRE_THAT(eq.min_frequency(), WithinAbs(10.0, 0.1));
        REQUIRE_THAT(eq.max_frequency(), WithinAbs(24000.0, 0.1));

        eq.set_gain_range(-30.0f, 30.0f);
        REQUIRE_THAT(eq.min_gain(), WithinAbs(-30.0, 0.1));
        REQUIRE_THAT(eq.max_gain(), WithinAbs(30.0, 0.1));
    }

    SECTION("Selected band") {
        eq.add_band({1000.0f, 0.0f, 1.0f});
        eq.set_selected_band(0);
        REQUIRE(eq.selected_band() == 0);
    }

    SECTION("Invalid band operations are no-ops and range setters clamp") {
        eq.add_band({500.0f, -3.0f, 0.8f, EqCurveView::FilterType::peak, true});

        EqCurveView::Band replacement{2000.0f, 6.0f, 2.0f, EqCurveView::FilterType::notch, true};
        eq.set_band(3, replacement);
        eq.remove_band(9);
        REQUIRE(eq.band_count() == 1);
        REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(500.0, 0.1));
        REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(-3.0, 0.1));
        REQUIRE_THAT(eq.bands()[0].q, WithinAbs(0.8, 0.01));

        eq.set_frequency_range(-20.0f, -10.0f);
        REQUIRE_THAT(eq.min_frequency(), WithinAbs(1.0, 0.001));
        REQUIRE_THAT(eq.max_frequency(), WithinAbs(2.0, 0.001));

        eq.set_gain_range(12.0f, 12.0f);
        REQUIRE_THAT(eq.min_gain(), WithinAbs(12.0, 0.001));
        REQUIRE_THAT(eq.max_gain(), WithinAbs(13.0, 0.001));
    }
}

TEST_CASE("EqCurveView spectrum overlay", "[view][eq_curve]") {
    EqCurveView eq;
    std::vector<float> spectrum(128, -60.0f);
    eq.set_spectrum(spectrum.data(), spectrum.size());
    eq.clear_spectrum();
    // Should not crash
}

TEST_CASE("EqCurveView hit testing and drag callbacks", "[view][eq_curve][issue-493]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 200, 100});
    eq.set_frequency_range(100.0f, 10000.0f);
    eq.set_gain_range(-12.0f, 12.0f);
    eq.add_band({1000.0f, 0.0f, 1.0f});

    int selected_index = -1;
    int selected_count = 0;
    int changed_index = -1;
    int changed_count = 0;
    EqCurveView::Band changed_band{};
    eq.on_band_selected = [&](size_t index) {
        selected_index = static_cast<int>(index);
        ++selected_count;
    };
    eq.on_band_changed = [&](size_t index, EqCurveView::Band band) {
        changed_index = static_cast<int>(index);
        changed_band = band;
        ++changed_count;
    };

    eq.on_mouse_down({100, 50});
    REQUIRE(eq.selected_band() == 0);
    REQUIRE(selected_index == 0);
    REQUIRE(selected_count == 1);

    eq.on_mouse_drag({400, -100});
    REQUIRE(changed_index == 0);
    REQUIRE(changed_count == 1);
    REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(10000.0, 0.1));
    REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(12.0, 0.001));
    REQUIRE_THAT(changed_band.frequency, WithinAbs(10000.0, 0.1));
    REQUIRE_THAT(changed_band.gain_db, WithinAbs(12.0, 0.001));

    eq.on_mouse_up({400, -100});
    eq.on_mouse_drag({0, 100});
    REQUIRE(changed_count == 1);
}

TEST_CASE("EqCurveView empty hit does not start a drag", "[view][eq_curve][issue-493]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 200, 100});
    eq.set_frequency_range(100.0f, 10000.0f);
    eq.set_gain_range(-12.0f, 12.0f);
    eq.add_band({1000.0f, 0.0f, 1.0f});

    int selected_count = 0;
    int changed_count = 0;
    eq.on_band_selected = [&](size_t) { ++selected_count; };
    eq.on_band_changed = [&](size_t, EqCurveView::Band) { ++changed_count; };

    eq.on_mouse_down({5, 5});
    REQUIRE(eq.selected_band() == -1);
    REQUIRE(selected_count == 0);

    eq.on_mouse_drag({200, 0});
    REQUIRE(changed_count == 0);
    REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(1000.0, 0.1));
    REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(0.0, 0.001));
}

// ── MidiKeyboard ────────────────────────────────────────────────────────────

TEST_CASE("MidiKeyboard note state", "[view][midi_keyboard]") {
    MidiKeyboard kb;

    SECTION("Note on/off") {
        REQUIRE_FALSE(kb.is_note_on(60));
        kb.note_on(60, 0.8f);
        REQUIRE(kb.is_note_on(60));
        kb.note_off(60);
        REQUIRE_FALSE(kb.is_note_on(60));
    }

    SECTION("All notes off") {
        kb.note_on(60);
        kb.note_on(64);
        kb.note_on(67);
        kb.all_notes_off();
        REQUIRE_FALSE(kb.is_note_on(60));
        REQUIRE_FALSE(kb.is_note_on(64));
        REQUIRE_FALSE(kb.is_note_on(67));
    }

    SECTION("Range clamping") {
        kb.set_range(48, 72);
        REQUIRE(kb.first_note() == 48);
        REQUIRE(kb.last_note() == 72);
    }

    SECTION("Out of range note is safe") {
        REQUIRE_FALSE(kb.is_note_on(-1));
        REQUIRE_FALSE(kb.is_note_on(128));
        kb.note_on(-1);  // Should not crash
        kb.note_on(128); // Should not crash
    }
}

TEST_CASE("MidiKeyboard interaction callback", "[view][midi_keyboard]") {
    MidiKeyboard kb;
    kb.set_range(60, 72);
    kb.set_bounds({0, 0, 300, 80});

    int last_note = -1;
    float last_vel = 0;
    kb.on_note_on = [&](int n, float v) { last_note = n; last_vel = v; };

    // Click near the start should hit a key
    kb.on_mouse_down({5, 40});
    REQUIRE(last_note >= 60);
    REQUIRE(last_vel > 0);
}

// ── ColorPicker ─────────────────────────────────────────────────────────────

TEST_CASE("ColorPicker set/get color", "[view][color_picker]") {
    ColorPicker picker;

    picker.set_color(Color::rgba8(255, 128, 0));
    REQUIRE(picker.color().r8() == 255);
    REQUIRE(picker.color().g8() == 128);
    REQUIRE(picker.color().b8() == 0);
}

TEST_CASE("ColorPicker hex round-trip", "[view][color_picker]") {
    ColorPicker picker;
    picker.set_hex("#ff6600");
    REQUIRE(picker.hex() == "#ff6600");
    REQUIRE(picker.color().r8() == 0xFF);
    REQUIRE(picker.color().g8() == 0x66);
    REQUIRE(picker.color().b8() == 0x00);
}

TEST_CASE("ColorPicker hex alpha and malformed input are stable",
          "[view][color_picker][issue-493]") {
    ColorPicker picker;
    picker.set_hex("#33669980");
    REQUIRE(picker.hex() == "#33669980");
    REQUIRE(picker.color().r8() == 0x33);
    REQUIRE(picker.color().g8() == 0x66);
    REQUIRE(picker.color().b8() == 0x99);
    REQUIRE(picker.color().a8() == 0x80);

    const auto before = picker.hex();
    picker.set_hex("336699");
    REQUIRE(picker.hex() == before);
    picker.set_hex("#3366991234");
    REQUIRE(picker.hex() == before);
    REQUIRE_NOTHROW(picker.set_hex("#zzzzzz"));
    REQUIRE(picker.hex() == before);
}

TEST_CASE("ColorPicker HSL round-trip", "[view][color_picker]") {
    ColorPicker picker;
    HSL hsl{120.0f, 1.0f, 0.5f};
    picker.set_hsl(hsl);
    REQUIRE(picker.color().g > picker.color().r);
    REQUIRE(picker.color().g > picker.color().b);
}

TEST_CASE("ColorPicker swatches", "[view][color_picker]") {
    ColorPicker picker;
    picker.set_swatches({
        Color::rgba8(255, 0, 0),
        Color::rgba8(0, 255, 0),
        Color::rgba8(0, 0, 255)
    });
    REQUIRE(picker.swatches().size() == 3);
}

TEST_CASE("ColorPicker mouse editing updates SL hue alpha and swatches",
          "[view][color_picker][issue-493]") {
    ColorPicker picker;
    picker.set_bounds({0, 0, 200, 280});
    picker.set_hex("#000000ff");

    int changes = 0;
    picker.on_change = [&](Color) { ++changes; };

    picker.on_mouse_down({90, 90});
    REQUIRE(changes == 1);
    REQUIRE(picker.color().r8() > 0);

    picker.on_mouse_down({176, 184});
    REQUIRE(changes == 2);
    REQUIRE_THAT(picker.hsl().h, WithinAbs(360.0, 0.1));

    picker.set_show_alpha(true);
    picker.on_mouse_down({4, 212});
    REQUIRE(changes == 3);
    REQUIRE(picker.color().a8() == 0);
    picker.on_mouse_drag({176, 212});
    REQUIRE(changes == 4);
    REQUIRE(picker.color().a8() == 255);
    picker.on_mouse_up({176, 212});
    picker.on_mouse_drag({4, 212});
    REQUIRE(changes == 4);

    picker.set_show_alpha(false);
    picker.set_swatches({
        Color::rgba8(255, 0, 0),
        Color::rgba8(0, 255, 0),
    });

    picker.on_mouse_down({32, 216});
    REQUIRE(changes == 5);
    REQUIRE(picker.color().g8() == 255);
    REQUIRE(picker.color().r8() == 0);
}

TEST_CASE("ColorPicker paint positions alpha cursor from normalized alpha",
          "[view][color_picker][issue-493]") {
    ColorPicker picker;
    picker.set_bounds({0, 0, 200, 280});
    picker.set_show_alpha(true);
    picker.set_hex("#33669980");

    pulp::canvas::RecordingCanvas canvas;
    picker.paint(canvas);

    bool found_alpha_cursor = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != pulp::canvas::DrawCommand::Type::fill_rect) continue;
        if (command.f[1] < 209.0f || command.f[1] > 211.0f) continue;
        if (command.f[2] != 4.0f || command.f[3] != 24.0f) continue;

        found_alpha_cursor = true;
        REQUIRE(command.f[0] > 86.0f);
        REQUIRE(command.f[0] < 91.0f);
    }
    REQUIRE(found_alpha_cursor);
}

// ── FileDropZone ────────────────────────────────────────────────────────────

TEST_CASE("FileDropZone extension filtering", "[view][file_drop]") {
    FileDropZone zone;
    zone.set_accepted_extensions({".wav", ".aiff"});

    SECTION("Valid extension drag") {
        zone.drag_enter({"test.wav"});
        REQUIRE(zone.is_drag_over());
        REQUIRE(zone.is_drag_valid());
        zone.drag_leave();
        REQUIRE_FALSE(zone.is_drag_over());
    }

    SECTION("Invalid extension drag") {
        zone.drag_enter({"test.exe"});
        REQUIRE(zone.is_drag_over());
        REQUIRE_FALSE(zone.is_drag_valid());
    }

    SECTION("Mixed extensions") {
        zone.drag_enter({"test.wav", "test.exe"});
        REQUIRE_FALSE(zone.is_drag_valid());
    }

    SECTION("Case insensitive") {
        zone.drag_enter({"test.WAV"});
        REQUIRE(zone.is_drag_valid());
    }
}

TEST_CASE("FileDropZone drop callback", "[view][file_drop]") {
    FileDropZone zone;
    zone.set_accepted_extensions({".wav"});

    std::vector<std::string> dropped;
    zone.on_drop = [&](const auto& paths) { dropped = paths; };

    zone.drop({"a.wav", "b.exe", "c.wav"});
    REQUIRE(dropped.size() == 2);
    REQUIRE(dropped[0] == "a.wav");
    REQUIRE(dropped[1] == "c.wav");
}

TEST_CASE("FileDropZone empty extensions accepts all", "[view][file_drop]") {
    FileDropZone zone;
    zone.drag_enter({"anything.xyz"});
    REQUIRE(zone.is_drag_valid());
}

// ── SplitView ───────────────────────────────────────────────────────────────

TEST_CASE("SplitView basic setup", "[view][split_view]") {
    SplitView split;
    split.set_bounds({0, 0, 400, 300});

    auto first = std::make_unique<View>();
    auto second = std::make_unique<View>();
    auto* f = first.get();
    auto* s = second.get();

    split.set_first(std::move(first));
    split.set_second(std::move(second));

    REQUIRE(split.first() == f);
    REQUIRE(split.second() == s);

    split.set_split_fraction(0.3f);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.3, 0.001));

    split.layout_children();
    // First pane should be about 30% of width
    REQUIRE(f->bounds().width < 200);
}

TEST_CASE("SplitView split fraction clamped", "[view][split_view]") {
    SplitView split;
    split.set_split_fraction(-0.5f);
    REQUIRE(split.split_fraction() >= 0.0f);
    split.set_split_fraction(1.5f);
    REQUIRE(split.split_fraction() <= 1.0f);
}

TEST_CASE("SplitView orientation", "[view][split_view]") {
    SplitView split;
    split.set_orientation(SplitView::Orientation::vertical);
    REQUIRE(split.orientation() == SplitView::Orientation::vertical);
}

// ── PropertyList ────────────────────────────────────────────────────────────

TEST_CASE("PropertyList basic operations", "[view][property_list]") {
    PropertyList list;

    std::vector<PropertyList::Property> props = {
        {"name", "Name", std::string("MyPlugin"), false, "General"},
        {"version", "Version", std::string("1.0.0"), true, "General"},
        {"gain", "Gain", 0.5f, false, "Audio"},
        {"bypass", "Bypass", false, false, "Audio"},
        {"color", "Color", Color::rgba8(255, 0, 0), false, "Visual"},
    };

    list.set_properties(props);
    REQUIRE(list.properties().size() == 5);

    auto* found = list.find_property("gain");
    REQUIRE(found != nullptr);
    REQUIRE(std::get<float>(found->value) == 0.5f);

    list.set_value("gain", 0.8f);
    found = list.find_property("gain");
    REQUIRE_THAT(std::get<float>(found->value), WithinAbs(0.8, 0.001));
}

TEST_CASE("PropertyList find missing returns nullptr", "[view][property_list]") {
    PropertyList list;
    REQUIRE(list.find_property("nonexistent") == nullptr);
}

TEST_CASE("PropertyList intrinsic height", "[view][property_list]") {
    PropertyList list;
    list.set_properties({
        {"a", "A", std::string("x"), false, ""},
        {"b", "B", std::string("y"), false, ""},
    });
    REQUIRE(list.intrinsic_height() > 0);
}

// ── Breadcrumb ──────────────────────────────────────────────────────────────

TEST_CASE("Breadcrumb push and pop", "[view][breadcrumb]") {
    Breadcrumb bc;

    bc.push({"Home", "home"});
    bc.push({"Settings", "settings"});
    bc.push({"Audio", "audio"});

    REQUIRE(bc.items().size() == 3);

    auto popped = bc.pop();
    REQUIRE(popped.label == "Audio");
    REQUIRE(bc.items().size() == 2);
}

TEST_CASE("Breadcrumb pop_to", "[view][breadcrumb]") {
    Breadcrumb bc;
    bc.push({"Home", "home"});
    bc.push({"A", "a"});
    bc.push({"B", "b"});
    bc.push({"C", "c"});

    bc.pop_to(1);
    REQUIRE(bc.items().size() == 2);
    REQUIRE(bc.items().back().label == "A");
}

TEST_CASE("Breadcrumb set_items replaces", "[view][breadcrumb]") {
    Breadcrumb bc;
    bc.set_items({{"Root", "root"}, {"Child", "child"}});
    REQUIRE(bc.items().size() == 2);
}

TEST_CASE("Breadcrumb navigate callback", "[view][breadcrumb]") {
    Breadcrumb bc;
    bc.set_bounds({0, 0, 400, 32});
    bc.push({"Home", "home"});
    bc.push({"Settings", "settings"});

    size_t nav_idx = 999;
    bc.on_navigate = [&](size_t idx, const Breadcrumb::Item&) { nav_idx = idx; };

    bc.on_mouse_down({10, 16}); // Should hit first item
    REQUIRE(nav_idx == 0);
}

TEST_CASE("Breadcrumb separator", "[view][breadcrumb]") {
    Breadcrumb bc;
    REQUIRE(bc.separator() == "/");
    bc.set_separator(">");
    REQUIRE(bc.separator() == ">");
}

TEST_CASE("Breadcrumb empty and out-of-range interactions are stable",
          "[view][breadcrumb][coverage]") {
    Breadcrumb bc;
    auto empty = bc.pop();
    REQUIRE(empty.label.empty());
    REQUIRE(empty.id.empty());

    bc.set_items({{"Home", "home"}, {"Settings", "settings"}});
    bc.pop_to(99);
    REQUIRE(bc.items().size() == 2);

    int navigate_calls = 0;
    bc.on_navigate = [&](size_t, const Breadcrumb::Item&) { ++navigate_calls; };
    bc.on_mouse_down({200, 16});
    REQUIRE(navigate_calls == 0);
}

TEST_CASE("Breadcrumb navigation targets later items and ignores separators",
          "[view][breadcrumb][coverage]") {
    Breadcrumb bc;
    bc.set_separator(">");
    bc.set_items({{"Home", "home"}, {"Settings", "settings"}, {"Audio", "audio"}});

    size_t nav_idx = 999;
    std::string nav_id;
    int navigate_calls = 0;
    bc.on_navigate = [&](size_t idx, const Breadcrumb::Item& item) {
        nav_idx = idx;
        nav_id = item.id;
        ++navigate_calls;
    };

    bc.on_mouse_down({60, 16});
    REQUIRE(navigate_calls == 1);
    REQUIRE(nav_idx == 1);
    REQUIRE(nav_id == "settings");

    bc.on_mouse_down({45, 16});
    REQUIRE(navigate_calls == 1);
}

TEST_CASE("Breadcrumb paint emits background, items, and separators",
          "[view][breadcrumb][coverage]") {
    Breadcrumb bc;
    bc.set_bounds({0, 0, 240, 32});
    bc.set_separator(">");
    bc.set_items({{"Home", "home"}, {"Settings", "settings"}, {"Audio", "audio"}});

    pulp::canvas::RecordingCanvas canvas;
    bc.paint(canvas);

    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::set_font) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::set_text_align) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_text) == 5);

    int home_text = 0;
    int separator_text = 0;
    for (const auto& command : canvas.commands()) {
        if (command.type != pulp::canvas::DrawCommand::Type::fill_text)
            continue;
        if (command.text == "Home")
            ++home_text;
        if (command.text == ">")
            ++separator_text;
    }

    REQUIRE(home_text == 1);
    REQUIRE(separator_text == 2);
}
