// test_standalone_settings_model.cpp: the Audio/MIDI settings rules shared by
// the view SettingsPanel and the native settings window.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/null_audio_device.hpp>
#include <pulp/format/detail/standalone_settings_window.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/standalone_settings_model.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace pulp;
using pulp::format::StandaloneApp;
using pulp::format::StandaloneConfig;
using pulp::format::StandaloneSettingsModel;

namespace {

class FakeAudio final : public audio::AudioSystem {
  public:
    std::vector<audio::DeviceInfo> devices;
    std::vector<audio::DeviceInfo> enumerate_devices() override {
        return devices;
    }
    std::unique_ptr<audio::AudioDevice> create_device(const std::string&) override {
        return {};
    }
    audio::DeviceInfo default_output_device() override {
        return {};
    }
    audio::DeviceInfo default_input_device() override {
        return {};
    }
};

class FakeMidi final : public midi::MidiSystem {
  public:
    std::vector<midi::MidiPortInfo> ports;
    std::vector<midi::MidiPortInfo> enumerate_inputs() override {
        return ports;
    }
    std::vector<midi::MidiPortInfo> enumerate_outputs() override {
        return {};
    }
    std::unique_ptr<midi::MidiInput> create_input() override {
        return {};
    }
    std::unique_ptr<midi::MidiOutput> create_output() override {
        return {};
    }
};

audio::DeviceInfo device(std::string id, std::string name, int in, int out, bool def_out = false,
                         bool def_in = false) {
    audio::DeviceInfo d;
    d.id = std::move(id);
    d.name = std::move(name);
    d.max_input_channels = in;
    d.max_output_channels = out;
    d.sample_rates = {44100, 48000, 96000};
    d.buffer_sizes = {64, 128, 256, 512};
    d.is_default_output = def_out;
    d.is_default_input = def_in;
    return d;
}

midi::MidiPortInfo port(std::string id, std::string name) {
    midi::MidiPortInfo p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.is_input = true;
    return p;
}

struct Rig {
    FakeAudio audio;
    FakeMidi midi;
    StandaloneSettingsModel model;
    Rig() {
        audio.devices = {device("spk", "Speakers", 0, 2, true),
                         device("usb", "USB Interface", 2, 2, false, true)};
        midi.ports = {port("m1", "Keys"), port("m2", "Pads")};
    }
    void load(const StandaloneConfig& cfg = {}) {
        model.set_config(cfg);
        model.refresh(&audio, &midi);
    }
};

} // namespace

TEST_CASE("constrain_sample_rates and constrain_buffer_sizes filter by the allowed lists",
          "[standalone][settings-model]") {
    using format::constrain_buffer_sizes;
    using format::constrain_sample_rates;
    REQUIRE(constrain_sample_rates({44100, 48000, 96000}, {}) ==
            std::vector<double>{44100, 48000, 96000});
    REQUIRE(constrain_sample_rates({44100, 48000, 96000}, {96000, 48000}) ==
            std::vector<double>{48000, 96000});
    // Nothing offered is allowed: the allowed list itself is offered.
    REQUIRE(constrain_sample_rates({44100}, {48000}) == std::vector<double>{48000});
    REQUIRE(constrain_buffer_sizes({64, 128, 256}, {256, 64}) == std::vector<int>{64, 256});
    REQUIRE(constrain_buffer_sizes({64}, {512}) == std::vector<int>{512});
}

TEST_CASE("the model lists the enumerated devices and follows the default first",
          "[standalone][settings-model]") {
    Rig rig;
    rig.load();
    const auto& out = rig.model.output_items();
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == "System Default (follow)");
    REQUIRE(out[1] == "Speakers (Default)");
    REQUIRE(out[2] == "USB Interface");
    // Audio input is an on/off choice over the system default input, not a
    // device list.
    const auto& in = rig.model.input_items();
    REQUIRE(in.size() == 2);
    REQUIRE(in[0].find("Off") == 0);
    REQUIRE(in[1].find("default input device") != std::string::npos);
    REQUIRE(rig.model.midi_items() == std::vector<std::string>{"None", "Keys", "Pads"});
}

TEST_CASE("the input row is hidden when the processor has no audio input",
          "[standalone][settings-model]") {
    Rig rig;
    StandaloneConfig cfg;
    cfg.supports_audio_input = false;
    cfg.input_channels = 2;
    rig.load(cfg);
    REQUIRE_FALSE(rig.model.input_visible());
    REQUIRE(rig.model.input_items().size() == 1);
    // Whatever the list says, the applied config asks for no input.
    rig.model.select_input(1);
    REQUIRE(rig.model.config().input_channels == 0);

    StandaloneConfig effect;
    rig.load(effect);
    REQUIRE(rig.model.input_visible());
}

TEST_CASE("allowed sample rates and buffer sizes restrict what the lists offer",
          "[standalone][settings-model]") {
    Rig rig;
    StandaloneConfig cfg;
    cfg.allowed_sample_rates = {48000, 96000};
    cfg.allowed_buffer_sizes = {128, 256};
    cfg.sample_rate = 44100; // outside the list: resolves to the first offered
    cfg.buffer_size = 256;
    rig.load(cfg);
    REQUIRE(rig.model.sample_rates() == std::vector<double>{48000, 96000});
    REQUIRE(rig.model.buffer_sizes() == std::vector<int>{128, 256});
    REQUIRE(rig.model.sample_rate_selected() == 0);
    REQUIRE(rig.model.buffer_size_selected() == 1);

    // Selecting a pinned device keeps the restriction.
    rig.model.select_output(2);
    REQUIRE(rig.model.sample_rates() == std::vector<double>{48000, 96000});
    const auto applied = rig.model.config();
    REQUIRE(applied.audio_device_id == "usb");
    REQUIRE(applied.sample_rate == 48000);
    REQUIRE(applied.buffer_size == 256);
}

TEST_CASE("a device change carries the chosen rate and buffer when it still offers them",
          "[standalone][settings-model]") {
    Rig rig;
    StandaloneConfig cfg;
    cfg.sample_rate = 48000;
    cfg.buffer_size = 128;
    rig.load(cfg);
    rig.model.select_sample_rate(2); // 88200 in the generic follow-default list
    rig.model.select_output(2);      // USB: 44100/48000/96000
    // 88200 is not offered there: falls back to the first rate.
    REQUIRE(rig.model.config().sample_rate == 44100);
    rig.model.select_sample_rate(2); // 96000
    rig.model.select_output(1);      // Speakers also offers 96000
    REQUIRE(rig.model.config().sample_rate == 96000);
}

TEST_CASE("buffer items and the latency text track the selected rate",
          "[standalone][settings-model]") {
    Rig rig;
    StandaloneConfig cfg;
    cfg.sample_rate = 48000;
    cfg.buffer_size = 256;
    rig.load(cfg);
    rig.model.select_output(1);
    REQUIRE(rig.model.latency_ms() > 5.3);
    REQUIRE(rig.model.latency_ms() < 5.4);
    REQUIRE(rig.model.latency_text() == "Latency: 5.3 ms");
    REQUIRE(rig.model.buffer_size_items()[2] == "256 samples (5.3 ms)");
    rig.model.select_sample_rate(0); // 44100 Hz
    REQUIRE(rig.model.buffer_size_items()[2] == "256 samples (5.8 ms)");
    REQUIRE(format::buffer_latency_ms(0.0, 256) == 0.0);
}

TEST_CASE("the MIDI list offers None, and the selection round-trips through the config",
          "[standalone][settings-model]") {
    Rig rig;
    rig.load();
    // An empty id opens the first port, so that is the row it selects.
    REQUIRE(rig.model.midi_selected() == 1);
    rig.model.select_midi(0);
    REQUIRE(rig.model.config().midi_input_id == StandaloneConfig::kNoMidiInput);
    rig.model.select_midi(2);
    REQUIRE(rig.model.config().midi_input_id == "m2");

    rig.load(rig.model.config());
    REQUIRE(rig.model.midi_selected() == 2);

    StandaloneConfig none;
    none.midi_input_id = StandaloneConfig::kNoMidiInput;
    rig.load(none);
    REQUIRE(rig.model.midi_selected() == 0);

    // The view panel's variant has no None row.
    StandaloneSettingsModel::Options options;
    options.midi_includes_none = false;
    StandaloneSettingsModel panel{options};
    panel.set_config({});
    panel.refresh(&rig.audio, &rig.midi);
    REQUIRE(panel.midi_items() == std::vector<std::string>{"Keys", "Pads"});
    panel.select_midi(1);
    REQUIRE(panel.config().midi_input_id == "m2");
}

TEST_CASE("missing audio and MIDI systems leave only the follow-default rows",
          "[standalone][settings-model]") {
    StandaloneSettingsModel model;
    model.set_config({});
    model.refresh(nullptr, nullptr);
    REQUIRE(model.output_items().size() == 1);
    REQUIRE(model.midi_items() == std::vector<std::string>{"None"});
    // Rates and buffers still come from the generic lists.
    REQUIRE_FALSE(model.sample_rates().empty());
}

TEST_CASE("the null device is listed from the null audio system and never saved as an id",
          "[standalone][settings-model]") {
    auto null_system = format::detail::create_null_audio_system();
    StandaloneSettingsModel model;
    model.set_config({});
    model.refresh(null_system.get(), nullptr);
    REQUIRE(model.output_items().size() == 2);
    REQUIRE(model.output_items()[1].find("ull") != std::string::npos);

    model.select_output(1);
    REQUIRE(model.output_selected() == 1);
    REQUIRE(model.config().audio_device_id.empty());
}

TEST_CASE("a model config survives a save and a load",
          "[standalone][settings-model][persistence]") {
    Rig rig;
    StandaloneConfig cfg;
    cfg.persist_settings = true;
    rig.load(cfg);
    rig.model.select_output(2);
    rig.model.select_sample_rate(2);
    rig.model.select_buffer_size(1);
    rig.model.select_midi(0);
    const auto chosen = rig.model.config();

    const std::string app = "pulp-test-settings-model-roundtrip";
    REQUIRE(StandaloneApp::save_persisted_config(app, chosen));
    const auto restored = StandaloneApp::load_persisted_config(app, StandaloneConfig{});
    REQUIRE(restored.audio_device_id == "usb");
    REQUIRE(restored.sample_rate == 96000);
    REQUIRE(restored.buffer_size == 128);
    REQUIRE(restored.midi_input_id == StandaloneConfig::kNoMidiInput);

    // The next launch shows the same selections.
    rig.load(restored);
    REQUIRE(rig.model.output_selected() == 2);
    REQUIRE(rig.model.sample_rate_selected() == 2);
    REQUIRE(rig.model.buffer_size_selected() == 1);
    REQUIRE(rig.model.midi_selected() == 0);
}

// -- start() and the MIDI input selection ------------------------------------

namespace pulp::format {
struct StandaloneMidiTestAccess {
    static void install(StandaloneApp& app, std::function<std::unique_ptr<midi::MidiSystem>()> midi,
                        std::function<std::unique_ptr<audio::AudioSystem>()> audio) {
        app.midi_system_factory_ = std::move(midi);
        app.audio_system_factory_ = std::move(audio);
    }
};
} // namespace pulp::format

namespace {

struct OpenLog {
    std::vector<std::string> opened;
};

class LoggingMidiInput final : public midi::MidiInput {
  public:
    explicit LoggingMidiInput(OpenLog& log) : log_(log) {}
    bool open(const std::string& id, midi::MidiInputCallback) override {
        log_.opened.push_back(id);
        open_ = true;
        return true;
    }
    void close() override {
        open_ = false;
    }
    bool is_open() const override {
        return open_;
    }

  private:
    OpenLog& log_;
    bool open_ = false;
};

class LoggingMidi final : public midi::MidiSystem {
  public:
    explicit LoggingMidi(OpenLog& log) : log_(log) {}
    std::vector<midi::MidiPortInfo> enumerate_inputs() override {
        return {port("m1", "Keys"), port("m2", "Pads")};
    }
    std::vector<midi::MidiPortInfo> enumerate_outputs() override {
        return {};
    }
    std::unique_ptr<midi::MidiInput> create_input() override {
        return std::make_unique<LoggingMidiInput>(log_);
    }
    std::unique_ptr<midi::MidiOutput> create_output() override {
        return {};
    }

  private:
    OpenLog& log_;
};

class MidiProcessor final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "Settings Model Midi";
        d.accepts_midi = true;
        d.output_buses = {{"Audio Out", 2}};
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        out.clear();
    }
};

std::unique_ptr<format::Processor> make_midi_processor() {
    return std::make_unique<MidiProcessor>();
}

std::vector<std::string> midi_ports_opened_by_start(const std::string& midi_input_id) {
    OpenLog log;
    format::StandaloneApp app(&make_midi_processor);
    format::StandaloneMidiTestAccess::install(
        app, [&log] { return std::make_unique<LoggingMidi>(log); },
        [] { return format::detail::create_null_audio_system(); });
    StandaloneConfig cfg;
    cfg.persist_settings = false;
    cfg.midi_input_id = midi_input_id;
    app.set_config(cfg);
    REQUIRE(app.start());
    app.stop();
    return log.opened;
}

} // namespace

TEST_CASE(
    "start() opens the chosen MIDI port, the first port for an empty id, and none for kNoMidiInput",
    "[standalone][settings-model][midi]") {
    REQUIRE(midi_ports_opened_by_start("m2") == std::vector<std::string>{"m2"});
    REQUIRE(midi_ports_opened_by_start("") == std::vector<std::string>{"m1"});
    REQUIRE(midi_ports_opened_by_start(StandaloneConfig::kNoMidiInput).empty());
}

// -- config() follows the rows the user sees ---------------------------------

TEST_CASE("a pinned output that has gone applies the default, not the missing id",
          "[standalone][settings-model][fallback]") {
    Rig rig;
    StandaloneConfig cfg;
    cfg.audio_device_id = "usb";
    rig.load(cfg);
    REQUIRE(rig.model.output_selected() == 2);
    REQUIRE(rig.model.config().audio_device_id == "usb");

    rig.audio.devices.pop_back(); // the interface was unplugged
    rig.model.refresh(&rig.audio, &rig.midi);
    REQUIRE(rig.model.output_selected() == 0);
    REQUIRE(rig.model.config().audio_device_id.empty());
}

TEST_CASE("input that has gone applies as off", "[standalone][settings-model][fallback]") {
    Rig rig;
    StandaloneConfig cfg;
    cfg.input_channels = 2;
    rig.load(cfg);
    REQUIRE(rig.model.input_selected() == 1);
    REQUIRE(rig.model.config().input_channels == 2);

    rig.audio.devices = {device("spk", "Speakers", 0, 2, true)}; // no input device left
    rig.model.refresh(&rig.audio, &rig.midi);
    REQUIRE(rig.model.input_selected() == 0);
    REQUIRE(rig.model.config().input_channels == 0);
}

TEST_CASE("a MIDI port that has gone applies the row shown",
          "[standalone][settings-model][fallback]") {
    Rig rig;
    StandaloneConfig cfg;
    cfg.midi_input_id = "m2";
    rig.load(cfg);
    REQUIRE(rig.model.config().midi_input_id == "m2");

    rig.midi.ports = {port("m1", "Keys")};
    rig.model.refresh(&rig.audio, &rig.midi);
    REQUIRE(rig.model.midi_selected() == 0);
    REQUIRE(rig.model.config().midi_input_id == StandaloneConfig::kNoMidiInput);

    // The view panel's variant shows the first port instead.
    StandaloneSettingsModel::Options options;
    options.midi_includes_none = false;
    StandaloneSettingsModel panel{options};
    panel.set_config(cfg);
    panel.refresh(&rig.audio, &rig.midi);
    REQUIRE(panel.midi_selected() == 0);
    REQUIRE(panel.config().midi_input_id == "m1");

    // No ports and nothing chosen stays "nothing chosen", so a keyboard plugged
    // in later is still picked up.
    rig.midi.ports.clear();
    rig.load({});
    REQUIRE(rig.model.config().midi_input_id.empty());
    panel.set_config({});
    panel.refresh(&rig.audio, &rig.midi);
    REQUIRE(panel.config().midi_input_id.empty());
}

TEST_CASE("an empty MIDI id follows the first port without pinning it",
          "[standalone][settings-model][midi]") {
    Rig rig;
    StandaloneConfig cfg;
    cfg.midi_input_id.clear();
    rig.load(cfg);
    REQUIRE(rig.model.midi_selected() == 1);
    REQUIRE(rig.model.config().midi_input_id.empty());

    // An explicit row selection changes the dynamic choice into a pinned id.
    rig.model.select_midi(2);
    REQUIRE(rig.model.config().midi_input_id == "m2");

    StandaloneSettingsModel::Options options;
    options.midi_includes_none = false;
    StandaloneSettingsModel panel{options};
    panel.set_config(cfg);
    panel.refresh(&rig.audio, &rig.midi);
    REQUIRE(panel.midi_selected() == 0);
    REQUIRE(panel.config().midi_input_id.empty());
    panel.select_midi(1);
    REQUIRE(panel.config().midi_input_id == "m2");
}

TEST_CASE("two inputs with the same channel count offer one on/off control, not a picker",
          "[standalone][settings-model][input]") {
    Rig rig;
    rig.audio.devices = {device("spk", "Speakers", 0, 2, true),
                         device("in-a", "Mic A", 2, 0, false, true), device("in-b", "Mic B", 2, 0)};
    rig.load();
    // One row per state: nothing in the list names a device that could not be routed.
    REQUIRE(rig.model.input_items().size() == 2);
    REQUIRE(std::string(StandaloneSettingsModel::input_label()).find("default") !=
            std::string::npos);
    REQUIRE(rig.model.config().input_channels == 0);
    rig.model.select_input(1);
    REQUIRE(rig.model.config().input_channels == 2);

    // The choice survives the config round trip instead of resetting.
    const auto chosen = rig.model.config();
    rig.load(chosen);
    REQUIRE(rig.model.input_selected() == 1);
    REQUIRE(rig.model.config().input_channels == 2);
}

TEST_CASE("devices and ports with equal names keep their own rows",
          "[standalone][settings-model][duplicate-names]") {
    Rig rig;
    rig.audio.devices = {device("a", "Interface", 0, 2), device("b", "Interface", 0, 2)};
    rig.midi.ports = {port("p1", "None"), port("p2", "None")};
    rig.load();
    REQUIRE(rig.model.output_items().size() == 3);
    rig.model.select_output(2);
    REQUIRE(rig.model.config().audio_device_id == "b");
    rig.model.select_output(1);
    REQUIRE(rig.model.config().audio_device_id == "a");

    // A port literally named "None" is a port, not the None row.
    REQUIRE(rig.model.midi_items() == std::vector<std::string>{"None", "None", "None"});
    rig.model.select_midi(0);
    REQUIRE(rig.model.config().midi_input_id == StandaloneConfig::kNoMidiInput);
    rig.model.select_midi(1);
    REQUIRE(rig.model.config().midi_input_id == "p1");
    rig.model.select_midi(2);
    REQUIRE(rig.model.config().midi_input_id == "p2");
    rig.load(rig.model.config());
    REQUIRE(rig.model.midi_selected() == 2);
}

// -- Apply and the app's lifetime --------------------------------------------

namespace {

// Wraps the null device so open() can be made to fail on demand.
struct OpenGate {
    bool fail = false;
};

class GatedDevice final : public audio::AudioDevice {
  public:
    GatedDevice(std::unique_ptr<audio::AudioDevice> inner, OpenGate& gate)
        : inner_(std::move(inner)), gate_(gate) {}
    bool open(const audio::DeviceConfig& c) override {
        return !gate_.fail && inner_->open(c);
    }
    void close() override {
        inner_->close();
    }
    bool start(audio::AudioCallback cb) override {
        return inner_->start(std::move(cb));
    }
    void stop() override {
        inner_->stop();
    }
    bool is_open() const override {
        return inner_->is_open();
    }
    bool is_running() const override {
        return inner_->is_running();
    }
    audio::DeviceInfo info() const override {
        return inner_->info();
    }
    double sample_rate() const override {
        return inner_->sample_rate();
    }
    int buffer_size() const override {
        return inner_->buffer_size();
    }

  private:
    std::unique_ptr<audio::AudioDevice> inner_;
    OpenGate& gate_;
};

class GatedSystem final : public audio::AudioSystem {
  public:
    explicit GatedSystem(OpenGate& gate)
        : inner_(format::detail::create_null_audio_system()), gate_(gate) {}
    std::vector<audio::DeviceInfo> enumerate_devices() override {
        return inner_->enumerate_devices();
    }
    std::unique_ptr<audio::AudioDevice> create_device(const std::string& id) override {
        return std::make_unique<GatedDevice>(inner_->create_device(id), gate_);
    }
    audio::DeviceInfo default_output_device() override {
        return inner_->default_output_device();
    }
    audio::DeviceInfo default_input_device() override {
        return inner_->default_input_device();
    }

  private:
    std::unique_ptr<audio::AudioSystem> inner_;
    OpenGate& gate_;
};

StandaloneConfig apply_test_config() {
    StandaloneConfig cfg;
    cfg.persist_settings = false;
    cfg.buffer_size = 256;
    return cfg;
}

} // namespace

TEST_CASE("a failed Apply keeps the last working config and audio running",
          "[standalone][settings-model][apply]") {
    OpenGate gate;
    format::StandaloneApp app(&make_midi_processor);
    format::StandaloneMidiTestAccess::install(
        app, [] { return std::make_unique<FakeMidi>(); },
        [&gate] { return std::make_unique<GatedSystem>(gate); });
    app.set_config(apply_test_config());
    REQUIRE(app.start());

    auto bad = apply_test_config();
    bad.buffer_size = 0; // the device refuses it
    REQUIRE_FALSE(app.apply_config(bad));
    REQUIRE(app.is_running());
    REQUIRE(app.config().buffer_size == 256);
    app.stop();
}

TEST_CASE("after a failed Apply that could not restore, the next valid Apply resumes audio",
          "[standalone][settings-model][apply]") {
    OpenGate gate;
    format::StandaloneApp app(&make_midi_processor);
    format::StandaloneMidiTestAccess::install(
        app, [] { return std::make_unique<FakeMidi>(); },
        [&gate] { return std::make_unique<GatedSystem>(gate); });
    app.set_config(apply_test_config());
    REQUIRE(app.start());

    gate.fail = true; // every open fails, so neither the change nor the restore works
    auto next = apply_test_config();
    next.buffer_size = 512;
    REQUIRE_FALSE(app.apply_config(next));
    REQUIRE_FALSE(app.is_running());

    gate.fail = false;
    REQUIRE(app.apply_config(next));
    REQUIRE(app.is_running());
    REQUIRE(app.config().buffer_size == 512);
    app.stop();
}

TEST_CASE("stop() and destruction close only this app's settings window",
          "[standalone][settings-model][lifetime]") {
    int closed_a = 0;
    int closed_b = 0;
    format::StandaloneApp app_b(&make_midi_processor);
    format::detail::register_standalone_settings_window_closer(&app_b, [&] { ++closed_b; });
    {
        format::StandaloneApp app_a(&make_midi_processor);
        format::detail::register_standalone_settings_window_closer(&app_a, [&] { ++closed_a; });
        app_a.stop();
        REQUIRE(closed_a == 1);
        REQUIRE(closed_b == 0);

        // Destruction closes a window registered after stop().
        format::detail::register_standalone_settings_window_closer(&app_a, [&] { ++closed_a; });
    }
    REQUIRE(closed_a == 2);
    REQUIRE(closed_b == 0);
    app_b.stop();
    REQUIRE(closed_b == 1);
}

TEST_CASE("choosing None with no ports keeps the no-MIDI sentinel",
          "[standalone][settings-model][midi]") {
    Rig rig;
    rig.midi.ports.clear();
    rig.load({});
    REQUIRE(rig.model.config().midi_input_id.empty());
    rig.model.select_midi(0);
    REQUIRE(rig.model.config().midi_input_id == StandaloneConfig::kNoMidiInput);
}

TEST_CASE("the no-None variant keeps an existing no-MIDI choice through an unrelated Apply",
          "[standalone][settings-model][midi]") {
    Rig rig;
    StandaloneSettingsModel::Options options;
    options.midi_includes_none = false;
    StandaloneSettingsModel panel{options};
    StandaloneConfig cfg;
    cfg.midi_input_id = StandaloneConfig::kNoMidiInput;
    panel.set_config(cfg);
    panel.refresh(&rig.audio, &rig.midi);
    REQUIRE(panel.midi_selected() == -1);
    panel.select_sample_rate(0); // unrelated change
    REQUIRE(panel.config().midi_input_id == StandaloneConfig::kNoMidiInput);
    panel.select_midi(1);
    REQUIRE(panel.config().midi_input_id == "m2");
}
