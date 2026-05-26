/// @file test_audio_device_manager.cpp
/// Item 1.2a — AudioDeviceManager persistence + MIDI hub.
///
/// Acceptance from the macOS-plugin-authoring plan:
///   - restart-with-same-device — save then load round-trips
///   - fallback-to-default-when-persisted-missing — resolver returns
///     the default id and sets the fallback flag
///   - subscriber callback fires
///   - subscriber-out-of-scope (no use-after-free) — token dtor
///     unsubscribes; later dispatch sees zero subscribers
///   - CPU-load reads sensibly under simulated load

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/audio_device_manager.hpp>
#include <pulp/state/properties_file.hpp>
#include <pulp/runtime/temporary_file.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value) : name_(name) {
        if (const char* existing = std::getenv(name)) old_value_ = existing;
        set(name, value);
    }
    ~ScopedEnvVar() {
        if (old_value_) set(name_.c_str(), *old_value_);
        else            unset(name_.c_str());
    }
private:
    static void set(const char* n, const std::string& v) {
#ifdef _WIN32
        _putenv_s(n, v.c_str());
#else
        setenv(n, v.c_str(), 1);
#endif
    }
    static void unset(const char* n) {
#ifdef _WIN32
        _putenv_s(n, "");
#else
        unsetenv(n);
#endif
    }
    std::string name_;
    std::optional<std::string> old_value_;
};

// Spin up a sandboxed `ApplicationProperties` whose paths land under a
// per-test temp directory. Avoids stomping the developer's real
// settings.
struct PropsSandbox {
    pulp::runtime::TemporaryFile marker;
    std::filesystem::path        home;
    std::unique_ptr<ScopedEnvVar> env;
    std::unique_ptr<pulp::state::ApplicationProperties> props;

    explicit PropsSandbox(const std::string& app_name)
        : marker(".home"),
          home(marker.path_string() + "_dir") {
        std::filesystem::create_directories(home);
#ifdef _WIN32
        env = std::make_unique<ScopedEnvVar>("APPDATA", home.string());
#else
        env = std::make_unique<ScopedEnvVar>("HOME", home.string());
#endif
        props = std::make_unique<pulp::state::ApplicationProperties>(app_name);
    }
};

DeviceInfo make_device(std::string id, std::string name) {
    DeviceInfo d;
    d.id = std::move(id);
    d.name = std::move(name);
    d.max_input_channels = 2;
    d.max_output_channels = 2;
    return d;
}

}  // namespace

TEST_CASE("AudioDeviceManager save/load round-trips selection",
          "[audio][audio-device-manager][issue-2935]") {
    PropsSandbox box("PulpADMRoundtrip");

    DeviceSelection sel;
    sel.output_device       = "device:built-in:out";
    sel.input_device        = "device:built-in:in";
    sel.sample_rate         = 48000.0;
    sel.buffer_size         = 256;
    sel.output_channel_mask = 0b11ULL;
    sel.input_channel_mask  = 0b1ULL;

    REQUIRE(AudioDeviceManager::save_selection(*box.props, sel));

    // Fresh ApplicationProperties pointed at the same file should see
    // the persisted values after load() — this is the "restart with
    // same device" path.
    pulp::state::ApplicationProperties reopened("PulpADMRoundtrip");
    reopened.load();
    auto loaded = AudioDeviceManager::load_selection(reopened);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->output_device       == sel.output_device);
    REQUIRE(loaded->input_device        == sel.input_device);
    REQUIRE_THAT(loaded->sample_rate, WithinAbs(48000.0, 0.001));
    REQUIRE(loaded->buffer_size         == 256);
    REQUIRE(loaded->output_channel_mask == 0b11ULL);
    REQUIRE(loaded->input_channel_mask  == 0b1ULL);
}

TEST_CASE("AudioDeviceManager load returns nullopt when no keys exist",
          "[audio][audio-device-manager][issue-2935]") {
    PropsSandbox box("PulpADMNoKeys");
    REQUIRE_FALSE(AudioDeviceManager::load_selection(*box.props).has_value());
}

TEST_CASE("AudioDeviceManager resolves missing devices to the system default",
          "[audio][audio-device-manager][issue-2935]") {
    std::vector<DeviceInfo> available = {
        make_device("device:builtin:out", "Built-in Output"),
        make_device("device:builtin:in",  "Built-in Input"),
    };

    SECTION("persisted device exists — no fallback") {
        DeviceSelection sel;
        sel.output_device = "device:builtin:out";
        sel.input_device  = "device:builtin:in";
        auto r = AudioDeviceManager::resolve_selection(
            sel, available, "device:builtin:out", "device:builtin:in");
        REQUIRE_FALSE(r.fallback_used_output);
        REQUIRE_FALSE(r.fallback_used_input);
        REQUIRE(r.resolved.output_device == "device:builtin:out");
    }

    SECTION("persisted output device missing — falls back, flag set") {
        DeviceSelection sel;
        sel.output_device = "device:scarlett-2i2";  // unplugged
        sel.input_device  = "device:builtin:in";
        auto r = AudioDeviceManager::resolve_selection(
            sel, available, "device:builtin:out", "device:builtin:in");
        REQUIRE(r.fallback_used_output);
        REQUIRE_FALSE(r.fallback_used_input);
        REQUIRE(r.resolved.output_device == "device:builtin:out");
    }

    SECTION("persisted input device missing — falls back independently") {
        DeviceSelection sel;
        sel.output_device = "device:builtin:out";
        sel.input_device  = "device:unknown-input";
        auto r = AudioDeviceManager::resolve_selection(
            sel, available, "device:builtin:out", "device:builtin:in");
        REQUIRE_FALSE(r.fallback_used_output);
        REQUIRE(r.fallback_used_input);
        REQUIRE(r.resolved.input_device == "device:builtin:in");
    }

    SECTION("empty input id with empty fallback — no fallback flagged") {
        DeviceSelection sel;
        sel.output_device = "device:builtin:out";
        sel.input_device  = "";
        auto r = AudioDeviceManager::resolve_selection(
            sel, available, "device:builtin:out", "");
        REQUIRE_FALSE(r.fallback_used_input);
        REQUIRE(r.resolved.input_device.empty());
    }
}

TEST_CASE("AudioDeviceManager selection_to_config derives channel counts from masks",
          "[audio][audio-device-manager][issue-2935]") {
    DeviceSelection sel;
    sel.output_device       = "id";
    sel.sample_rate         = 96000.0;
    sel.buffer_size         = 128;
    sel.output_channel_mask = 0b1111ULL;  // 4 channels
    sel.input_channel_mask  = 0b101ULL;   // 2 channels

    auto cfg = AudioDeviceManager::selection_to_config(sel);
    REQUIRE(cfg.device_id == "id");
    REQUIRE_THAT(cfg.sample_rate, WithinAbs(96000.0, 0.001));
    REQUIRE(cfg.buffer_size     == 128);
    REQUIRE(cfg.output_channels == 4);
    REQUIRE(cfg.input_channels  == 2);
}

TEST_CASE("AudioDeviceManager selection_to_config falls back to defaults on zero",
          "[audio][audio-device-manager][issue-2935]") {
    DeviceSelection sel;  // all defaulted
    auto cfg = AudioDeviceManager::selection_to_config(sel, 2, 1);
    REQUIRE_THAT(cfg.sample_rate, WithinAbs(48000.0, 0.001));
    REQUIRE(cfg.buffer_size     == 256);
    REQUIRE(cfg.output_channels == 2);
    REQUIRE(cfg.input_channels  == 1);
}

TEST_CASE("AudioDeviceManager dispatches MIDI to live subscribers",
          "[audio][audio-device-manager][midi][issue-2935]") {
    AudioDeviceManager mgr;

    std::atomic<int> count_a{0};
    std::atomic<int> count_b{0};
    auto token_a = mgr.subscribe_midi(
        [&](const pulp::midi::MidiEvent&) { ++count_a; });
    auto token_b = mgr.subscribe_midi(
        [&](const pulp::midi::MidiEvent&) { ++count_b; });

    REQUIRE(mgr.midi_subscriber_count() == 2);
    REQUIRE(token_a.active());
    REQUIRE(token_b.active());

    auto note = pulp::midi::MidiEvent::note_on(0, 60, 100);
    mgr.dispatch_midi_event(note);
    mgr.dispatch_midi_event(note);

    REQUIRE(count_a.load() == 2);
    REQUIRE(count_b.load() == 2);
}

TEST_CASE("AudioDeviceManager subscriber going out of scope auto-unsubscribes",
          "[audio][audio-device-manager][midi][issue-2935]") {
    AudioDeviceManager mgr;
    std::atomic<int> count{0};

    {
        auto tok = mgr.subscribe_midi(
            [&](const pulp::midi::MidiEvent&) { ++count; });
        REQUIRE(mgr.midi_subscriber_count() == 1);
        mgr.dispatch_midi_event(pulp::midi::MidiEvent::note_on(0, 60, 100));
        REQUIRE(count.load() == 1);
    }  // tok destroyed here → automatic unsubscribe

    REQUIRE(mgr.midi_subscriber_count() == 0);

    // Dispatch after unsubscribe must NOT call the stale handler — if
    // it did, we'd touch a dangling reference to `count`. Catch2 + ASan
    // CI catch the use-after-free; the assertion here is the
    // observable post-condition.
    mgr.dispatch_midi_event(pulp::midi::MidiEvent::note_on(0, 60, 100));
    REQUIRE(count.load() == 1);
}

TEST_CASE("AudioDeviceManager outliving the manager is a safe no-op",
          "[audio][audio-device-manager][midi][issue-2935]") {
    MidiSubscriptionToken tok;
    {
        AudioDeviceManager mgr;
        tok = mgr.subscribe_midi([](const pulp::midi::MidiEvent&) {});
        REQUIRE(tok.active());
        // Manager goes out of scope here while `tok` lives on.
    }
    // Token destructor must not deref the dead manager.
    REQUIRE_FALSE(tok.active());
    // Explicit reset() on the dead-manager token also safe.
    tok.reset();
}

TEST_CASE("AudioDeviceManager CPU-load tracks work performed in the audio window",
          "[audio][audio-device-manager][cpu-load][issue-2935]") {
    AudioDeviceManager mgr;

    // Take a few cheap measurements first — load should stay well
    // below 1.0 when essentially no work happens inside the window.
    for (int i = 0; i < 32; ++i) {
        mgr.begin_cpu_measure(/*num_frames=*/512, /*sample_rate=*/48000.0f);
        // No work — exit immediately.
        mgr.end_cpu_measure();
    }
    const float idle_load = mgr.cpu_load();
    REQUIRE(idle_load >= 0.0f);
    REQUIRE(idle_load < 0.5f);

    // Now simulate a stressed callback: sleep for ~half a buffer worth
    // of wall-clock time inside the window.
    const auto half_buffer = std::chrono::microseconds(
        (512 * 1'000'000) / 48000 / 2);
    for (int i = 0; i < 32; ++i) {
        mgr.begin_cpu_measure(512, 48000.0f);
        std::this_thread::sleep_for(half_buffer);
        mgr.end_cpu_measure();
    }
    const float busy_load = mgr.cpu_load();
    REQUIRE(busy_load > idle_load);
    // Peak from the busy run must be at least non-trivial. We don't
    // assert a tight upper bound — CI machines vary wildly under
    // load — but the value must be a finite positive number.
    REQUIRE(mgr.peak_cpu_load() > 0.0f);
    REQUIRE(std::isfinite(mgr.peak_cpu_load()));

    mgr.reset_peak_cpu_load();
    REQUIRE(mgr.peak_cpu_load() == 0.0f);
}
