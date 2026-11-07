// The standalone app remembers the plug-in's state across launches: the
// parameters plus whatever the plug-in saves itself, kept in the same settings
// file as the device selection. These tests launch the app against a fake audio
// system (no hardware is opened), quit it, launch it again under the same name,
// and check what came back.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/device.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/state/properties_file.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace pulp;
using namespace pulp::format;
using namespace std::chrono_literals;

namespace {

constexpr state::ParamID kGain = 1;
constexpr state::ParamID kMeter = 2;

class FakeAudioDevice : public audio::AudioDevice {
  public:
    bool open(const audio::DeviceConfig& config) override {
        config_ = config;
        open_ = true;
        return true;
    }
    void close() override {
        open_ = false;
    }
    bool start(audio::AudioCallback) override {
        running_ = true;
        return true;
    }
    void stop() override {
        running_ = false;
    }
    bool is_open() const override {
        return open_;
    }
    bool is_running() const override {
        return running_;
    }
    audio::DeviceInfo info() const override {
        audio::DeviceInfo i;
        i.id = "fake-device";
        i.name = "Fake Device";
        i.max_output_channels = 2;
        i.is_default_output = true;
        return i;
    }
    double sample_rate() const override {
        return config_.sample_rate;
    }
    int buffer_size() const override {
        return config_.buffer_size;
    }

  private:
    audio::DeviceConfig config_{};
    bool open_ = false;
    bool running_ = false;
};

class FakeAudioSystem : public audio::AudioSystem {
  public:
    std::vector<audio::DeviceInfo> enumerate_devices() override {
        return {default_output_device()};
    }
    std::unique_ptr<audio::AudioDevice> create_device(const std::string&) override {
        return std::make_unique<FakeAudioDevice>();
    }
    audio::DeviceInfo default_output_device() override {
        audio::DeviceInfo i;
        i.id = "fake-device";
        i.name = "Fake Device";
        i.max_output_channels = 2;
        i.is_default_output = true;
        return i;
    }
    audio::DeviceInfo default_input_device() override {
        return {};
    }
};

// The settings file is keyed by the plug-in name, so each test run uses its own.
std::string& app_name() {
    static std::string name = [] {
#ifdef _WIN32
        const auto pid = ::_getpid();
#else
        const auto pid = ::getpid();
#endif
        return "PulpStandaloneStateTest" + std::to_string(pid);
    }();
    return name;
}

class StatefulProcessor : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = app_name();
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.standalone-state";
        d.input_buses = {{"Audio In", 2}};
        d.output_buses = {{"Audio Out", 2}};
        d.accepts_midi = false;
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kGain, .name = "Gain", .range = {0.0f, 1.0f, 0.5f}});
        state::ParamInfo meter{.id = kMeter, .name = "Level", .range = {0.0f, 1.0f, 0.0f}};
        meter.read_only = true;
        store.add_parameter(meter);
    }
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const ProcessContext&) override {}

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(note.begin(), note.end());
    }
    bool deserialize_plugin_state(std::span<const uint8_t> bytes) override {
        const std::string text(bytes.begin(), bytes.end());
        if (text == "refuse")
            return false;
        note = text;
        return true;
    }

    std::string note;
};

std::unique_ptr<Processor> make_processor() {
    return std::make_unique<StatefulProcessor>();
}

StatefulProcessor& processor_of(StandaloneApp& app) {
    auto* processor = dynamic_cast<StatefulProcessor*>(app.processor());
    REQUIRE(processor != nullptr);
    return *processor;
}

} // namespace

namespace pulp::format {
struct StandaloneAudioDeviceTestAccess {
    static void
    set_audio_system_factory(StandaloneApp& app,
                             std::function<std::unique_ptr<audio::AudioSystem>()> factory) {
        app.audio_system_factory_ = std::move(factory);
    }
};
} // namespace pulp::format

namespace {

StandaloneConfig persisting_config() {
    StandaloneConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.buffer_size = 256;
    cfg.output_channels = 2;
    cfg.input_channels = 0;
    cfg.persist_settings = true;
    return cfg;
}

void install_fake_audio(StandaloneApp& app) {
    StandaloneAudioDeviceTestAccess::set_audio_system_factory(
        app, [] { return std::make_unique<FakeAudioSystem>(); });
}

void forget_saved_settings() {
    std::error_code ec;
    std::filesystem::remove_all(state::ApplicationProperties::user_settings_dir(app_name()), ec);
}

// Launch, run `use`, quit.
template <typename Use> void run_session(const StandaloneConfig& cfg, Use&& use) {
    StandaloneApp app(&make_processor);
    install_fake_audio(app);
    app.set_config(cfg);
    REQUIRE(app.start());
    use(app);
    app.stop();
}

// Removes the settings file when a test ends, however it ends.
struct ScopedSettings {
    ScopedSettings() {
        forget_saved_settings();
    }
    ~ScopedSettings() {
        forget_saved_settings();
    }
};

} // namespace

TEST_CASE("standalone state blob round-trips beside the device selection",
          "[format][standalone][persistence][state]") {
    ScopedSettings settings;

    StandaloneConfig cfg = persisting_config();
    cfg.audio_device_id = "device-42";
    cfg.tempo_bpm = 133.0;
    REQUIRE(StandaloneApp::save_persisted_config(app_name(), cfg));

    REQUIRE_FALSE(StandaloneApp::load_persisted_state(app_name()).has_value());

    const std::vector<uint8_t> blob{0, 1, 2, 3, 250, 251, 252, 253, 254, 255};
    REQUIRE(StandaloneApp::save_persisted_state(app_name(), blob));
    REQUIRE(StandaloneApp::load_persisted_state(app_name()) == blob);

    // Saving the state kept the device selection, and saving the selection
    // again keeps the state.
    REQUIRE(StandaloneApp::load_persisted_config(app_name()).audio_device_id == "device-42");
    cfg.tempo_bpm = 97.0;
    REQUIRE(StandaloneApp::save_persisted_config(app_name(), cfg));
    REQUIRE(StandaloneApp::load_persisted_state(app_name()) == blob);
    REQUIRE(StandaloneApp::load_persisted_config(app_name()).tempo_bpm == 97.0);
}

TEST_CASE("standalone state blob refuses what cannot be stored", "[format][standalone][state]") {
    ScopedSettings settings;

    REQUIRE_FALSE(StandaloneApp::save_persisted_state("", std::vector<uint8_t>{1}));
    REQUIRE_FALSE(StandaloneApp::load_persisted_state("").has_value());

    const std::vector<uint8_t> too_big(StandaloneApp::kMaxPersistedStateBytes + 1, 7);
    REQUIRE_FALSE(StandaloneApp::save_persisted_state(app_name(), too_big));
    REQUIRE_FALSE(StandaloneApp::load_persisted_state(app_name()).has_value());

    // Text that is not base64 reads as nothing saved.
    state::ApplicationProperties props(app_name());
    props.load();
    props.user_settings().set_string("standalone.plugin_state", "not base64 !!");
    props.save();
    REQUIRE_FALSE(StandaloneApp::load_persisted_state(app_name()).has_value());
}

TEST_CASE("standalone restores the state it saved on quit", "[format][standalone][state]") {
    ScopedSettings settings;
    const auto cfg = persisting_config();

    run_session(cfg, [](StandaloneApp& app) {
        REQUIRE(app.plugin_state_persistence_active());
        // First launch: nothing saved, so the processor starts at its defaults.
        REQUIRE(app.state().get_value(kGain) == 0.5f);
        REQUIRE(processor_of(app).note.empty());

        app.state().set_value(kGain, 0.8f);
        processor_of(app).note = "my sample";
    });
    REQUIRE(StandaloneApp::load_persisted_state(app_name()).has_value());

    run_session(cfg, [](StandaloneApp& app) {
        // Back before anything else touches the processor.
        REQUIRE(app.state().get_value(kGain) == 0.8f);
        REQUIRE(processor_of(app).note == "my sample");
    });
}

TEST_CASE("standalone starts clean when the saved state is unusable",
          "[format][standalone][state]") {
    ScopedSettings settings;
    const auto cfg = persisting_config();

    SECTION("nothing was saved") {
        run_session(cfg, [](StandaloneApp& app) { REQUIRE(app.state().get_value(kGain) == 0.5f); });
    }
    SECTION("the blob is not plug-in state") {
        REQUIRE(StandaloneApp::save_persisted_state(app_name(),
                                                    std::vector<uint8_t>{'n', 'o', 'p', 'e'}));
        run_session(cfg, [](StandaloneApp& app) {
            REQUIRE(app.state().get_value(kGain) == 0.5f);
            REQUIRE(processor_of(app).note.empty());
        });
    }
    SECTION("the blob was cut short") {
        StatefulProcessor writer;
        state::StateStore store;
        writer.set_state_store(&store);
        writer.define_parameters(store);
        store.set_value(kGain, 0.9f);
        writer.note = "kept";
        auto blob = plugin_state_io::serialize(store, writer);
        blob.resize(blob.size() / 2);
        REQUIRE(StandaloneApp::save_persisted_state(app_name(), blob));
        run_session(cfg, [](StandaloneApp& app) {
            REQUIRE(app.state().get_value(kGain) == 0.5f);
            REQUIRE(processor_of(app).note.empty());
        });
    }
    SECTION("the plug-in refuses its own saved payload") {
        StatefulProcessor writer;
        state::StateStore store;
        writer.set_state_store(&store);
        writer.define_parameters(store);
        store.set_value(kGain, 0.9f);
        writer.note = "refuse";
        REQUIRE(StandaloneApp::save_persisted_state(app_name(),
                                                    plugin_state_io::serialize(store, writer)));
        run_session(cfg, [](StandaloneApp& app) {
            // Parameters are rolled back with the payload, so the app is clean.
            REQUIRE(app.state().get_value(kGain) == 0.5f);
            REQUIRE(processor_of(app).note.empty());
        });
    }
}

TEST_CASE("standalone leaves the plug-in alone when persistence is off",
          "[format][standalone][state]") {
    ScopedSettings settings;
    REQUIRE(StandaloneApp::save_persisted_state(app_name(), [] {
        StatefulProcessor writer;
        state::StateStore store;
        writer.set_state_store(&store);
        writer.define_parameters(store);
        store.set_value(kGain, 0.9f);
        return plugin_state_io::serialize(store, writer);
    }()));
    const auto saved = StandaloneApp::load_persisted_state(app_name());

    auto cfg = persisting_config();
    SECTION("persist_plugin_state is false") {
        cfg.persist_plugin_state = false;
    }
    SECTION("persist_settings is false") {
        cfg.persist_settings = false;
    }
    SECTION("the launch is headless") {
        cfg.headless = true;
        cfg.screenshot_path = "pulp-standalone-state-test.png";
    }

    run_session(cfg, [&](StandaloneApp& app) {
        REQUIRE_FALSE(app.plugin_state_persistence_active());
        REQUIRE(app.state().get_value(kGain) == 0.5f);
        app.state().set_value(kGain, 0.1f);
    });
    // Quitting did not overwrite what was saved.
    REQUIRE(StandaloneApp::load_persisted_state(app_name()) == saved);
}

TEST_CASE("standalone saves a change after it has been quiet, and not before",
          "[format][standalone][state]") {
    ScopedSettings settings;
    auto cfg = persisting_config();
    cfg.state_save_debounce_ms = 1500;

    StandaloneApp app(&make_processor);
    install_fake_audio(app);
    app.set_config(cfg);
    REQUIRE(app.start());

    const auto t0 = std::chrono::steady_clock::now();

    SECTION("a change is written once the debounce has passed") {
        app.state().set_value(kGain, 0.7f);
        app.service_state_persistence(t0); // noticed
        REQUIRE(app.plugin_state_save_count() == 0);
        app.service_state_persistence(t0 + 1000ms); // still inside the debounce
        REQUIRE(app.plugin_state_save_count() == 0);
        app.service_state_persistence(t0 + 1700ms); // quiet long enough
        REQUIRE(app.plugin_state_save_count() == 1);
        REQUIRE(StandaloneApp::load_persisted_state(app_name()).has_value());

        // Nothing changed since: no more writes.
        app.service_state_persistence(t0 + 5000ms);
        app.service_state_persistence(t0 + 9000ms);
        REQUIRE(app.plugin_state_save_count() == 1);
    }

    SECTION("a change that keeps changing waits, but not forever") {
        // Changing every half second for twelve seconds: the debounce never
        // elapses, so the ten second cap is what writes it, once.
        for (int i = 0; i < 24; ++i) {
            app.state().set_value(kGain, 0.3f + 0.01f * static_cast<float>(i));
            app.service_state_persistence(t0 + std::chrono::milliseconds(i * 500));
            if (i < 19)
                REQUIRE(app.plugin_state_save_count() == 0);
        }
        REQUIRE(app.plugin_state_save_count() == 1);
    }

    SECTION("a meter moving is not a change") {
        for (int i = 0; i < 20; ++i) {
            app.state().set_value(kMeter, 0.05f * static_cast<float>(i));
            app.service_state_persistence(t0 + std::chrono::milliseconds(i * 500));
        }
        app.service_state_persistence(t0 + 20s);
        REQUIRE(app.plugin_state_save_count() == 0);
    }

    SECTION("state the plug-in flags as changed is saved") {
        processor_of(app).note = "loaded a sample";
        processor_of(app).flag_state_dirty();
        app.service_state_persistence(t0);
        app.service_state_persistence(t0 + 2s);
        REQUIRE(app.plugin_state_save_count() == 1);
    }

    app.stop();
}

TEST_CASE("standalone writes nothing on quit when nothing changed", "[format][standalone][state]") {
    ScopedSettings settings;
    run_session(persisting_config(), [](StandaloneApp& app) {
        // Only a meter moves.
        app.state().set_value(kMeter, 0.9f);
    });
    REQUIRE_FALSE(StandaloneApp::load_persisted_state(app_name()).has_value());
}
