#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
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

TEST_CASE("WaveformEditor handles empty audio without invalid viewport state", "[view][waveform_editor]") {
    WaveformEditor editor;
    editor.set_audio_data(nullptr, 0, 48000.0f);
    editor.set_visible_range(10, 10);

    REQUIRE(editor.total_samples() == 0);
    REQUIRE(editor.sample_rate() == 48000.0f);
    REQUIRE(editor.visible_start() == 0);
    REQUIRE(editor.visible_length() == 0);
    REQUIRE(editor.playhead() == 0);
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

    editor.set_selection(900, 100);
    REQUIRE(cb_start == 100);
    REQUIRE(cb_end == 900);

    editor.clear_selection();
    REQUIRE(cb_start == 0);
    REQUIRE(cb_end == 0);
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

TEST_CASE("WaveformEditor clamps dependent state when replacing audio", "[view][waveform_editor]") {
    WaveformEditor editor;
    auto long_data = make_sine(1000);
    editor.set_audio_data(long_data.data(), 1000, 44100.0f);
    editor.set_selection(700, 950);
    editor.set_playhead(900);
    editor.add_region({800, 1000, "Tail"});

    auto short_data = make_sine(400);
    editor.set_audio_data(short_data.data(), 400, 44100.0f);

    REQUIRE(editor.selection_start() == 400);
    REQUIRE(editor.selection_end() == 400);
    REQUIRE_FALSE(editor.has_selection());
    REQUIRE(editor.playhead() == 400);
    REQUIRE(editor.regions().front().start_sample == 400);
    REQUIRE(editor.regions().front().end_sample == 400);
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

TEST_CASE("WaveformRegion reports signed length", "[view][waveform_editor]") {
    WaveformRegion forward{100, 180, "Forward"};
    REQUIRE(forward.length() == 80);

    WaveformRegion reversed{220, 150, "Reversed"};
    REQUIRE(reversed.length() == -70);
}

TEST_CASE("WaveformEditor paint skips empty audio", "[view][waveform_editor]") {
    WaveformEditor editor;
    editor.set_bounds({0, 0, 400, 150});

    RecordingCanvas canvas;
    editor.paint(canvas);

    REQUIRE(canvas.command_count() == 0);
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

TEST_CASE("WaveformEditor key Cmd+0 zooms to fit", "[view][waveform_editor]") {
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
    REQUIRE(editor.on_key_event(zero));
    REQUIRE(editor.visible_start() == 0);
    REQUIRE(editor.visible_length() == 1000);
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

TEST_CASE("WaveformEditor mouse drag extends selection and release finalizes", "[view][waveform_editor]") {
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

    REQUIRE(editor.selection_start() == 300);
    REQUIRE(editor.selection_end() == 700);
    REQUIRE(editor.selection_length() == 400);
}

TEST_CASE("WaveformViewport clamps empty and edge ranges", "[view][waveform_editor][primitives]") {
    WaveformViewport empty;
    empty.set_bounds({10, 0, 100, 40});
    empty.set_total_samples(0);
    empty.set_visible_range(20, 20);

    REQUIRE(empty.total_samples == 0);
    REQUIRE(empty.visible_start == 0);
    REQUIRE(empty.visible_length == 0);
    REQUIRE(empty.x_to_sample(60) == 0);
    REQUIRE(empty.sample_to_x(123) == Catch::Approx(10.0f));

    WaveformViewport viewport;
    viewport.set_bounds({10, 0, 100, 40});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(995, 1);

    REQUIRE(viewport.visible_start == 984);
    REQUIRE(viewport.visible_length == 16);
    REQUIRE(viewport.visible_end() == 1000);
    REQUIRE(viewport.sample_to_x(984) == Catch::Approx(10.0f));
    REQUIRE(viewport.sample_to_x(1000) == Catch::Approx(110.0f));
    REQUIRE(viewport.x_to_sample(-100.0f) == 984);
    REQUIRE(viewport.x_to_sample(500.0f) == 1000);
    REQUIRE(viewport.samples_per_pixel() == Catch::Approx(0.16));
}

TEST_CASE("WaveformRenderPlan creates bounded sample spans", "[view][waveform_editor][primitives]") {
    WaveformViewport viewport;
    viewport.set_bounds({0, 0, 10, 40});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(100, 100);

    auto plan = build_waveform_render_plan(viewport, 5);

    REQUIRE(plan.spans.size() == 5);
    REQUIRE(plan.spans.front().sample_start == 100);
    REQUIRE(plan.spans.front().sample_end == 120);
    REQUIRE(plan.spans.back().sample_start == 180);
    REQUIRE(plan.spans.back().sample_end == 200);
    REQUIRE(plan.spans.back().x == Catch::Approx(8.0f));
    REQUIRE(plan.spans.back().width == Catch::Approx(2.0f));
}

TEST_CASE("WaveformHandleModel and hit test find nearest visible handles", "[view][waveform_editor][primitives]") {
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
    REQUIRE(handles.size() == 9);
    int visited = 0;
    model.for_each_handle([&](const WaveformHandle&) { ++visited; });
    REQUIRE(visited == 9);
    REQUIRE(model.slice_markers.size() == 3);
    REQUIRE(model.slice_markers[0] == 0);
    REQUIRE(model.slice_markers[1] == 500);
    REQUIRE(model.slice_markers[2] == 999);

    auto selection_hit = hit_test_waveform_handles(viewport, model, 30.0f, 0.25f);
    REQUIRE(selection_hit.kind == WaveformHandleKind::selection_end);
    REQUIRE(selection_hit.sample == 300);

    auto slice_hit = hit_test_waveform_handles(viewport, model, 50.0f, 0.25f);
    REQUIRE(slice_hit.kind == WaveformHandleKind::slice_marker);
    REQUIRE(slice_hit.id == 1);
    REQUIRE(slice_hit.sample == 500);

    auto miss = hit_test_waveform_handles(viewport, model, 55.0f, 0.25f);
    REQUIRE_FALSE(miss);
    REQUIRE(miss.sample == 550);
}

TEST_CASE("WaveformSnapResolver supports bounds candidates and grid", "[view][waveform_editor][primitives]") {
    WaveformSnapSettings settings;
    settings.tolerance_samples = 8;
    settings.candidates = {250};

    auto candidate = resolve_waveform_snap(246, 1000, settings);
    REQUIRE(candidate.snapped);
    REQUIRE(candidate.source == WaveformSnapSource::candidate);
    REQUIRE(candidate.sample == 250);

    auto outside = resolve_waveform_snap(260, 1000, settings);
    REQUIRE_FALSE(outside.snapped);
    REQUIRE(outside.sample == 260);

    settings.grid_interval_samples = 100;
    auto grid = resolve_waveform_snap(296, 1000, settings);
    REQUIRE(grid.snapped);
    REQUIRE(grid.source == WaveformSnapSource::grid);
    REQUIRE(grid.sample == 300);

    auto bounds = resolve_waveform_snap(3, 1000, settings);
    REQUIRE(bounds.snapped);
    REQUIRE(bounds.source == WaveformSnapSource::bounds);
    REQUIRE(bounds.sample == 0);
}

TEST_CASE("WaveformPlayheadOverlay reports visibility from viewport", "[view][waveform_editor][primitives]") {
    WaveformViewport viewport;
    viewport.set_bounds({0, 0, 80, 40});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(100, 400);

    auto visible = build_waveform_playhead_overlay(viewport, 200);
    REQUIRE(visible.visible);
    REQUIRE(visible.sample == 200);
    REQUIRE(visible.x == Catch::Approx(20.0f));

    auto hidden = build_waveform_playhead_overlay(viewport, 600);
    REQUIRE_FALSE(hidden.visible);
    REQUIRE(hidden.sample == 600);
    REQUIRE(hidden.x == Catch::Approx(0.0f));
}

TEST_CASE("Waveform point handles are visible and hittable at the right edge", "[view][waveform_editor][primitives]") {
    WaveformViewport viewport;
    viewport.set_bounds({0, 0, 100, 40});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(100, 400);

    REQUIRE(viewport.visible_end() == 500);
    REQUIRE_FALSE(viewport.sample_visible(500));
    REQUIRE(viewport.sample_point_visible(500));

    WaveformHandleModel model;
    model.set_total_samples(1000);
    model.set_loop(200, 500);

    auto hit = hit_test_waveform_handles(viewport, model, 100.0f, 0.1f);
    REQUIRE(hit.kind == WaveformHandleKind::loop_end);
    REQUIRE(hit.sample == 500);

    auto playhead = build_waveform_playhead_overlay(viewport, 500);
    REQUIRE(playhead.visible);
    REQUIRE(playhead.x == Catch::Approx(100.0f));

    viewport.set_visible_range(984, 16);
    auto final_playhead = build_waveform_playhead_overlay(viewport, 1000);
    REQUIRE(final_playhead.visible);
    REQUIRE(final_playhead.x == Catch::Approx(100.0f));
}

TEST_CASE("WaveformEditorTransaction keeps semantic edit state outside input events", "[view][waveform_editor][primitives]") {
    WaveformEditorTransaction tx;
    tx.begin(WaveformEditorOperationKind::create_selection, 300);

    auto update = tx.update(100);
    REQUIRE(update.active);
    REQUIRE(update.selection.start == 100);
    REQUIRE(update.selection.end == 300);

    auto commit = tx.commit(500);
    REQUIRE(commit.committed);
    REQUIRE(commit.selection.start == 300);
    REQUIRE(commit.selection.end == 500);
    REQUIRE_FALSE(tx.active());

    tx.begin(WaveformEditorOperationKind::drag_selection_start, 0, {100, 400});
    auto dragged = tx.update(350);
    REQUIRE(dragged.selection.start == 350);
    REQUIRE(dragged.selection.end == 400);

    auto cancelled = tx.cancel();
    REQUIRE(cancelled.cancelled);
    REQUIRE(cancelled.selection.start == 100);
    REQUIRE(cancelled.selection.end == 400);
    REQUIRE_FALSE(tx.active());

    tx.begin(WaveformEditorOperationKind::move_playhead, 0, {}, 120);
    auto moved = tx.commit(480);
    REQUIRE(moved.playhead_sample == 480);
}

TEST_CASE("WaveformEditorSurface composes viewport handles render plan and playhead", "[view][waveform_editor][surface]") {
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
    REQUIRE(snapshot.viewport.visible_start == 100);
    REQUIRE(snapshot.viewport.visible_length == 400);
    REQUIRE(snapshot.handles.has_trim);
    REQUIRE(snapshot.handles.has_loop);
    REQUIRE(snapshot.handles.slice_markers.size() == 2);
    REQUIRE(snapshot.playhead.visible);
    REQUIRE(snapshot.playhead.x == Catch::Approx(25.0f));

    const auto plan = surface.render_plan(4);
    REQUIRE(plan.spans.size() == 4);
    REQUIRE(plan.spans.front().sample_start == 100);
    REQUIRE(plan.spans.front().sample_end == 200);
    REQUIRE(plan.spans.back().sample_start == 400);
    REQUIRE(plan.spans.back().sample_end == 500);

    const auto hit = surface.hit_test(25.0f, 0.1f);
    REQUIRE(hit.kind == WaveformHandleKind::loop_start);
    REQUIRE(hit.sample == 200);
}

TEST_CASE("WaveformEditorSurface edits sampler handles with snapping", "[view][waveform_editor][surface]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);

    WaveformSnapSettings snap;
    snap.grid_interval_samples = 50;
    snap.tolerance_samples = 20;
    surface.set_snap_settings(snap);

    surface.set_trim(100, 900);
    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::trim_start));
    auto trim_update = surface.update_edit(138);
    REQUIRE(trim_update.snap.snapped);
    REQUIRE(trim_update.snap.sample == 150);
    REQUIRE(surface.handles().trim.start == 150);
    auto trim_commit = surface.commit_edit(180);
    REQUIRE(trim_commit.committed);
    REQUIRE(surface.handles().trim.start == 200);
    REQUIRE(surface.handles().trim.end == 900);

    surface.set_loop(200, 800);
    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::loop_end));
    auto loop_commit = surface.commit_edit(752);
    REQUIRE(loop_commit.snap.sample == 750);
    REQUIRE(surface.handles().loop.start == 200);
    REQUIRE(surface.handles().loop.end == 750);

    surface.set_fade_in(100);
    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::fade_in));
    const auto fade_in_commit = surface.commit_edit(138);
    REQUIRE(fade_in_commit.committed);
    REQUIRE(surface.handles().fade_in_end == 150);

    surface.set_fade_out(900);
    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::fade_out));
    const auto fade_out_commit = surface.commit_edit(861);
    REQUIRE(fade_out_commit.committed);
    REQUIRE(surface.handles().fade_out_start == 850);

    surface.set_slice_markers({200, 600});
    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::slice_marker, 1));
    const auto slice_commit = surface.commit_edit(638);
    REQUIRE(slice_commit.committed);
    REQUIRE(surface.handles().slice_markers.size() == 2);
    REQUIRE(surface.handles().slice_markers[0] == 200);
    REQUIRE(surface.handles().slice_markers[1] == 650);

    surface.set_playhead(0);
    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::playhead));
    const auto playhead_commit = surface.commit_edit(901);
    REQUIRE(playhead_commit.committed);
    REQUIRE(surface.handles().has_playhead);
    REQUIRE(surface.handles().playhead == 900);

    REQUIRE(surface.begin_selection_edit(123));
    const auto selection_commit = surface.commit_edit(288);
    REQUIRE(selection_commit.committed);
    REQUIRE(surface.handles().selection.start == 123);
    REQUIRE(surface.handles().selection.end == 300);

    REQUIRE(surface.begin_selection_edit(250));
    const auto empty_selection = surface.commit_edit(250);
    REQUIRE(empty_selection.committed);
    REQUIRE_FALSE(surface.handles().has_selection);
}

TEST_CASE("WaveformEditorSurface direct selection setters keep empty selections hidden", "[view][waveform_editor][surface]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);

    surface.set_selection(250, 250);
    REQUIRE_FALSE(surface.handles().has_selection);
    REQUIRE_FALSE(surface.snapshot().handles.has_selection);

    surface.set_selection(-200, -100);
    REQUIRE_FALSE(surface.handles().has_selection);
    surface.set_selection(1200, 1400);
    REQUIRE_FALSE(surface.handles().has_selection);

    surface.set_selection(700, 950);
    REQUIRE(surface.handles().has_selection);
    surface.set_total_samples(400);
    REQUIRE_FALSE(surface.handles().has_selection);
    REQUIRE_FALSE(surface.snapshot().handles.has_selection);
}

TEST_CASE("WaveformEditorSurface selection handle edits stay cleared after sample shrink", "[view][waveform_editor][surface]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);
    surface.set_selection(700, 950);

    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::selection_start));
    surface.set_total_samples(400);
    REQUIRE_FALSE(surface.handles().has_selection);

    const auto update = surface.update_edit(300);
    REQUIRE(update.active);
    REQUIRE_FALSE(update.handles.has_selection);
    REQUIRE_FALSE(surface.handles().has_selection);

    const auto commit = surface.commit_edit(300);
    REQUIRE(commit.committed);
    REQUIRE_FALSE(surface.handles().has_selection);
}

TEST_CASE("WaveformEditorSurface rejects invalid edits and cancel restores initial handles", "[view][waveform_editor][surface]") {
    WaveformEditorSurface empty;
    REQUIRE_FALSE(empty.begin_selection_edit(10));
    REQUIRE_FALSE(empty.begin_handle_edit(WaveformHandleKind::playhead));

    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);
    surface.set_loop(200, 700);
    surface.set_slice_markers({100});

    REQUIRE_FALSE(surface.begin_handle_edit(WaveformHandleKind::playhead));
    REQUIRE_FALSE(surface.begin_handle_edit(WaveformHandleKind::trim_start));
    REQUIRE_FALSE(surface.begin_handle_edit(WaveformHandleKind::slice_marker, 3));

    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::loop_start));
    const auto loop_update = surface.update_edit(450);
    REQUIRE(loop_update.active);
    REQUIRE(surface.handles().loop.start == 450);
    REQUIRE_FALSE(surface.begin_handle_edit(WaveformHandleKind::loop_end));
    REQUIRE_FALSE(surface.begin_selection_edit(300));

    auto cancelled = surface.cancel_edit();
    REQUIRE(cancelled.cancelled);
    REQUIRE_FALSE(surface.edit_active());
    REQUIRE(surface.handles().loop.start == 200);
    REQUIRE(surface.handles().loop.end == 700);

    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::loop_end, 0));
    const auto second_cancel = surface.cancel_edit();
    REQUIRE(second_cancel.cancelled);
    REQUIRE_FALSE(surface.begin_handle_edit(WaveformHandleKind::loop_start, 999));
}

TEST_CASE("WaveformEditorSurface preserves unrelated live state during active edits", "[view][waveform_editor][surface]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);
    surface.set_loop(200, 800);
    surface.set_playhead(100);

    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::loop_start));
    surface.set_playhead(500);
    const auto update = surface.update_edit(300);
    REQUIRE(update.active);
    REQUIRE(surface.handles().loop.start == 300);
    REQUIRE(surface.handles().loop.end == 800);
    REQUIRE(surface.handles().playhead == 500);

    const auto cancel = surface.cancel_edit();
    REQUIRE(cancel.cancelled);
    REQUIRE(surface.handles().loop.start == 200);
    REQUIRE(surface.handles().loop.end == 800);
    REQUIRE(surface.handles().playhead == 500);
}

TEST_CASE("WaveformEditorSurface trim and loop handles do not cross endpoints", "[view][waveform_editor][surface]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);

    surface.set_trim(100, 400);
    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::trim_start));
    const auto trim_commit = surface.commit_edit(500);
    REQUIRE(trim_commit.committed);
    REQUIRE(surface.handles().trim.start == 400);
    REQUIRE(surface.handles().trim.end == 400);

    surface.set_loop(200, 700);
    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::loop_end));
    const auto loop_commit = surface.commit_edit(100);
    REQUIRE(loop_commit.committed);
    REQUIRE(surface.handles().loop.start == 200);
    REQUIRE(surface.handles().loop.end == 200);
}

TEST_CASE("WaveformEditorSurface slice marker ids are transient sorted indices", "[view][waveform_editor][surface]") {
    WaveformEditorSurface surface;
    surface.set_total_samples(1000);
    surface.set_bounds({0, 0, 100, 40});
    surface.set_visible_range(0, 1000);
    surface.set_slice_markers({200, 600, 800});

    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::slice_marker, 1));
    const auto cross_commit = surface.commit_edit(850);
    REQUIRE(cross_commit.committed);
    REQUIRE(surface.handles().slice_markers.size() == 3);
    REQUIRE(surface.handles().slice_markers[0] == 200);
    REQUIRE(surface.handles().slice_markers[1] == 800);
    REQUIRE(surface.handles().slice_markers[2] == 850);

    REQUIRE(surface.begin_handle_edit(WaveformHandleKind::slice_marker, 2));
    const auto duplicate_commit = surface.commit_edit(800);
    REQUIRE(duplicate_commit.committed);
    REQUIRE(surface.handles().slice_markers.size() == 2);
    REQUIRE(surface.handles().slice_markers[0] == 200);
    REQUIRE(surface.handles().slice_markers[1] == 800);
}

TEST_CASE("WaveformSnapResolver rejects implicit boundary snaps from out-of-range candidates", "[view][waveform_editor][primitives]") {
    WaveformSnapSettings settings;
    settings.snap_to_bounds = false;
    settings.tolerance_samples = 50;
    settings.candidates = {-100, 2000};

    auto candidate = resolve_waveform_snap(990, 1000, settings);
    REQUIRE_FALSE(candidate.snapped);
    REQUIRE(candidate.sample == 990);

    settings.grid_interval_samples = 600;
    auto grid = resolve_waveform_snap(990, 1000, settings);
    REQUIRE_FALSE(grid.snapped);
    REQUIRE(grid.sample == 990);

    settings.tolerance_samples = 500;
    auto lower_grid = resolve_waveform_snap(990, 1000, settings);
    REQUIRE(lower_grid.snapped);
    REQUIRE(lower_grid.source == WaveformSnapSource::grid);
    REQUIRE(lower_grid.sample == 600);

    settings.tolerance_samples = 50;
    settings.candidates = {960};
    auto valid = resolve_waveform_snap(990, 1000, settings);
    REQUIRE(valid.snapped);
    REQUIRE(valid.source == WaveformSnapSource::candidate);
    REQUIRE(valid.sample == 960);
}
