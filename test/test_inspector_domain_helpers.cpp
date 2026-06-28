#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/audio_device_manager.hpp>
#include <pulp/inspect/audio_inspector.hpp>
#include <pulp/inspect/console_capture.hpp>
#include <pulp/inspect/editor_url.hpp>
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/state/store.hpp>

#include "../inspect/src/inspector_overlay_internal.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

using namespace pulp::inspect;
using namespace pulp::state;

TEST_CASE("ConsoleCapture preserves previous callback ordering",
          "[inspect][console]") {
    ConsoleCapture capture;
    bool previous_saw_empty_capture = false;
    std::string previous_message;

    auto cb = capture.callback([&](std::string_view level, std::string_view message) {
        previous_saw_empty_capture = capture.entries().empty();
        previous_message = std::string(level) + ":" + std::string(message);
    });

    cb("debug", "ordered");

    auto entries = capture.entries();
    REQUIRE(previous_saw_empty_capture);
    REQUIRE(previous_message == "debug:ordered");
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].level == "debug");
    REQUIRE(entries[0].message == "ordered");
}

TEST_CASE("Editor URL formatter replaces repeated tokens deterministically",
          "[inspect][editor-url]") {
    auto url = format_editor_url(
        "custom://open?primary={path}&backup={path}&line={line}"
        "&again={line}&col={col}&col2={col}",
        "src/space file.cpp", 27, 4);

    REQUIRE(url ==
            "custom://open?primary=src/space file.cpp&backup=src/space file.cpp"
            "&line=27&again=27&col=4&col2=4");
}

TEST_CASE("Inspector overlay color formatter emits CSS hex with alpha only when needed",
          "[inspect][overlay]") {
    REQUIRE(color_to_hex(pulp::canvas::Color::rgba8(0x0a, 0x1b, 0x2c)) ==
            "#0a1b2c");
    REQUIRE(color_to_hex(pulp::canvas::Color::rgba8(0x0a, 0x1b, 0x2c, 0x7f)) ==
            "#0a1b2c7f");
}

TEST_CASE("AudioInspector MIDI and underrun histories keep newest entries",
          "[inspect][audio]") {
    AudioInspector audio;

    for (int i = 0; i < 205; ++i) {
        audio.log_midi(0x90, static_cast<uint8_t>(i % 128),
                       static_cast<uint8_t>((i * 2) % 128),
                       "midi-" + std::to_string(i));
    }

    auto events = audio.recent_midi();
    REQUIRE(events.size() == 200);
    REQUIRE(events.front().status == 0x90);
    REQUIRE(events.front().data1 == 5);
    REQUIRE(events.front().data2 == 10);
    REQUIRE(events.front().description == "midi-5");
    REQUIRE(events.back().data1 == 204 % 128);
    REQUIRE(events.back().data2 == (204 * 2) % 128);
    REQUIRE(events.back().description == "midi-204");

    AudioConfig cfg;
    cfg.sample_rate = 1000000.0;
    cfg.buffer_size = 1;
    audio.set_config(cfg);
    auto read = audio.config();
    REQUIRE(read.sample_rate == 1000000.0);
    REQUIRE(read.buffer_size == 1);

    audio.set_metering_enabled(true);
    audio.report_levels({{0.75f, 0.5f}});
    auto levels = audio.latest_levels();
    REQUIRE(levels.size() == 1);
    REQUIRE(levels[0].peak == 0.75f);
    REQUIRE(levels[0].rms == 0.5f);

    for (uint64_t frame = 0; frame < 55; ++frame) {
        audio.begin_callback();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        audio.end_callback(frame);
    }

    auto underruns = audio.recent_underruns();
    REQUIRE(underruns.size() == 50);
    REQUIRE(underruns.front().frame == 5);
    REQUIRE(underruns.back().frame == 54);
    REQUIRE(underruns.front().budget_ms > 0.0f);
    REQUIRE(underruns.front().callback_time_ms > underruns.front().budget_ms);
    REQUIRE_THAT(underruns.front().budget_ms,
                 Catch::Matchers::WithinAbs(0.001f, 0.0001f));
}

TEST_CASE("AudioInspector stores runtime telemetry as latest-value snapshot",
          "[inspect][audio][telemetry][snapshot]") {
    AudioInspector audio;

    auto empty = audio.runtime_telemetry();
    REQUIRE_FALSE(empty.available);
    REQUIRE(empty.xrun_count == 0);
    REQUIRE(empty.process_load.callback_count == 0);

    pulp::audio::AudioProcessLoadSnapshot load;
    load.load = 0.25f;
    load.peak_load = 0.75f;
    load.last_load = 0.50f;
    load.elapsed_ns = 1234;
    load.available_ns = 5678;
    load.callback_count = 9;
    load.overload_count = 2;

    audio.set_runtime_telemetry(load, 3);

    auto snapshot = audio.runtime_telemetry();
    REQUIRE(snapshot.available);
    REQUIRE(snapshot.xrun_count == 3);
    REQUIRE(snapshot.process_load.load == 0.25f);
    REQUIRE(snapshot.process_load.peak_load == 0.75f);
    REQUIRE(snapshot.process_load.last_load == 0.50f);
    REQUIRE(snapshot.process_load.elapsed_ns == 1234);
    REQUIRE(snapshot.process_load.available_ns == 5678);
    REQUIRE(snapshot.process_load.callback_count == 9);
    REQUIRE(snapshot.process_load.overload_count == 2);

    audio.clear_runtime_telemetry();
    snapshot = audio.runtime_telemetry();
    REQUIRE_FALSE(snapshot.available);
    REQUIRE(snapshot.xrun_count == 0);
    REQUIRE(snapshot.process_load.callback_count == 0);
}

TEST_CASE("AudioInspector consumes AudioDeviceManager runtime telemetry",
          "[inspect][audio][telemetry][device-manager]") {
    pulp::audio::AudioDeviceManager manager;
    AudioInspector audio;

    manager.begin_cpu_measure(64, 48000.0f);
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    manager.end_cpu_measure();
    manager.bump_xrun_counter(5);

    const auto manager_snapshot = manager.runtime_telemetry_snapshot();
    audio.set_runtime_telemetry(manager_snapshot.process_load,
                                manager_snapshot.xrun_count);

    const auto inspector_snapshot = audio.runtime_telemetry();
    REQUIRE(inspector_snapshot.available);
    REQUIRE(inspector_snapshot.xrun_count == 5);
    REQUIRE(inspector_snapshot.process_load.callback_count ==
            manager_snapshot.process_load.callback_count);
    REQUIRE(inspector_snapshot.process_load.overload_count ==
            manager_snapshot.process_load.overload_count);
    REQUIRE(inspector_snapshot.process_load.elapsed_ns ==
            manager_snapshot.process_load.elapsed_ns);
    REQUIRE(inspector_snapshot.process_load.available_ns ==
            manager_snapshot.process_load.available_ns);
}

TEST_CASE("StateInspector snapshots metadata, display values, and modulation",
          "[inspect][state]") {
    StateStore store;
    ParamInfo gain;
    gain.id = 7;
    gain.name = "Gain";
    gain.unit = "dB";
    gain.range = {-60.0f, 12.0f, -6.0f, 0.5f};
    gain.to_string = [](float value) {
        return std::to_string(static_cast<int>(value)) + " dB";
    };
    store.add_parameter(gain);
    store.set_value(7, -12.0f);
    store.set_mod_offset(7, 3.0f);

    StateInspector inspector(store);
    auto params = inspector.all_params();

    REQUIRE(params.size() == 1);
    REQUIRE(params[0].id == 7);
    REQUIRE(params[0].name == "Gain");
    REQUIRE(params[0].unit == "dB");
    REQUIRE(params[0].value == -12.0f);
    REQUIRE(params[0].modulated == -9.0f);
    REQUIRE(params[0].default_value == -6.0f);
    REQUIRE(params[0].min == -60.0f);
    REQUIRE(params[0].max == 12.0f);
    REQUIRE(params[0].step == 0.5f);
    REQUIRE(params[0].display_value == "-12 dB");
    REQUIRE_THAT(params[0].normalized,
                 Catch::Matchers::WithinAbs(48.0f / 72.0f, 0.0001f));
}

TEST_CASE("StateInspector writes through and caps recent change history",
          "[inspect][state]") {
    StateStore store;
    ParamInfo mix;
    mix.id = 3;
    mix.name = "Mix";
    mix.unit = "%";
    mix.range = {0.0f, 200.0f, 0.0f};
    store.add_parameter(mix);
    StateInspector inspector(store);

    inspector.set_param(3, 12.5f);
    REQUIRE(store.get_value(3) == 12.5f);

    for (int i = 0; i < 105; ++i)
        inspector.set_param(3, static_cast<float>(i));

    auto changes = inspector.recent_changes();
    REQUIRE(changes.size() == 100);
    REQUIRE(changes.front().id == 3);
    REQUIRE(changes.front().value == 5.0f);
    REQUIRE(changes.back().id == 3);
    REQUIRE(changes.back().value == 104.0f);
}
