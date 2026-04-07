#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
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
}

TEST_CASE("EqCurveView spectrum overlay", "[view][eq_curve]") {
    EqCurveView eq;
    std::vector<float> spectrum(128, -60.0f);
    eq.set_spectrum(spectrum.data(), spectrum.size());
    eq.clear_spectrum();
    // Should not crash
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
