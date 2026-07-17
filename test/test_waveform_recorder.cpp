#include <catch2/catch_test_macros.hpp>

#include <pulp/view/widgets.hpp>

#include <vector>

using namespace pulp::view;

namespace {

// The center transport button lives at the middle of the waveform area, which
// is comfortably the center of the whole widget for the sizes used here.
Point transport_point(const Rect& bounds) {
    return {bounds.width * 0.5f, bounds.height * 0.5f};
}

// A point inside the bottom level-meter strip, biased toward the requested
// normalized x so we can drive the threshold thumb. The meter sits ~30px above
// the bottom edge with 16px horizontal padding (see WaveformRecorder layout).
Point meter_point(const Rect& bounds, float norm_x) {
    const float pad = 16.0f;
    float track_w = bounds.width - 2.0f * pad;
    float x = pad + norm_x * track_w;
    float y = bounds.height - pad - 11.0f;  // middle of the 22px strip
    return {x, y};
}

}  // namespace

TEST_CASE("WaveformRecorder transport button cycles state",
          "[view][waveform_recorder]") {
    WaveformRecorder rec;
    rec.set_bounds({0, 0, 900, 300});

    std::vector<WaveformRecorder::State> seen;
    rec.on_state_change = [&](WaveformRecorder::State s) { seen.push_back(s); };

    REQUIRE(rec.state() == WaveformRecorder::State::armed);

    Point btn = transport_point(rec.local_bounds());

    rec.on_mouse_down(btn);
    REQUIRE(rec.state() == WaveformRecorder::State::recording);

    rec.on_mouse_down(btn);
    REQUIRE(rec.state() == WaveformRecorder::State::captured);

    rec.on_mouse_down(btn);
    REQUIRE(rec.state() == WaveformRecorder::State::armed);

    // Each click fired the callback once with the new state, in order.
    REQUIRE(seen.size() == 3);
    REQUIRE(seen[0] == WaveformRecorder::State::recording);
    REQUIRE(seen[1] == WaveformRecorder::State::captured);
    REQUIRE(seen[2] == WaveformRecorder::State::armed);
}

TEST_CASE("WaveformRecorder transport fires per-state action callbacks",
          "[view][waveform_recorder]") {
    WaveformRecorder rec;
    rec.set_bounds({0, 0, 900, 300});

    int records = 0, stops = 0, plays = 0;
    rec.on_record = [&] { ++records; };
    rec.on_stop = [&] { ++stops; };
    rec.on_play = [&] { ++plays; };

    Point btn = transport_point(rec.local_bounds());

    rec.on_mouse_down(btn);  // armed -> recording: record
    rec.on_mouse_down(btn);  // recording -> captured: stop
    rec.on_mouse_down(btn);  // captured -> armed: play

    REQUIRE(records == 1);
    REQUIRE(stops == 1);
    REQUIRE(plays == 1);
}

TEST_CASE("WaveformRecorder threshold drags only while armed",
          "[view][waveform_recorder]") {
    WaveformRecorder rec;
    rec.set_bounds({0, 0, 900, 300});

    int change_count = 0;
    float last = -1.0f;
    rec.on_threshold_change = [&](float t) {
        ++change_count;
        last = t;
    };

    REQUIRE(rec.state() == WaveformRecorder::State::armed);

    // Drag the thumb toward the right end of the meter.
    auto bounds = rec.local_bounds();
    rec.on_mouse_down(meter_point(bounds, 0.2f));
    rec.on_mouse_drag(meter_point(bounds, 0.8f));
    rec.on_mouse_up(meter_point(bounds, 0.8f));

    REQUIRE(change_count > 0);
    REQUIRE(rec.threshold() > 0.7f);
    REQUIRE(last == rec.threshold());

    // In recording state the meter must NOT change the threshold.
    rec.set_state(WaveformRecorder::State::recording);
    float held = rec.threshold();
    int before = change_count;
    rec.on_mouse_down(meter_point(bounds, 0.1f));
    rec.on_mouse_drag(meter_point(bounds, 0.05f));
    rec.on_mouse_up(meter_point(bounds, 0.05f));
    REQUIRE(rec.threshold() == held);
    REQUIRE(change_count == before);

    // Same in captured state.
    rec.set_state(WaveformRecorder::State::captured);
    rec.on_mouse_down(meter_point(bounds, 0.1f));
    rec.on_mouse_drag(meter_point(bounds, 0.05f));
    rec.on_mouse_up(meter_point(bounds, 0.05f));
    REQUIRE(rec.threshold() == held);
    REQUIRE(change_count == before);
}

TEST_CASE("WaveformRecorder set_state round-trips and is idempotent",
          "[view][waveform_recorder]") {
    WaveformRecorder rec;
    rec.set_bounds({0, 0, 900, 300});

    int changes = 0;
    rec.on_state_change = [&](WaveformRecorder::State) { ++changes; };

    rec.set_state(WaveformRecorder::State::recording);
    REQUIRE(rec.state() == WaveformRecorder::State::recording);
    rec.set_state(WaveformRecorder::State::captured);
    REQUIRE(rec.state() == WaveformRecorder::State::captured);
    rec.set_state(WaveformRecorder::State::armed);
    REQUIRE(rec.state() == WaveformRecorder::State::armed);
    REQUIRE(changes == 3);

    // Setting the same state is a no-op (no callback, no change).
    rec.set_state(WaveformRecorder::State::armed);
    REQUIRE(rec.state() == WaveformRecorder::State::armed);
    REQUIRE(changes == 3);
}

TEST_CASE("WaveformRecorder accessors round-trip",
          "[view][waveform_recorder]") {
    WaveformRecorder rec;
    rec.set_waveform({-1.0f, -0.5f, 0.0f, 0.5f, 1.0f});
    REQUIRE(rec.waveform().size() == 5);

    rec.set_level(0.6f);
    REQUIRE(rec.level() == 0.6f);
    rec.set_level(2.0f);  // clamps
    REQUIRE(rec.level() == 1.0f);

    rec.set_threshold(0.25f);
    REQUIRE(rec.threshold() == 0.25f);
}

// ── Notify on set_state ──────────────────────────────────────────────────────
//
// set_state can drive on_state_change; a Notify chooses whether it does. This
// lets a programmatic re-seed of the transport state avoid echoing back into
// whatever just set it.

TEST_CASE("WaveformRecorder::set_state Notify::none is silent",
          "[view][waveform_recorder][notify]") {
    WaveformRecorder rec;
    int fired = 0;
    rec.on_state_change = [&](WaveformRecorder::State) { ++fired; };

    rec.set_state(WaveformRecorder::State::recording, Notify::none);
    REQUIRE(rec.state() == WaveformRecorder::State::recording);
    REQUIRE(fired == 0);
}

TEST_CASE("WaveformRecorder::set_state Notify::sync / async",
          "[view][waveform_recorder][notify]") {
    pulp::view::flush_async_notifications();
    WaveformRecorder rec;
    std::vector<WaveformRecorder::State> seen;
    rec.on_state_change = [&](WaveformRecorder::State s) { seen.push_back(s); };

    rec.set_state(WaveformRecorder::State::recording, Notify::sync);
    rec.set_state(WaveformRecorder::State::recording, Notify::sync);  // no-op
    REQUIRE(seen.size() == 1);
    REQUIRE(seen.back() == WaveformRecorder::State::recording);

    rec.set_state(WaveformRecorder::State::captured, Notify::async);
    REQUIRE(seen.size() == 1);
    pulp::view::flush_async_notifications();
    REQUIRE(seen.size() == 2);
    REQUIRE(seen.back() == WaveformRecorder::State::captured);
}
