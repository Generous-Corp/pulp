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
#include <pulp/view/theme_editor.hpp>

#include <algorithm>
#include <string_view>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using Catch::Matchers::WithinAbs;

namespace {

bool has_text(const RecordingCanvas& canvas, std::string_view text) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const DrawCommand& command) {
                           return command.type == DrawCommand::Type::fill_text &&
                                  command.text == text;
                       });
}

bool has_fill_color(const RecordingCanvas& canvas, Color color) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const DrawCommand& command) {
                           return command.type == DrawCommand::Type::set_fill_color &&
                                  command.color == color;
                       });
}

} // namespace

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

TEST_CASE("EqCurveView paint covers disabled grid and disabled band handles",
          "[view][eq_curve][coverage][issue-652]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 240, 120});
    eq.set_show_grid(false);
    eq.add_band({500.0f, 12.0f, 1.0f, EqCurveView::FilterType::peak, false});

    pulp::canvas::RecordingCanvas canvas;
    eq.paint(canvas);

    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_circle) == 0);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::stroke_circle) == 0);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) > 0);
}

TEST_CASE("EqCurveView set_band updates valid indices",
          "[view][eq_curve][coverage][phase3]") {
    EqCurveView eq;
    eq.add_band({500.0f, -3.0f, 0.8f, EqCurveView::FilterType::peak, true});

    EqCurveView::Band replacement{2000.0f, 4.0f, 1.5f, EqCurveView::FilterType::notch, true};
    eq.set_band(0, replacement);

    REQUIRE(eq.band_count() == 1);
    REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(2000.0f, 0.001f));
    REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(4.0f, 0.001f));
    REQUIRE_THAT(eq.bands()[0].q, WithinAbs(1.5f, 0.001f));
    REQUIRE(eq.bands()[0].type == EqCurveView::FilterType::notch);
}

TEST_CASE("EqCurveView paint covers grid spectrum and enabled handles",
          "[view][eq_curve][coverage][phase3]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 240, 120});
    const float spectrum[] = {-18.0f, -12.0f, -24.0f, -6.0f};
    eq.set_spectrum(spectrum, sizeof(spectrum) / sizeof(spectrum[0]));
    eq.add_band({1000.0f, 6.0f, 1.0f, EqCurveView::FilterType::peak, true,
                 Color::rgba8(240, 20, 20)});
    eq.set_selected_band(0);

    RecordingCanvas canvas;
    eq.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) > 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) > 200);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_circle) == 1);
    REQUIRE(has_fill_color(canvas, Color::rgba8(240, 20, 20)));
}

TEST_CASE("EqCurveView forwards rich mouse events to the base pointer hook",
          "[view][eq_curve][coverage][phase3]") {
    EqCurveView eq;
    int forwarded = 0;
    eq.on_pointer_event = [&](const MouseEvent& event) {
        ++forwarded;
        REQUIRE(event.pointer_id == 9);
    };

    eq.on_mouse_event(MouseEvent{{8, 9}, {8, 9}, MouseButton::left, 0, 9, 1, true});

    REQUIRE(forwarded == 1);
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

TEST_CASE("MidiKeyboard vertical drag releases previous notes and misses",
          "[view][midi_keyboard][issue-493]") {
    MidiKeyboard kb;
    kb.set_range(60, 64);
    kb.set_orientation(MidiKeyboard::Orientation::vertical);
    kb.set_bounds({0, 0, 80, 300});

    std::vector<int> note_ons;
    std::vector<int> note_offs;
    kb.on_note_on = [&](int note, float velocity) {
        REQUIRE_THAT(velocity, WithinAbs(0.8, 0.001));
        note_ons.push_back(note);
    };
    kb.on_note_off = [&](int note) { note_offs.push_back(note); };

    kb.on_mouse_down({10, 20});
    REQUIRE(note_ons.size() == 1);
    REQUIRE(note_ons[0] == 60);
    REQUIRE(kb.is_note_on(60));

    kb.on_mouse_drag({10, 110});
    REQUIRE(note_offs.size() == 1);
    REQUIRE(note_offs[0] == 60);
    REQUIRE(note_ons.size() == 2);
    REQUIRE(note_ons[1] == 61);
    REQUIRE_FALSE(kb.is_note_on(60));
    REQUIRE(kb.is_note_on(61));

    kb.on_mouse_drag({90, 310});
    REQUIRE(note_offs.size() == 2);
    REQUIRE(note_offs[1] == 61);
    REQUIRE_FALSE(kb.is_note_on(61));

    kb.on_mouse_up({90, 310});
    REQUIRE(note_offs.size() == 2);
}

TEST_CASE("MidiKeyboard drag releases old notes and supports vertical range",
          "[view][midi_keyboard][coverage][issue-652]") {
    MidiKeyboard kb;
    kb.set_range(80, 60);
    REQUIRE(kb.first_note() == 80);
    REQUIRE(kb.last_note() == 80);

    kb.set_range(60, 72);
    kb.set_orientation(MidiKeyboard::Orientation::vertical);
    kb.set_bounds({0, 0, 100, 130});

    std::vector<int> note_ons;
    std::vector<int> note_offs;
    kb.on_note_on = [&](int note, float velocity) {
        note_ons.push_back(note);
        REQUIRE_THAT(velocity, WithinAbs(0.8f, 0.001f));
    };
    kb.on_note_off = [&](int note) { note_offs.push_back(note); };

    kb.on_mouse_down({10, 12});
    REQUIRE(note_ons == std::vector<int>{61});
    REQUIRE(kb.is_note_on(61));

    kb.on_mouse_drag({10, 120});
    REQUIRE(note_offs == std::vector<int>{61});
    REQUIRE_FALSE(kb.is_note_on(61));
    REQUIRE_FALSE(note_ons.empty());
    REQUIRE(kb.is_note_on(note_ons.back()));

    kb.on_mouse_up({10, 120});
    REQUIRE_FALSE(kb.is_note_on(note_ons.back()));
    REQUIRE(note_offs.back() == note_ons.back());
}

TEST_CASE("MidiKeyboard paint emits note names and active highlight color",
          "[view][midi_keyboard][issue-493]") {
    MidiKeyboard kb;
    kb.set_range(60, 64);
    kb.set_bounds({0, 0, 300, 80});
    kb.set_show_note_names(true);
    kb.set_highlight_color(Color::rgba8(255, 0, 0));
    kb.note_on(60);

    RecordingCanvas canvas;
    kb.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 5);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_rounded_rect) == 3);
    REQUIRE(has_text(canvas, "C4"));
    REQUIRE(has_fill_color(canvas, Color::rgba8(255, 0, 0)));
}

TEST_CASE("MidiKeyboard forwards rich mouse events to the base pointer hook",
          "[view][midi_keyboard][coverage][phase3]") {
    MidiKeyboard kb;
    int forwarded = 0;
    Point last{};
    kb.on_pointer_event = [&](const MouseEvent& event) {
        ++forwarded;
        last = event.position;
    };

    kb.on_mouse_event(MouseEvent{{12, 34}, {12, 34}, MouseButton::left, 0, 7, 1, true});

    REQUIRE(forwarded == 1);
    REQUIRE(last.x == 12.0f);
    REQUIRE(last.y == 34.0f);
}

TEST_CASE("MidiKeyboard overlapping black keys win hit testing",
          "[view][midi_keyboard][coverage][phase3]") {
    MidiKeyboard kb;
    kb.set_range(60, 61);
    kb.set_bounds({0, 0, 100, 80});
    std::vector<int> notes;
    kb.on_note_on = [&](int note, float) { notes.push_back(note); };

    kb.on_mouse_down({80, 20});

    REQUIRE(notes == std::vector<int>{61});
    REQUIRE(kb.is_note_on(61));
    REQUIRE_FALSE(kb.is_note_on(60));
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

TEST_CASE("ColorPicker paint draws configured swatches",
          "[view][color_picker][coverage][phase3]") {
    ColorPicker picker;
    picker.set_bounds({0, 0, 200, 260});
    picker.set_swatches({
        Color::rgba8(255, 0, 0),
        Color::rgba8(0, 255, 0),
        Color::rgba8(0, 0, 255),
    });

    RecordingCanvas canvas;
    picker.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) >= 5);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_rounded_rect) >= 4);
    REQUIRE(has_fill_color(canvas, Color::rgba8(255, 0, 0)));
    REQUIRE(has_fill_color(canvas, Color::rgba8(0, 255, 0)));
    REQUIRE(has_fill_color(canvas, Color::rgba8(0, 0, 255)));
}

TEST_CASE("ColorPicker forwards rich mouse events to the base pointer hook",
          "[view][color_picker][coverage][phase3]") {
    ColorPicker picker;
    int forwarded = 0;
    picker.on_pointer_event = [&](const MouseEvent& event) {
        ++forwarded;
        REQUIRE(event.pointer_id == 3);
        REQUIRE(event.is_down);
    };

    picker.on_mouse_event(MouseEvent{{1, 2}, {1, 2}, MouseButton::left, 0, 3, 1, true});

    REQUIRE(forwarded == 1);
}

TEST_CASE("ColorPicker hidden alpha bar ignores alpha-region input",
          "[view][color_picker][coverage][phase3]") {
    ColorPicker picker;
    picker.set_bounds({0, 0, 200, 280});
    picker.set_show_alpha(false);
    picker.set_hex("#33669980");
    int changes = 0;
    picker.on_change = [&](Color) { ++changes; };

    picker.on_mouse_down({60, 212});
    picker.on_mouse_drag({176, 212});
    picker.on_mouse_up({176, 212});

    REQUIRE(changes == 0);
    REQUIRE(picker.hex() == "#33669980");
}

TEST_CASE("ColorPicker mode and outside mouse input are stable",
          "[view][color_picker][coverage][issue-652]") {
    ColorPicker picker;
    picker.set_bounds({0, 0, 200, 260});
    picker.set_mode(ColorPicker::Mode::hex_only);
    REQUIRE(picker.mode() == ColorPicker::Mode::hex_only);

    picker.set_color(Color::rgba8(10, 20, 30));
    auto before = picker.hex();
    int changes = 0;
    picker.on_change = [&](Color) { ++changes; };

    picker.on_mouse_down({300, 300});
    picker.on_mouse_drag({4, 4});
    picker.on_mouse_up({4, 4});

    REQUIRE(changes == 0);
    REQUIRE(picker.hex() == before);
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

TEST_CASE("FileDropZone rejects extensionless files when extensions are required",
          "[view][file_drop][coverage][phase3]") {
    FileDropZone zone;
    zone.set_accepted_extensions({".wav"});

    zone.drag_enter({"README"});

    REQUIRE(zone.is_drag_over());
    REQUIRE_FALSE(zone.is_drag_valid());
}

TEST_CASE("FileDropZone paint reflects idle valid and invalid drag states",
          "[view][file_drop][coverage][issue-652]") {
    FileDropZone zone;
    zone.set_bounds({0, 0, 180, 120});
    zone.set_label("Idle");
    zone.set_hover_label("Ready");
    zone.set_accepted_extensions({".wav"});
    auto fill_text_count = [](const pulp::canvas::RecordingCanvas& canvas,
                              const std::string& text) {
        int count = 0;
        for (const auto& command : canvas.commands()) {
            if (command.type == pulp::canvas::DrawCommand::Type::fill_text &&
                command.text == text) {
                ++count;
            }
        }
        return count;
    };

    pulp::canvas::RecordingCanvas idle;
    zone.paint(idle);
    REQUIRE(idle.count(pulp::canvas::DrawCommand::Type::fill_text) == 1);
    REQUIRE(fill_text_count(idle, "Idle") == 1);
    REQUIRE(idle.count(pulp::canvas::DrawCommand::Type::stroke_line) == 3);

    zone.drag_enter({"take.wav"});
    pulp::canvas::RecordingCanvas valid;
    zone.paint(valid);
    REQUIRE(fill_text_count(valid, "Ready") == 1);

    zone.drag_enter({"take.txt"});
    pulp::canvas::RecordingCanvas invalid;
    zone.paint(invalid);
    REQUIRE(fill_text_count(invalid, "Idle") == 1);

    zone.set_icon_style(FileDropZone::IconStyle::none);
    pulp::canvas::RecordingCanvas no_icon;
    zone.paint(no_icon);
    REQUIRE(no_icon.count(pulp::canvas::DrawCommand::Type::stroke_line) == 0);
}

TEST_CASE("FileDropZone invalid or empty drops do not call callback",
          "[view][file_drop][coverage][issue-652]") {
    FileDropZone zone;
    zone.set_accepted_extensions({".wav"});

    int drops = 0;
    zone.on_drop = [&](const std::vector<std::string>&) { ++drops; };

    zone.drag_enter({"notes.txt"});
    REQUIRE(zone.is_drag_over());
    REQUIRE_FALSE(zone.is_drag_valid());

    zone.drop({"notes.txt"});
    zone.drop({});
    REQUIRE(drops == 0);
    REQUIRE_FALSE(zone.is_drag_over());
    REQUIRE_FALSE(zone.is_drag_valid());
}

TEST_CASE("FileDropZone paint covers idle valid invalid and no-icon states",
          "[view][file_drop][issue-493]") {
    FileDropZone zone;
    zone.set_bounds({0, 0, 200, 120});
    zone.set_label("Drop audio");
    zone.set_hover_label("Release audio");
    zone.set_accepted_extensions({".wav"});

    zone.set_icon_style(FileDropZone::IconStyle::none);
    RecordingCanvas idle;
    zone.paint(idle);
    REQUIRE(has_text(idle, "Drop audio"));
    REQUIRE(idle.count(DrawCommand::Type::stroke_line) == 0);

    zone.set_icon_style(FileDropZone::IconStyle::upload);
    zone.drag_enter({"sound.wav"});
    RecordingCanvas valid;
    zone.paint(valid);
    REQUIRE(has_text(valid, "Release audio"));
    REQUIRE(valid.count(DrawCommand::Type::stroke_line) == 3);

    zone.drag_enter({"sound.txt"});
    RecordingCanvas invalid;
    zone.paint(invalid);
    REQUIRE(has_text(invalid, "Drop audio"));
    REQUIRE(invalid.count(DrawCommand::Type::stroke_line) == 3);
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

TEST_CASE("SplitView drag handling clamps to pane minimums and ignores misses",
          "[view][split_view][issue-493]") {
    SplitView split;
    split.set_bounds({0, 0, 400, 200});
    split.set_first(std::make_unique<View>());
    split.set_second(std::make_unique<View>());
    split.set_min_first_size(80.0f);
    split.set_min_second_size(80.0f);
    split.set_split_fraction(0.5f);
    split.layout_children();

    int changes = 0;
    float last_fraction = 0.0f;
    split.on_split_changed = [&](float fraction) {
        ++changes;
        last_fraction = fraction;
    };

    split.on_mouse_drag({10, 100});
    REQUIRE(changes == 0);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.5, 0.001));

    split.on_mouse_down({20, 20});
    split.on_mouse_drag({300, 100});
    REQUIRE(changes == 0);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.5, 0.001));

    split.on_mouse_down({200, 100});
    split.on_mouse_drag({10, 100});
    REQUIRE(changes == 1);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.2, 0.001));
    REQUIRE_THAT(last_fraction, WithinAbs(0.2, 0.001));

    split.on_mouse_drag({390, 100});
    REQUIRE(changes == 2);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.8, 0.001));
    split.on_mouse_up({390, 100});

    split.on_mouse_drag({100, 100});
    REQUIRE(changes == 2);

    split.set_orientation(SplitView::Orientation::vertical);
    split.set_split_fraction(0.5f);
    split.layout_children();
    split.on_mouse_down({200, 100});
    split.on_mouse_drag({200, 190});
    REQUIRE(changes == 3);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.6, 0.001));
}

TEST_CASE("SplitView paint emits horizontal and vertical divider grips",
          "[view][split_view][issue-493]") {
    SplitView split;
    split.set_bounds({0, 0, 300, 180});

    pulp::canvas::RecordingCanvas canvas;
    split.paint(canvas);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_circle) == 3);

    canvas.clear();
    split.set_orientation(SplitView::Orientation::vertical);
    split.paint(canvas);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_circle) == 3);
}

TEST_CASE("SplitView can replace panes and lays out with custom divider width",
          "[view][split_view][coverage][phase3-batch]") {
    SplitView split;
    split.set_bounds({0, 0, 300, 120});
    split.set_split_fraction(0.25f);
    split.set_divider_width(10.0f);

    auto first = std::make_unique<View>();
    auto second = std::make_unique<View>();
    auto* original_first = first.get();
    auto* second_ptr = second.get();
    split.set_first(std::move(first));
    split.set_second(std::move(second));
    split.layout_children();

    REQUIRE(split.first() == original_first);
    REQUIRE(split.second() == second_ptr);
    REQUIRE_THAT(split.divider_width(), WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(original_first->bounds().width, WithinAbs(70.0f, 0.001f));
    REQUIRE_THAT(second_ptr->bounds().x, WithinAbs(80.0f, 0.001f));

    auto replacement = std::make_unique<View>();
    auto* replacement_ptr = replacement.get();
    split.set_first(std::move(replacement));
    split.layout_children();

    REQUIRE(split.first() == replacement_ptr);
    REQUIRE(split.second() == second_ptr);
    REQUIRE_THAT(replacement_ptr->bounds().width, WithinAbs(70.0f, 0.001f));

    split.set_second(nullptr);
    REQUIRE(split.second() == nullptr);
    split.layout_children();
    REQUIRE(split.first() == replacement_ptr);
}

TEST_CASE("SplitView forwards rich mouse events to the base pointer hook",
          "[view][split_view][coverage][phase3]") {
    SplitView split;
    int forwarded = 0;
    split.on_pointer_event = [&](const MouseEvent& event) {
        ++forwarded;
        REQUIRE(event.position.x == 5.0f);
    };

    split.on_mouse_event(MouseEvent{{5, 6}, {5, 6}, MouseButton::left, 0, 0, 1, true});

    REQUIRE(forwarded == 1);
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

TEST_CASE("PropertyList mouse editing toggles writable booleans only",
          "[view][property_list][issue-493]") {
    PropertyList list;
    list.set_bounds({0, 0, 240, 160});
    list.set_properties({
        {"enabled", "Enabled", false, false, "General"},
        {"locked", "Locked", true, true, "General"},
        {"name", "Name", std::string("Osc"), false, "Info"},
    });

    int changes = 0;
    std::string changed_key;
    bool changed_bool = false;
    list.on_change = [&](const std::string& key, PropertyList::PropertyValue value) {
        ++changes;
        changed_key = key;
        if (std::holds_alternative<bool>(value))
            changed_bool = std::get<bool>(value);
    };

    list.on_mouse_down({12, 30});
    auto* enabled = list.find_property("enabled");
    REQUIRE(enabled != nullptr);
    REQUIRE(std::get<bool>(enabled->value));
    REQUIRE(changes == 1);
    REQUIRE(changed_key == "enabled");
    REQUIRE(changed_bool);

    list.on_mouse_down({12, 60});
    auto* locked = list.find_property("locked");
    REQUIRE(locked != nullptr);
    REQUIRE(std::get<bool>(locked->value));
    REQUIRE(changes == 1);

    list.on_mouse_down({12, 110});
    REQUIRE(changes == 1);

    list.on_mouse_down({12, 500});
    REQUIRE(changes == 1);
}

TEST_CASE("PropertyList paints categories and scalar value variants",
          "[view][property_list][issue-493]") {
    PropertyList list;
    list.set_bounds({0, 0, 260, 180});
    list.set_row_height(20.0f);
    list.set_label_width_fraction(0.5f);
    list.set_properties({
        {"name", "", std::string("Osc"), false, "General"},
        {"gain", "Gain", 0.5f, false, "General"},
        {"voices", "Voices", 8, false, "Synth"},
        {"enabled", "Enabled", true, false, "Synth"},
    });

    const auto with_categories = list.intrinsic_height();
    list.set_show_categories(false);
    REQUIRE_THAT(list.intrinsic_height(), WithinAbs(80.0, 0.001));
    REQUIRE(with_categories > list.intrinsic_height());

    pulp::canvas::RecordingCanvas canvas;
    list.paint(canvas);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) == 4);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_text) == 8);

    bool saw_key_label = false;
    bool saw_float_value = false;
    bool saw_int_value = false;
    bool saw_bool_value = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != pulp::canvas::DrawCommand::Type::fill_text)
            continue;
        saw_key_label = saw_key_label || command.text == "name";
        saw_float_value = saw_float_value || command.text == "0.50";
        saw_int_value = saw_int_value || command.text == "8";
        saw_bool_value = saw_bool_value || command.text == "true";
    }
    REQUIRE(saw_key_label);
    REQUIRE(saw_float_value);
    REQUIRE(saw_int_value);
    REQUIRE(saw_bool_value);

    canvas.clear();
    list.set_show_categories(true);
    list.paint(canvas);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_text) == 10);
}

TEST_CASE("PropertyList paints color values and selected row highlight",
          "[view][property_list][coverage][phase3]") {
    PropertyList list;
    list.set_bounds({0, 0, 260, 100});
    list.set_row_height(24.0f);
    list.set_show_categories(false);
    list.set_properties({
        {"accent", "Accent", Color::rgba8(0x33, 0x66, 0x99), true, ""},
        {"enabled", "Enabled", false, false, ""},
    });

    list.on_mouse_down({12, 10});

    RecordingCanvas canvas;
    list.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(has_text(canvas, "#336699"));
    REQUIRE(has_text(canvas, "false"));
    REQUIRE(has_fill_color(canvas, Color::rgba8(0x33, 0x66, 0x99)));
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

// ── ThemeEditor ────────────────────────────────────────────────────────────

TEST_CASE("ThemeEditor covers missing selection empty theme and selected paint",
          "[view][theme-editor][coverage][issue-652]") {
    ThemeEditor editor;
    editor.set_bounds({0, 0, 360, 120});

    editor.select_token("missing");
    REQUIRE(editor.selected_token() == "missing");
    REQUIRE(editor.token_names().empty());

    pulp::canvas::RecordingCanvas empty;
    editor.paint_all(empty);
    REQUIRE(empty.count(pulp::canvas::DrawCommand::Type::fill_text) == 1);

    Theme theme;
    theme.colors["accent.primary"] = Color::rgba8(1, 2, 3);
    theme.colors["bg.primary"] = Color::rgba8(4, 5, 6);
    editor.set_theme(theme);
    editor.select_token("accent.primary");

    pulp::canvas::RecordingCanvas selected;
    editor.paint_all(selected);

    REQUIRE(selected.count(pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(selected.count(pulp::canvas::DrawCommand::Type::stroke_rounded_rect) == 1);
    REQUIRE(selected.count(pulp::canvas::DrawCommand::Type::fill_text) == 3);
    REQUIRE(editor.export_json().find("accent.primary") != std::string::npos);
}

TEST_CASE("ThemeEditor wraps swatches when the row is full",
          "[view][theme-editor][coverage][phase3]") {
    ThemeEditor editor;
    editor.set_bounds({0, 0, 80, 160});

    Theme theme;
    theme.colors["accent.primary"] = Color::rgba8(1, 2, 3);
    theme.colors["accent.secondary"] = Color::rgba8(4, 5, 6);
    theme.colors["bg.primary"] = Color::rgba8(7, 8, 9);
    editor.set_theme(theme);

    RecordingCanvas canvas;
    editor.paint_all(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 3);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 4);
    REQUIRE(has_text(canvas, "Theme Editor"));
}
