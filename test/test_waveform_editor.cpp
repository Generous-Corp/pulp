#include <catch2/catch_test_macros.hpp>
#include <pulp/view/waveform_editor.hpp>
#include <pulp/canvas/canvas.hpp>
#include <vector>
#include <cmath>

using namespace pulp::view;
using namespace pulp::canvas;

static std::vector<float> make_sine(int samples, float freq = 440.0f, float sr = 44100.0f) {
    std::vector<float> data(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        data[static_cast<size_t>(i)] = std::sin(2.0f * 3.14159f * freq * static_cast<float>(i) / sr);
    }
    return data;
}

TEST_CASE("WaveformEditor set audio data", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    REQUIRE(editor.total_samples() == 44100);
    REQUIRE(editor.sample_rate() == 44100.0f);
}

TEST_CASE("WaveformEditor selection", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    REQUIRE_FALSE(editor.has_selection());

    editor.set_selection(1000, 5000);
    REQUIRE(editor.has_selection());
    REQUIRE(editor.selection_start() == 1000);
    REQUIRE(editor.selection_end() == 5000);
    REQUIRE(editor.selection_length() == 4000);
}

TEST_CASE("WaveformEditor clear selection", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    editor.set_selection(100, 200);
    REQUIRE(editor.has_selection());

    editor.clear_selection();
    REQUIRE_FALSE(editor.has_selection());
}

TEST_CASE("WaveformEditor selection callback", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    int cb_start = -1, cb_end = -1;
    editor.on_selection_changed = [&](int s, int e) { cb_start = s; cb_end = e; };

    editor.set_selection(500, 1500);
    REQUIRE(cb_start == 500);
    REQUIRE(cb_end == 1500);
}

TEST_CASE("WaveformEditor clamps selection bounds and reversed ranges", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);

    editor.set_selection(-100, 1200);
    REQUIRE(editor.selection_start() == 0);
    REQUIRE(editor.selection_end() == 1000);
    REQUIRE(editor.selection_length() == 1000);

    editor.set_selection(900, 100);
    REQUIRE(editor.selection_start() == 100);
    REQUIRE(editor.selection_end() == 900);
    REQUIRE(editor.selection_length() == 800);
}

TEST_CASE("WaveformEditor zoom in/out", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    REQUIRE(editor.visible_length() == 44100);

    editor.zoom_in(2.0f);
    REQUIRE(editor.visible_length() < 44100);
    REQUIRE(editor.visible_length() > 0);

    int zoomed_len = editor.visible_length();
    editor.zoom_out(2.0f);
    REQUIRE(editor.visible_length() > zoomed_len);
}

TEST_CASE("WaveformEditor zoom to fit", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    editor.zoom_in(4.0f);
    REQUIRE(editor.visible_length() < 44100);

    editor.zoom_to_fit();
    REQUIRE(editor.visible_length() == 44100);
    REQUIRE(editor.visible_start() == 0);
}

TEST_CASE("WaveformEditor zoom to selection", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    editor.set_selection(10000, 20000);
    editor.zoom_to_selection();
    REQUIRE(editor.visible_start() == 10000);
    REQUIRE(editor.visible_length() == 10000);
}

TEST_CASE("WaveformEditor zoom to selection is a no-op without selection", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);
    editor.set_visible_range(200, 250);

    editor.zoom_to_selection();

    REQUIRE(editor.visible_start() == 200);
    REQUIRE(editor.visible_length() == 250);
}

TEST_CASE("WaveformEditor scroll", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);
    editor.set_visible_range(0, 10000);

    editor.scroll(5000);
    REQUIRE(editor.visible_start() == 5000);
    REQUIRE(editor.visible_length() == 10000);
}

TEST_CASE("WaveformEditor scroll clamped at boundaries", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);
    editor.set_visible_range(0, 10000);

    editor.scroll(-5000); // can't go below 0
    REQUIRE(editor.visible_start() == 0);

    editor.scroll(100000); // clamps to end
    REQUIRE(editor.visible_start() + editor.visible_length() <= 44100);
}

TEST_CASE("WaveformEditor visible range clamps minimum length near end", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);

    editor.set_visible_range(995, 1);
    REQUIRE(editor.visible_length() == 16);
    REQUIRE(editor.visible_start() == 984);

    editor.set_visible_range(-50, 100);
    REQUIRE(editor.visible_start() == 0);
    REQUIRE(editor.visible_length() == 100);
}

TEST_CASE("WaveformEditor playhead", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    editor.set_playhead(22050);
    REQUIRE(editor.playhead() == 22050);
}

TEST_CASE("WaveformEditor playhead clamps to audio bounds", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);

    editor.set_playhead(-50);
    REQUIRE(editor.playhead() == 0);

    editor.set_playhead(5000);
    REQUIRE(editor.playhead() == 1000);
}

TEST_CASE("WaveformEditor regions", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    REQUIRE(editor.regions().empty());

    editor.add_region({1000, 5000, "Loop A"});
    editor.add_region({20000, 30000, "Loop B"});
    REQUIRE(editor.regions().size() == 2);

    editor.clear_regions();
    REQUIRE(editor.regions().empty());
}

TEST_CASE("WaveformEditor paint produces draw commands", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);
    editor.set_bounds({0, 0, 400, 150});

    RecordingCanvas canvas;
    editor.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) > 0);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) > 0);
}

TEST_CASE("WaveformEditor paint records regions selection and playhead", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(400);
    editor.set_audio_data(data.data(), 400, 44100.0f);
    editor.set_bounds({0, 0, 80, 40});
    editor.set_selection(40, 120);
    editor.add_region({160, 240, "Loop"});
    editor.set_playhead(200);

    RecordingCanvas canvas;
    editor.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 3);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) >= 82);
}

TEST_CASE("WaveformEditor key Cmd+A selects all", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    KeyEvent e;
    e.key = KeyCode::a;
#ifdef __APPLE__
    e.modifiers = kModCmd;
#else
    e.modifiers = kModCtrl;
#endif
    e.is_down = true;
    REQUIRE(editor.on_key_event(e));
    REQUIRE(editor.selection_start() == 0);
    REQUIRE(editor.selection_end() == 44100);
}

TEST_CASE("WaveformEditor key scrolls and release events are ignored", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);
    editor.set_visible_range(200, 200);

    KeyEvent release;
    release.key = KeyCode::right;
    release.is_down = false;
    REQUIRE_FALSE(editor.on_key_event(release));
    REQUIRE(editor.visible_start() == 200);

    KeyEvent right;
    right.key = KeyCode::right;
    REQUIRE(editor.on_key_event(right));
    REQUIRE(editor.visible_start() == 220);

    KeyEvent left;
    left.key = KeyCode::left;
    REQUIRE(editor.on_key_event(left));
    REQUIRE(editor.visible_start() == 200);
}

TEST_CASE("WaveformEditor mouse click and shift extend selection", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);
    editor.set_bounds({0, 0, 100, 40});

    MouseEvent down;
    down.is_down = true;
    down.position = {25, 10};
    editor.on_mouse_event(down);
    REQUIRE(editor.selection_start() == 250);
    REQUIRE(editor.selection_end() == 250);
    REQUIRE_FALSE(editor.has_selection());

    editor.set_selection(100, 200);
    int cb_start = -1;
    int cb_end = -1;
    editor.on_selection_changed = [&](int start, int end) {
        cb_start = start;
        cb_end = end;
    };

    MouseEvent shift_down;
    shift_down.is_down = true;
    shift_down.modifiers = kModShift;
    shift_down.position = {50, 10};
    editor.on_mouse_event(shift_down);

    REQUIRE(editor.selection_start() == 100);
    REQUIRE(editor.selection_end() == 500);
    REQUIRE(cb_start == 100);
    REQUIRE(cb_end == 500);
}
