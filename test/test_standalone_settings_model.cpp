// test_standalone_settings_model.cpp: the Audio/MIDI settings rules shared by
// the view SettingsPanel and the native settings window.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/null_audio_device.hpp>
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
    // Only devices with inputs feed the input list.
    const auto& in = rig.model.input_items();
    REQUIRE(in.size() == 2);
    REQUIRE(in[1] == "USB Interface (Default)");
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
