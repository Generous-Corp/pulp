// A screenshot-only standalone launch must never touch the audio backend.
// `--screenshot` paints a few frames, writes a PNG and exits, so the device
// callback could only ever push silence at whoever is sitting at the machine —
// and opening a device is exactly what made the Standalone format unverifiable
// without a human present. StandaloneApp::start() therefore skips the audio
// system + device entirely for such a run, while every other launch (including
// a capture that also asks for a live probe/scope/WAV readout) still creates,
// opens and starts the device exactly as before.
//
// The device lifecycle is asserted through a fake AudioSystem so both
// directions run on a machine (and in CI) with no audio hardware and, more to
// the point, without opening one.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/device.hpp>
#include <pulp/format/detail/standalone_environment.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::format;

namespace {

// Every device-lifecycle call the host can make, counted. Shared by the fake
// system and the test so an assertion reads the tally after start().
struct DeviceCalls {
    int systems_created = 0;
    int devices_created = 0;
    int opened = 0;
    int started = 0;
    int stopped = 0;
    int closed = 0;
};

class FakeAudioDevice : public audio::AudioDevice {
public:
    explicit FakeAudioDevice(DeviceCalls& calls) : calls_(calls) {}

    bool open(const audio::DeviceConfig& config) override {
        ++calls_.opened;
        config_ = config;
        open_ = true;
        return true;
    }
    void close() override { ++calls_.closed; open_ = false; }
    bool start(audio::AudioCallback callback) override {
        ++calls_.started;
        callback_ = std::move(callback);
        running_ = true;
        return true;
    }
    void stop() override { ++calls_.stopped; running_ = false; }

    bool is_open() const override { return open_; }
    bool is_running() const override { return running_; }
    audio::DeviceInfo info() const override {
        audio::DeviceInfo i;
        i.id = "fake-device";
        i.name = "Fake Device";
        i.max_output_channels = 2;
        i.is_default_output = true;
        return i;
    }
    double sample_rate() const override { return config_.sample_rate; }
    int buffer_size() const override { return config_.buffer_size; }

private:
    DeviceCalls& calls_;
    audio::DeviceConfig config_{};
    audio::AudioCallback callback_;
    bool open_ = false;
    bool running_ = false;
};

class FakeAudioSystem : public audio::AudioSystem {
public:
    explicit FakeAudioSystem(DeviceCalls& calls) : calls_(calls) {}

    std::vector<audio::DeviceInfo> enumerate_devices() override {
        return {default_output_device()};
    }
    std::unique_ptr<audio::AudioDevice> create_device(const std::string&) override {
        ++calls_.devices_created;
        return std::make_unique<FakeAudioDevice>(calls_);
    }
    audio::DeviceInfo default_output_device() override {
        audio::DeviceInfo i;
        i.id = "fake-device";
        i.name = "Fake Device";
        i.max_output_channels = 2;
        i.is_default_output = true;
        return i;
    }
    audio::DeviceInfo default_input_device() override { return {}; }

private:
    DeviceCalls& calls_;
};

class CaptureProbeProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "StandaloneCaptureProbe";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.standalone-capture";
        d.input_buses = {{"Audio In", 2}};
        d.output_buses = {{"Audio Out", 2}};
        d.accepts_midi = false;  // no CoreMIDI client from a unit test
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

std::unique_ptr<Processor> make_capture_probe() {
    return std::make_unique<CaptureProbeProcessor>();
}

StandaloneConfig base_config() {
    StandaloneConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.buffer_size = 256;
    cfg.output_channels = 2;
    cfg.input_channels = 0;
    cfg.persist_settings = false;  // no ApplicationProperties I/O in a test
    return cfg;
}

// Scoped environment variable, restored on destruction so one test's env can
// never leak into the next.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* prev = std::getenv(name)) {
            had_previous_ = true;
            previous_ = prev;
        }
        ::setenv(name, value, 1);
    }
    ~ScopedEnv() {
        if (had_previous_) ::setenv(name_.c_str(), previous_.c_str(), 1);
        else ::unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

}  // namespace

namespace pulp::format {
// Injects the fake audio system so the device-lifecycle contract is asserted
// without opening real hardware. Mirrors the StandaloneRenderTestAccess hook.
struct StandaloneAudioDeviceTestAccess {
    static void set_audio_system_factory(
        StandaloneApp& app,
        std::function<std::unique_ptr<audio::AudioSystem>()> factory) {
        app.audio_system_factory_ = std::move(factory);
    }
    static audio::AudioDevice* device(StandaloneApp& app) {
        return app.audio_device_.get();
    }
};
}  // namespace pulp::format

using pulp::format::StandaloneAudioDeviceTestAccess;

namespace {

// Routes the app's audio system, if the host asks for one, to the fake.
// `calls.systems_created` therefore counts host requests for the backend: a
// screenshot-only run must leave it at 0.
void install_fake_audio(StandaloneApp& app, DeviceCalls& calls) {
    StandaloneAudioDeviceTestAccess::set_audio_system_factory(app, [&calls] {
        ++calls.systems_created;
        return std::make_unique<FakeAudioSystem>(calls);
    });
}

}  // namespace

TEST_CASE("Standalone screenshot capture opens no audio device",
          "[format][standalone][screenshot][audio-device]") {
    DeviceCalls calls;
    StandaloneApp app(&make_capture_probe);
    install_fake_audio(app, calls);

    StandaloneConfig cfg = base_config();
    cfg.headless = true;
    cfg.screenshot_path = "/tmp/pulp-standalone-capture-test.png";
    app.set_config(cfg);

    REQUIRE(app.start());

    // The identity assertion: the host never asked for an audio backend, so no
    // device object exists to have been created, opened or started.
    CHECK(calls.systems_created == 0);
    CHECK(calls.devices_created == 0);
    CHECK(calls.opened == 0);
    CHECK(calls.started == 0);
    CHECK(app.audio_system() == nullptr);
    CHECK(StandaloneAudioDeviceTestAccess::device(app) == nullptr);
    CHECK(app.audio_skipped_for_capture());

    // The app is otherwise live: the processor exists and was prepared, so the
    // editor the capture is about to photograph has real state behind it.
    CHECK(app.is_running());
    CHECK(app.processor() != nullptr);

    app.stop();
    CHECK_FALSE(app.is_running());
    CHECK(calls.stopped == 0);
    CHECK(calls.closed == 0);
}

TEST_CASE("Standalone without a screenshot still opens the audio device",
          "[format][standalone][audio-device]") {
    DeviceCalls calls;
    StandaloneApp app(&make_capture_probe);
    install_fake_audio(app, calls);

    StandaloneConfig cfg = base_config();
    app.set_config(cfg);

    REQUIRE(app.start());

    // The normal path is untouched: create → open → start, exactly as before.
    CHECK(calls.systems_created == 1);
    CHECK(calls.devices_created == 1);
    CHECK(calls.opened == 1);
    CHECK(calls.started == 1);
    CHECK(app.audio_system() != nullptr);
    CHECK(StandaloneAudioDeviceTestAccess::device(app) != nullptr);
    CHECK(StandaloneAudioDeviceTestAccess::device(app)->is_running());
    CHECK_FALSE(app.audio_skipped_for_capture());
    CHECK(app.is_running());

    app.stop();
    CHECK(calls.stopped == 1);
    CHECK(calls.closed == 1);
}

TEST_CASE("Standalone capture keeps audio when a live readout is requested",
          "[format][standalone][screenshot][audio-device]") {
    StandaloneConfig cfg = base_config();
    cfg.headless = true;
    cfg.screenshot_path = "/tmp/pulp-standalone-capture-test.png";

    SECTION("probe JSON") {
        cfg.audio_probe_json_path = "/tmp/pulp-probe.json";
    }
    SECTION("scope JSON") {
        cfg.audio_scope_json_path = "/tmp/pulp-scope.json";
    }
    SECTION("capture WAV") {
        cfg.audio_capture_wav_path = "/tmp/pulp-capture.wav";
    }
    SECTION("rolling capture WAV") {
        cfg.audio_capture_rolling_path = "/tmp/pulp-rolling.wav";
    }
    SECTION("explicit keep-audio opt-in") {
        cfg.screenshot_keeps_audio = true;
    }

    DeviceCalls calls;
    StandaloneApp app(&make_capture_probe);
    install_fake_audio(app, calls);
    app.set_config(cfg);

    REQUIRE(app.start());

    // These readouts are produced BY the render callback, so the device has to
    // run for the capture to mean anything.
    CHECK(calls.devices_created == 1);
    CHECK(calls.opened == 1);
    CHECK(calls.started == 1);
    CHECK_FALSE(app.audio_skipped_for_capture());

    app.stop();
}

TEST_CASE("standalone_capture_skips_audio decides on the whole request",
          "[format][standalone][screenshot][audio-device]") {
    using detail::standalone_capture_skips_audio;

    StandaloneConfig cfg = base_config();

    // No capture requested: an ordinary launch always gets its device.
    CHECK_FALSE(standalone_capture_skips_audio(cfg));

    cfg.screenshot_path = "/tmp/shot.png";
    CHECK(standalone_capture_skips_audio(cfg));

    SECTION("a probe JSON readout keeps audio") {
        cfg.audio_probe_json_path = "/tmp/probe.json";
        CHECK_FALSE(standalone_capture_skips_audio(cfg));
    }
    SECTION("a scope JSON readout keeps audio") {
        cfg.audio_scope_json_path = "/tmp/scope.json";
        CHECK_FALSE(standalone_capture_skips_audio(cfg));
    }
    SECTION("a capture WAV readout keeps audio") {
        cfg.audio_capture_wav_path = "/tmp/capture.wav";
        CHECK_FALSE(standalone_capture_skips_audio(cfg));
    }
    SECTION("a rolling capture WAV readout keeps audio") {
        cfg.audio_capture_rolling_path = "/tmp/rolling.wav";
        CHECK_FALSE(standalone_capture_skips_audio(cfg));
    }
    SECTION("the explicit opt-in keeps audio") {
        cfg.screenshot_keeps_audio = true;
        CHECK_FALSE(standalone_capture_skips_audio(cfg));
    }
    SECTION("headless alone does not skip audio") {
        cfg.screenshot_path.clear();
        cfg.headless = true;
        CHECK_FALSE(standalone_capture_skips_audio(cfg));
    }
}

TEST_CASE("PULP_SCREENSHOT_KEEP_AUDIO opts a capture back into audio",
          "[format][standalone][screenshot][audio-device]") {
    StandaloneConfig cfg = base_config();
    cfg.screenshot_path = "/tmp/shot.png";

    {
        ScopedEnv keep("PULP_SCREENSHOT_KEEP_AUDIO", "1");
        auto resolved = detail::standalone_config_from_environment(cfg);
        CHECK(resolved.screenshot_keeps_audio);
        CHECK_FALSE(detail::standalone_capture_skips_audio(resolved));
    }

    {
        ScopedEnv keep("PULP_SCREENSHOT_KEEP_AUDIO", "0");
        auto resolved = detail::standalone_config_from_environment(cfg);
        CHECK_FALSE(resolved.screenshot_keeps_audio);
        CHECK(detail::standalone_capture_skips_audio(resolved));
    }
}
