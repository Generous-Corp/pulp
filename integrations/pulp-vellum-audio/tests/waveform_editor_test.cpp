#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <pulp/vellum_audio/audio_display_provider.hpp>
#include <pulp/vellum_audio/waveform_editor.hpp>
#include <pulp/vellum_audio/waveform_editor_primitives.hpp>
#include <vector>
#include <vellum/canvas/canvas.hpp>
#include <vellum/canvas/recording_canvas.hpp>

using namespace pulp::vellum_audio;
using namespace vellum::view;
using namespace vellum::canvas;

struct TestApprox {
    float expected;
    explicit TestApprox(float value) : expected(value) {}
    friend bool operator==(float actual, const TestApprox& rhs) {
        return std::fabs(actual - rhs.expected) <= 1.0e-4f;
    }
    friend bool operator==(const TestApprox& lhs, float actual) {
        return actual == lhs;
    }
};

static std::vector<float> make_sine(int samples, float freq = 440.0f, float sr = 44100.0f) {
    std::vector<float> data(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        data[static_cast<size_t>(i)] =
            std::sin(2.0f * 3.14159f * freq * static_cast<float>(i) / sr);
    }
    return data;
}

TEST_CASE("waveform editor snapshots audio display provider data",
          "[pulp-vellum-audio][waveform]") {
    pulp::vellum_audio::AudioDisplayProvider provider({
        {.name = "waveform", .shape = ::vellum::value::ChannelShape::vector},
    });
    const auto channel = provider.find("waveform", ::vellum::value::ChannelShape::vector);
    REQUIRE(channel);
    const std::array samples{-.5f, 0.0f, .75f, 1.0f};
    provider.publish_scope(*channel, samples, {.sample_rate = 48000.0, .sequence = 4});

    WaveformEditor editor;
    editor.set_audio_provider(&provider, *channel);
    CHECK(editor.total_samples() == 4);
    CHECK(editor.sample_rate() == 48000.0f);
}

TEST_CASE("waveform_01", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    CHECK(editor.total_samples() == 44100);
    CHECK(editor.sample_rate() == 44100.0f);
}

TEST_CASE("waveform_02", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    editor.set_audio_data(nullptr, 0, 48000.0f);
    editor.set_visible_range(10, 10);

    CHECK(editor.total_samples() == 0);
    CHECK(editor.sample_rate() == 48000.0f);
    CHECK(editor.visible_start() == 0);
    CHECK(editor.visible_length() == 0);
    CHECK(editor.playhead() == 0);
}

TEST_CASE("waveform_03", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    CHECK(!(editor.has_selection()));

    editor.set_selection(1000, 5000);
    CHECK(editor.has_selection());
    CHECK(editor.selection_start() == 1000);
    CHECK(editor.selection_end() == 5000);
    CHECK(editor.selection_length() == 4000);
}

TEST_CASE("waveform_04", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    editor.set_selection(100, 200);
    CHECK(editor.has_selection());

    editor.clear_selection();
    CHECK(!(editor.has_selection()));
}

TEST_CASE("waveform_05", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    int cb_start = -1, cb_end = -1;
    editor.on_selection_changed = [&](int s, int e) {
        cb_start = s;
        cb_end = e;
    };

    editor.set_selection(500, 1500);
    CHECK(cb_start == 500);
    CHECK(cb_end == 1500);

    editor.set_selection(900, 100);
    CHECK(cb_start == 100);
    CHECK(cb_end == 900);

    editor.clear_selection();
    CHECK(cb_start == 0);
    CHECK(cb_end == 0);
}

TEST_CASE("waveform_06", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);

    editor.set_selection(-100, 1200);
    CHECK(editor.selection_start() == 0);
    CHECK(editor.selection_end() == 1000);
    CHECK(editor.selection_length() == 1000);

    editor.set_selection(900, 100);
    CHECK(editor.selection_start() == 100);
    CHECK(editor.selection_end() == 900);
    CHECK(editor.selection_length() == 800);
}

TEST_CASE("waveform_07", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    CHECK(editor.visible_length() == 44100);

    editor.zoom_in(2.0f);
    CHECK(editor.visible_length() < 44100);
    CHECK(editor.visible_length() > 0);

    int zoomed_len = editor.visible_length();
    editor.zoom_out(2.0f);
    CHECK(editor.visible_length() > zoomed_len);
}

TEST_CASE("waveform_08", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    editor.zoom_in(4.0f);
    CHECK(editor.visible_length() < 44100);

    editor.zoom_to_fit();
    CHECK(editor.visible_length() == 44100);
    CHECK(editor.visible_start() == 0);
}

TEST_CASE("waveform_09", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    editor.set_selection(10000, 20000);
    editor.zoom_to_selection();
    CHECK(editor.visible_start() == 10000);
    CHECK(editor.visible_length() == 10000);
}

TEST_CASE("waveform_10", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);
    editor.set_visible_range(200, 250);

    editor.zoom_to_selection();

    CHECK(editor.visible_start() == 200);
    CHECK(editor.visible_length() == 250);
}

TEST_CASE("waveform_11", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);
    editor.set_visible_range(0, 10000);

    editor.scroll(5000);
    CHECK(editor.visible_start() == 5000);
    CHECK(editor.visible_length() == 10000);
}

TEST_CASE("waveform_12", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);
    editor.set_visible_range(0, 10000);

    editor.scroll(-5000); // can't go below 0
    CHECK(editor.visible_start() == 0);

    editor.scroll(100000); // clamps to end
    CHECK(editor.visible_start() + editor.visible_length() <= 44100);
}

TEST_CASE("waveform_13", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);

    editor.set_visible_range(995, 1);
    CHECK(editor.visible_length() == 16);
    CHECK(editor.visible_start() == 984);

    editor.set_visible_range(-50, 100);
    CHECK(editor.visible_start() == 0);
    CHECK(editor.visible_length() == 100);
}

TEST_CASE("waveform_14", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto long_data = make_sine(1000);
    editor.set_audio_data(long_data.data(), 1000, 44100.0f);
    editor.set_selection(700, 950);
    editor.set_playhead(900);
    editor.add_region({800, 1000, "Tail"});

    auto short_data = make_sine(400);
    editor.set_audio_data(short_data.data(), 400, 44100.0f);

    CHECK(editor.selection_start() == 400);
    CHECK(editor.selection_end() == 400);
    CHECK(!(editor.has_selection()));
    CHECK(editor.playhead() == 400);
    CHECK(editor.regions().front().start_sample == 400);
    CHECK(editor.regions().front().end_sample == 400);
}

TEST_CASE("waveform_15", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    editor.set_playhead(22050);
    CHECK(editor.playhead() == 22050);
}

TEST_CASE("waveform_16", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);

    editor.set_playhead(-50);
    CHECK(editor.playhead() == 0);

    editor.set_playhead(5000);
    CHECK(editor.playhead() == 1000);
}

TEST_CASE("waveform_17", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(44100);
    editor.set_audio_data(data.data(), 44100, 44100.0f);

    CHECK(editor.regions().empty());

    editor.add_region({1000, 5000, "Loop A"});
    editor.add_region({20000, 30000, "Loop B"});
    CHECK(editor.regions().size() == 2);

    editor.clear_regions();
    CHECK(editor.regions().empty());
}

TEST_CASE("waveform_18", "[pulp-vellum-audio][waveform]") {
    WaveformRegion forward{100, 180, "Forward"};
    CHECK(forward.length() == 80);

    WaveformRegion reversed{220, 150, "Reversed"};
    CHECK(reversed.length() == -70);
}

TEST_CASE("waveform_19", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    editor.set_bounds({0, 0, 400, 150});

    RecordingCanvas canvas;
    editor.paint(canvas);

    CHECK(canvas.command_count() == 0);
}

TEST_CASE("waveform_20", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);
    editor.set_bounds({0, 0, 400, 150});

    RecordingCanvas canvas;
    editor.paint(canvas);

    CHECK(canvas.count(DrawCommand::Type::fill_rect) > 0);
    CHECK(canvas.count(DrawCommand::Type::stroke_line) > 0);
}

TEST_CASE("waveform_21", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(400);
    editor.set_audio_data(data.data(), 400, 44100.0f);
    editor.set_bounds({0, 0, 80, 40});
    editor.set_selection(40, 120);
    editor.add_region({160, 240, "Loop"});
    editor.set_playhead(200);

    RecordingCanvas canvas;
    editor.paint(canvas);

    CHECK(canvas.count(DrawCommand::Type::fill_rect) >= 3);
    CHECK(canvas.count(DrawCommand::Type::stroke_line) >= 82);
}

TEST_CASE("waveform_22", "[pulp-vellum-audio][waveform]") {
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
    CHECK(editor.on_key_event(e));
    CHECK(editor.selection_start() == 0);
    CHECK(editor.selection_end() == 44100);
}

TEST_CASE("waveform_23", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);
    editor.set_visible_range(200, 200);

    KeyEvent release;
    release.key = KeyCode::right;
    release.is_down = false;
    CHECK(!(editor.on_key_event(release)));
    CHECK(editor.visible_start() == 200);

    KeyEvent right;
    right.key = KeyCode::right;
    CHECK(editor.on_key_event(right));
    CHECK(editor.visible_start() == 220);

    KeyEvent left;
    left.key = KeyCode::left;
    CHECK(editor.on_key_event(left));
    CHECK(editor.visible_start() == 200);
}

TEST_CASE("waveform_24", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);
    editor.set_visible_range(250, 250);

    KeyEvent zero;
    zero.key = KeyCode::num0;
#ifdef __APPLE__
    zero.modifiers = kModCmd;
#else
    zero.modifiers = kModCtrl;
#endif
    zero.is_down = true;
    CHECK(editor.on_key_event(zero));
    CHECK(editor.visible_start() == 0);
    CHECK(editor.visible_length() == 1000);
}

TEST_CASE("waveform_25", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);
    editor.set_bounds({0, 0, 100, 40});

    MouseEvent down;
    down.is_down = true;
    down.position = {25, 10};
    editor.on_mouse_event(down);
    CHECK(editor.selection_start() == 250);
    CHECK(editor.selection_end() == 250);
    CHECK(!(editor.has_selection()));

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

    CHECK(editor.selection_start() == 100);
    CHECK(editor.selection_end() == 500);
    CHECK(cb_start == 100);
    CHECK(cb_end == 500);
}

TEST_CASE("waveform_26", "[pulp-vellum-audio][waveform]") {
    WaveformEditor editor;
    auto data = make_sine(1000);
    editor.set_audio_data(data.data(), 1000, 44100.0f);
    editor.set_bounds({0, 0, 100, 40});

    MouseEvent down;
    down.phase = MousePhase::press;
    down.is_down = true;
    down.position = {30, 10};
    editor.on_mouse_event(down);

    MouseEvent drag;
    drag.phase = MousePhase::drag;
    drag.position = {70, 10};
    editor.on_mouse_event(drag);

    MouseEvent up;
    up.phase = MousePhase::release;
    up.position = {70, 10};
    editor.on_mouse_event(up);

    CHECK(editor.selection_start() == 300);
    CHECK(editor.selection_end() == 700);
    CHECK(editor.selection_length() == 400);
}

TEST_CASE("waveform_27", "[pulp-vellum-audio][waveform]") {
    WaveformViewport empty;
    empty.set_bounds({10, 0, 100, 40});
    empty.set_total_samples(0);
    empty.set_visible_range(20, 20);

    CHECK(empty.total_samples == 0);
    CHECK(empty.visible_start == 0);
    CHECK(empty.visible_length == 0);
    CHECK(empty.x_to_sample(60) == 0);
    CHECK(empty.sample_to_x(123) == TestApprox(10.0f));

    WaveformViewport viewport;
    viewport.set_bounds({10, 0, 100, 40});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(995, 1);

    CHECK(viewport.visible_start == 984);
    CHECK(viewport.visible_length == 16);
    CHECK(viewport.visible_end() == 1000);
    CHECK(viewport.sample_to_x(984) == TestApprox(10.0f));
    CHECK(viewport.sample_to_x(1000) == TestApprox(110.0f));
    CHECK(viewport.x_to_sample(-100.0f) == 984);
    CHECK(viewport.x_to_sample(500.0f) == 1000);
    CHECK(viewport.samples_per_pixel() == TestApprox(0.16));
}

TEST_CASE("waveform_28", "[pulp-vellum-audio][waveform]") {
    WaveformViewport viewport;
    viewport.set_bounds({0, 0, 10, 40});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(100, 100);

    auto plan = build_waveform_render_plan(viewport, 5);

    CHECK(plan.spans.size() == 5);
    CHECK(plan.spans.front().sample_start == 100);
    CHECK(plan.spans.front().sample_end == 120);
    CHECK(plan.spans.back().sample_start == 180);
    CHECK(plan.spans.back().sample_end == 200);
    CHECK(plan.spans.back().x == TestApprox(8.0f));
    CHECK(plan.spans.back().width == TestApprox(2.0f));
}

TEST_CASE("waveform_29", "[pulp-vellum-audio][waveform]") {
    WaveformViewport viewport;
    viewport.set_bounds({0, 0, 100, 40});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(0, 1000);

    WaveformHandleModel model;
    model.set_total_samples(1000);
    model.set_selection(100, 300);
    model.set_loop(200, 800);
    model.set_fade_in(1200);
    model.set_slice_markers({500, 500, -10, 999});
    model.set_playhead(700);

    auto handles = model.handles();
    CHECK(handles.size() == 9);
    int visited = 0;
    model.for_each_handle([&](const WaveformHandle&) { ++visited; });
    CHECK(visited == 9);
    CHECK(model.slice_markers.size() == 3);
    CHECK(model.slice_markers[0] == 0);
    CHECK(model.slice_markers[1] == 500);
    CHECK(model.slice_markers[2] == 999);

    auto selection_hit = hit_test_waveform_handles(viewport, model, 30.0f, 0.25f);
    CHECK(selection_hit.kind == WaveformHandleKind::selection_end);
    CHECK(selection_hit.sample == 300);

    auto slice_hit = hit_test_waveform_handles(viewport, model, 50.0f, 0.25f);
    CHECK(slice_hit.kind == WaveformHandleKind::slice_marker);
    CHECK(slice_hit.id == 1);
    CHECK(slice_hit.sample == 500);

    auto miss = hit_test_waveform_handles(viewport, model, 55.0f, 0.25f);
    CHECK(!(miss));
    CHECK(miss.sample == 550);
}

TEST_CASE("waveform_30", "[pulp-vellum-audio][waveform]") {
    WaveformSnapSettings settings;
    settings.tolerance_samples = 8;
    settings.candidates = {250};

    auto candidate = resolve_waveform_snap(246, 1000, settings);
    CHECK(candidate.snapped);
    CHECK(candidate.source == WaveformSnapSource::candidate);
    CHECK(candidate.sample == 250);

    auto outside = resolve_waveform_snap(260, 1000, settings);
    CHECK(!(outside.snapped));
    CHECK(outside.sample == 260);

    settings.grid_interval_samples = 100;
    auto grid = resolve_waveform_snap(296, 1000, settings);
    CHECK(grid.snapped);
    CHECK(grid.source == WaveformSnapSource::grid);
    CHECK(grid.sample == 300);

    auto bounds = resolve_waveform_snap(3, 1000, settings);
    CHECK(bounds.snapped);
    CHECK(bounds.source == WaveformSnapSource::bounds);
    CHECK(bounds.sample == 0);
}

TEST_CASE("waveform_31", "[pulp-vellum-audio][waveform]") {
    WaveformViewport viewport;
    viewport.set_bounds({0, 0, 80, 40});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(100, 400);

    auto visible = build_waveform_playhead_overlay(viewport, 200);
    CHECK(visible.visible);
    CHECK(visible.sample == 200);
    CHECK(visible.x == TestApprox(20.0f));

    auto hidden = build_waveform_playhead_overlay(viewport, 600);
    CHECK(!(hidden.visible));
    CHECK(hidden.sample == 600);
    CHECK(hidden.x == TestApprox(0.0f));
}

TEST_CASE("waveform_32", "[pulp-vellum-audio][waveform]") {
    WaveformViewport viewport;
    viewport.set_bounds({0, 0, 100, 40});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(100, 400);

    CHECK(viewport.visible_end() == 500);
    CHECK(!(viewport.sample_visible(500)));
    CHECK(viewport.sample_point_visible(500));

    WaveformHandleModel model;
    model.set_total_samples(1000);
    model.set_loop(200, 500);

    auto hit = hit_test_waveform_handles(viewport, model, 100.0f, 0.1f);
    CHECK(hit.kind == WaveformHandleKind::loop_end);
    CHECK(hit.sample == 500);

    auto playhead = build_waveform_playhead_overlay(viewport, 500);
    CHECK(playhead.visible);
    CHECK(playhead.x == TestApprox(100.0f));

    viewport.set_visible_range(984, 16);
    auto final_playhead = build_waveform_playhead_overlay(viewport, 1000);
    CHECK(final_playhead.visible);
    CHECK(final_playhead.x == TestApprox(100.0f));
}

TEST_CASE("waveform_33", "[pulp-vellum-audio][waveform]") {
    WaveformEditorTransaction tx;
    tx.begin(WaveformEditorOperationKind::create_selection, 300);

    auto update = tx.update(100);
    CHECK(update.active);
    CHECK(update.selection.start == 100);
    CHECK(update.selection.end == 300);

    auto commit = tx.commit(500);
    CHECK(commit.committed);
    CHECK(commit.selection.start == 300);
    CHECK(commit.selection.end == 500);
    CHECK(!(tx.active()));

    tx.begin(WaveformEditorOperationKind::drag_selection_start, 0, {100, 400});
    auto dragged = tx.update(350);
    CHECK(dragged.selection.start == 350);
    CHECK(dragged.selection.end == 400);

    auto cancelled = tx.cancel();
    CHECK(cancelled.cancelled);
    CHECK(cancelled.selection.start == 100);
    CHECK(cancelled.selection.end == 400);
    CHECK(!(tx.active()));

    tx.begin(WaveformEditorOperationKind::move_playhead, 0, {}, 120);
    auto moved = tx.commit(480);
    CHECK(moved.playhead_sample == 480);
}

TEST_CASE("waveform_34", "[pulp-vellum-audio][waveform]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(100, 400);
    surface.set_selection(120, 180);
    surface.set_trim(50, 900);
    surface.set_loop(200, 800);
    surface.set_fade_in(150);
    surface.set_fade_out(850);
    surface.set_slice_markers({250, 250, 490});
    surface.set_playhead(200);

    const auto snapshot = surface.snapshot();
    CHECK(snapshot.viewport.visible_start == 100);
    CHECK(snapshot.viewport.visible_length == 400);
    CHECK(snapshot.handles.has_trim);
    CHECK(snapshot.handles.has_loop);
    CHECK(snapshot.handles.slice_markers.size() == 2);
    CHECK(snapshot.playhead.visible);
    CHECK(snapshot.playhead.x == TestApprox(25.0f));

    const auto plan = surface.render_plan(4);
    CHECK(plan.spans.size() == 4);
    CHECK(plan.spans.front().sample_start == 100);
    CHECK(plan.spans.front().sample_end == 200);
    CHECK(plan.spans.back().sample_start == 400);
    CHECK(plan.spans.back().sample_end == 500);

    const auto hit = surface.hit_test(25.0f, 0.1f);
    CHECK(hit.kind == WaveformHandleKind::loop_start);
    CHECK(hit.sample == 200);
}

TEST_CASE("waveform_35", "[pulp-vellum-audio][waveform]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);

    WaveformSnapSettings snap;
    snap.grid_interval_samples = 50;
    snap.tolerance_samples = 20;
    surface.set_snap_settings(snap);

    surface.set_trim(100, 900);
    CHECK(surface.begin_handle_edit(WaveformHandleKind::trim_start));
    auto trim_update = surface.update_edit(138);
    CHECK(trim_update.snap.snapped);
    CHECK(trim_update.snap.sample == 150);
    CHECK(surface.handles().trim.start == 150);
    auto trim_commit = surface.commit_edit(180);
    CHECK(trim_commit.committed);
    CHECK(surface.handles().trim.start == 200);
    CHECK(surface.handles().trim.end == 900);

    surface.set_loop(200, 800);
    CHECK(surface.begin_handle_edit(WaveformHandleKind::loop_end));
    auto loop_commit = surface.commit_edit(752);
    CHECK(loop_commit.snap.sample == 750);
    CHECK(surface.handles().loop.start == 200);
    CHECK(surface.handles().loop.end == 750);

    surface.set_fade_in(100);
    CHECK(surface.begin_handle_edit(WaveformHandleKind::fade_in));
    const auto fade_in_commit = surface.commit_edit(138);
    CHECK(fade_in_commit.committed);
    CHECK(surface.handles().fade_in_end == 150);

    surface.set_fade_out(900);
    CHECK(surface.begin_handle_edit(WaveformHandleKind::fade_out));
    const auto fade_out_commit = surface.commit_edit(861);
    CHECK(fade_out_commit.committed);
    CHECK(surface.handles().fade_out_start == 850);

    surface.set_slice_markers({200, 600});
    CHECK(surface.begin_handle_edit(WaveformHandleKind::slice_marker, 1));
    const auto slice_commit = surface.commit_edit(638);
    CHECK(slice_commit.committed);
    CHECK(surface.handles().slice_markers.size() == 2);
    CHECK(surface.handles().slice_markers[0] == 200);
    CHECK(surface.handles().slice_markers[1] == 650);

    surface.set_playhead(0);
    CHECK(surface.begin_handle_edit(WaveformHandleKind::playhead));
    const auto playhead_commit = surface.commit_edit(901);
    CHECK(playhead_commit.committed);
    CHECK(surface.handles().has_playhead);
    CHECK(surface.handles().playhead == 900);

    CHECK(surface.begin_selection_edit(123));
    const auto selection_commit = surface.commit_edit(288);
    CHECK(selection_commit.committed);
    CHECK(surface.handles().selection.start == 123);
    CHECK(surface.handles().selection.end == 300);

    CHECK(surface.begin_selection_edit(250));
    const auto empty_selection = surface.commit_edit(250);
    CHECK(empty_selection.committed);
    CHECK(!(surface.handles().has_selection));
}

TEST_CASE("waveform_36", "[pulp-vellum-audio][waveform]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);

    surface.set_selection(250, 250);
    CHECK(!(surface.handles().has_selection));
    CHECK(!(surface.snapshot().handles.has_selection));

    surface.set_selection(-200, -100);
    CHECK(!(surface.handles().has_selection));
    surface.set_selection(1200, 1400);
    CHECK(!(surface.handles().has_selection));

    surface.set_selection(700, 950);
    CHECK(surface.handles().has_selection);
    surface.set_total_samples(400);
    CHECK(!(surface.handles().has_selection));
    CHECK(!(surface.snapshot().handles.has_selection));
}

TEST_CASE("waveform_37", "[pulp-vellum-audio][waveform]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);
    surface.set_selection(700, 950);

    CHECK(surface.begin_handle_edit(WaveformHandleKind::selection_start));
    surface.set_total_samples(400);
    CHECK(!(surface.handles().has_selection));

    const auto update = surface.update_edit(300);
    CHECK(update.active);
    CHECK(!(update.handles.has_selection));
    CHECK(!(surface.handles().has_selection));

    const auto commit = surface.commit_edit(300);
    CHECK(commit.committed);
    CHECK(!(surface.handles().has_selection));
}

TEST_CASE("waveform_38", "[pulp-vellum-audio][waveform]") {
    WaveformEditorSurface empty;
    CHECK(!(empty.begin_selection_edit(10)));
    CHECK(!(empty.begin_handle_edit(WaveformHandleKind::playhead)));

    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);
    surface.set_loop(200, 700);
    surface.set_slice_markers({100});

    CHECK(!(surface.begin_handle_edit(WaveformHandleKind::playhead)));
    CHECK(!(surface.begin_handle_edit(WaveformHandleKind::trim_start)));
    CHECK(!(surface.begin_handle_edit(WaveformHandleKind::slice_marker, 3)));

    CHECK(surface.begin_handle_edit(WaveformHandleKind::loop_start));
    const auto loop_update = surface.update_edit(450);
    CHECK(loop_update.active);
    CHECK(surface.handles().loop.start == 450);
    CHECK(!(surface.begin_handle_edit(WaveformHandleKind::loop_end)));
    CHECK(!(surface.begin_selection_edit(300)));

    auto cancelled = surface.cancel_edit();
    CHECK(cancelled.cancelled);
    CHECK(!(surface.edit_active()));
    CHECK(surface.handles().loop.start == 200);
    CHECK(surface.handles().loop.end == 700);

    CHECK(surface.begin_handle_edit(WaveformHandleKind::loop_end, 0));
    const auto second_cancel = surface.cancel_edit();
    CHECK(second_cancel.cancelled);
    CHECK(!(surface.begin_handle_edit(WaveformHandleKind::loop_start, 999)));
}

TEST_CASE("waveform_39", "[pulp-vellum-audio][waveform]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);
    surface.set_loop(200, 800);
    surface.set_playhead(100);

    CHECK(surface.begin_handle_edit(WaveformHandleKind::loop_start));
    surface.set_playhead(500);
    const auto update = surface.update_edit(300);
    CHECK(update.active);
    CHECK(surface.handles().loop.start == 300);
    CHECK(surface.handles().loop.end == 800);
    CHECK(surface.handles().playhead == 500);

    const auto cancel = surface.cancel_edit();
    CHECK(cancel.cancelled);
    CHECK(surface.handles().loop.start == 200);
    CHECK(surface.handles().loop.end == 800);
    CHECK(surface.handles().playhead == 500);
}

TEST_CASE("waveform_40", "[pulp-vellum-audio][waveform]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);

    surface.set_trim(100, 400);
    CHECK(surface.begin_handle_edit(WaveformHandleKind::trim_start));
    const auto trim_commit = surface.commit_edit(500);
    CHECK(trim_commit.committed);
    CHECK(surface.handles().trim.start == 400);
    CHECK(surface.handles().trim.end == 400);

    surface.set_loop(200, 700);
    CHECK(surface.begin_handle_edit(WaveformHandleKind::loop_end));
    const auto loop_commit = surface.commit_edit(100);
    CHECK(loop_commit.committed);
    CHECK(surface.handles().loop.start == 200);
    CHECK(surface.handles().loop.end == 200);
}

TEST_CASE("waveform_41", "[pulp-vellum-audio][waveform]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);
    surface.set_slice_markers({200, 600, 800});

    CHECK(surface.begin_handle_edit(WaveformHandleKind::slice_marker, 1));
    const auto cross_commit = surface.commit_edit(850);
    CHECK(cross_commit.committed);
    CHECK(surface.handles().slice_markers.size() == 3);
    CHECK(surface.handles().slice_markers[0] == 200);
    CHECK(surface.handles().slice_markers[1] == 800);
    CHECK(surface.handles().slice_markers[2] == 850);

    CHECK(surface.begin_handle_edit(WaveformHandleKind::slice_marker, 2));
    const auto duplicate_commit = surface.commit_edit(800);
    CHECK(duplicate_commit.committed);
    CHECK(surface.handles().slice_markers.size() == 2);
    CHECK(surface.handles().slice_markers[0] == 200);
    CHECK(surface.handles().slice_markers[1] == 800);
}

TEST_CASE("waveform_42", "[pulp-vellum-audio][waveform]") {
    WaveformSnapSettings settings;
    settings.snap_to_bounds = false;
    settings.tolerance_samples = 50;
    settings.candidates = {-100, 2000};

    auto candidate = resolve_waveform_snap(990, 1000, settings);
    CHECK(!(candidate.snapped));
    CHECK(candidate.sample == 990);

    settings.grid_interval_samples = 600;
    auto grid = resolve_waveform_snap(990, 1000, settings);
    CHECK(!(grid.snapped));
    CHECK(grid.sample == 990);

    settings.tolerance_samples = 500;
    auto lower_grid = resolve_waveform_snap(990, 1000, settings);
    CHECK(lower_grid.snapped);
    CHECK(lower_grid.source == WaveformSnapSource::grid);
    CHECK(lower_grid.sample == 600);

    settings.tolerance_samples = 50;
    settings.candidates = {960};
    auto valid = resolve_waveform_snap(990, 1000, settings);
    CHECK(valid.snapped);
    CHECK(valid.source == WaveformSnapSource::candidate);
    CHECK(valid.sample == 960);
}
