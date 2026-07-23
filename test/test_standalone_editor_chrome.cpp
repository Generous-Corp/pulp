#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/format/detail/delayed_action.hpp>
#include <pulp/format/detail/standalone_editor_chrome.hpp>
#include <pulp/format/standalone_settings.hpp>
#include <pulp/format/detail/standalone_audio_probe_json.hpp>
#include <pulp/format/detail/standalone_audio_scope_json.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace pulp::format;
using namespace pulp::format::detail;
using namespace pulp::view;

namespace {

struct ScopedEnv {
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* prev = std::getenv(name_.c_str())) {
            prev_ = std::string(prev);
            had_prev_ = true;
        }
    }

    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), /*overwrite=*/1);
#endif
    }

    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }

    ~ScopedEnv() {
        if (had_prev_) set(prev_);
        else unset();
    }

private:
    std::string name_;
    std::string prev_;
    bool had_prev_ = false;
};

class StubWindowHost final : public WindowHost {
public:
    ContentSize content_size_{640, 360};
    std::function<void()> idle_callback_;
    ResizeCallback resize_callback_;
    float design_width_ = 0.0f;
    float design_height_ = 0.0f;
    float aspect_ratio_ = 0.0f;
    std::vector<ContentSize> content_size_requests_;
    int repaint_calls_ = 0;

    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override { ++repaint_calls_; }
    ContentSize get_content_size() const override { return content_size_; }
    void set_idle_callback(std::function<void()> cb) override {
        idle_callback_ = std::move(cb);
    }
    void set_resize_callback(ResizeCallback cb) override {
        resize_callback_ = std::move(cb);
    }
    void set_design_viewport(float design_w, float design_h) override {
        design_width_ = design_w;
        design_height_ = design_h;
    }
    void set_fixed_aspect_ratio(float ratio) override {
        aspect_ratio_ = ratio;
    }
    void request_content_size(float width, float height) override {
        content_size_requests_.push_back({
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
        });
    }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

class DefaultWindowHost final : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override {}
    std::vector<uint8_t> capture_png() override { return {0x89, 'P', 'N', 'G'}; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

struct StubScriptedUi {
    int poll_calls_ = 0;
    std::function<void()> repaint_callback_;

    void poll() { ++poll_calls_; }
    void set_repaint_callback(std::function<void()> cb) {
        repaint_callback_ = std::move(cb);
    }
};

struct StubSettingsPoller {
    int poll_calls_ = 0;
    void poll() { ++poll_calls_; }
};

struct StubBridge {
    std::vector<WindowHost::ContentSize> resize_calls_;

    void resize(uint32_t width, uint32_t height) {
        resize_calls_.push_back({width, height});
    }
};

int counted_null_processor_factory_calls = 0;

std::unique_ptr<Processor> counted_null_processor_factory() {
    ++counted_null_processor_factory_calls;
    return nullptr;
}

class StubAudioSystem final : public pulp::audio::AudioSystem {
public:
    std::vector<pulp::audio::DeviceInfo> devices;
    int enumerate_calls = 0;

    std::vector<pulp::audio::DeviceInfo> enumerate_devices() override {
        ++enumerate_calls;
        return devices;
    }

    std::unique_ptr<pulp::audio::AudioDevice> create_device(const std::string&) override {
        return nullptr;
    }

    pulp::audio::DeviceInfo default_output_device() override {
        for (const auto& device : devices) {
            if (device.is_default_output) return device;
        }
        return devices.empty() ? pulp::audio::DeviceInfo{} : devices.front();
    }

    pulp::audio::DeviceInfo default_input_device() override {
        for (const auto& device : devices) {
            if (device.is_default_input) return device;
        }
        return devices.empty() ? pulp::audio::DeviceInfo{} : devices.front();
    }
};

class StubMidiSystem final : public pulp::midi::MidiSystem {
public:
    std::vector<pulp::midi::MidiPortInfo> inputs;
    int enumerate_calls = 0;

    std::vector<pulp::midi::MidiPortInfo> enumerate_inputs() override {
        ++enumerate_calls;
        return inputs;
    }

    std::vector<pulp::midi::MidiPortInfo> enumerate_outputs() override {
        return {};
    }

    std::unique_ptr<pulp::midi::MidiInput> create_input() override {
        return nullptr;
    }

    std::unique_ptr<pulp::midi::MidiOutput> create_output() override {
        return nullptr;
    }

    void set_port_change_callback(PortChangeCallback cb) override {
        port_change_callback_ = std::move(cb);
    }

    void fire_port_change() {
        if (port_change_callback_) port_change_callback_();
    }

private:
    PortChangeCallback port_change_callback_;
};

template <typename T>
void collect_descendants(View& root, std::vector<T*>& out) {
    if (auto* match = dynamic_cast<T*>(&root)) out.push_back(match);
    for (size_t i = 0; i < root.child_count(); ++i) {
        collect_descendants(*root.child_at(i), out);
    }
}

template <typename T>
std::vector<T*> descendants(View& root) {
    std::vector<T*> out;
    collect_descendants(root, out);
    return out;
}

TabPanel& settings_tabs(SettingsPanel& panel) {
    // The panel hosts a "Done" header plus the Audio/MIDI/<plugin> TabPanel — find the
    // TabPanel among the children rather than assuming it's the only child.
    for (int i = 0; i < panel.child_count(); ++i)
        if (auto* tabs = dynamic_cast<TabPanel*>(panel.child_at(static_cast<size_t>(i))))
            return *tabs;
    REQUIRE(false);  // no TabPanel found
    static TabPanel never; return never;  // unreachable; satisfies the return type
}

} // namespace

TEST_CASE("Standalone settings callbacks rebind after successful apply",
          "[standalone][chrome]") {
    SettingsPanel panel;
    bool apply_called = false;
    bool rebind_called = false;
    bool signal_changed = false;
    bool file_loaded = false;
    bool transport_called = false;

    auto callbacks = make_standalone_settings_callbacks(
        panel,
        StandaloneSettingsActions{
            .apply_config = [&](const StandaloneConfig&) {
                apply_called = true;
                return true;
            },
            .rebind_after_apply = [&](SettingsPanel& rebound_panel) {
                rebind_called = (&rebound_panel == &panel);
            },
            .on_test_signal_changed = [&](const TestSignalConfig&) {
                signal_changed = true;
            },
            .on_file_load = [&](const std::string& path) {
                file_loaded = (path == "/tmp/test.wav");
            },
            .on_file_transport = [&](bool play, bool loop) {
                transport_called = play && !loop;
            },
        });

    callbacks.on_config_apply(StandaloneConfig{});
    callbacks.on_test_signal_changed(TestSignalConfig{});
    callbacks.on_file_load("/tmp/test.wav");
    callbacks.on_file_transport(true, false);

    REQUIRE(apply_called);
    REQUIRE(rebind_called);
    REQUIRE(signal_changed);
    REQUIRE(file_loaded);
    REQUIRE(transport_called);
}

TEST_CASE("Standalone settings callbacks skip rebind when apply fails",
          "[standalone][chrome]") {
    SettingsPanel panel;
    bool rebind_called = false;

    auto callbacks = make_standalone_settings_callbacks(
        panel,
        StandaloneSettingsActions{
            .apply_config = [](const StandaloneConfig&) { return false; },
            .rebind_after_apply = [&](SettingsPanel&) { rebind_called = true; },
        });

    callbacks.on_config_apply(StandaloneConfig{});
    REQUIRE_FALSE(rebind_called);
}

TEST_CASE("StandaloneApp apply_config updates idle configuration without starting audio",
          "[standalone]") {
    counted_null_processor_factory_calls = 0;
    StandaloneApp app(counted_null_processor_factory);

    StandaloneConfig cfg;
    cfg.audio_device_id = "BuiltInOut";
    cfg.midi_input_id = "Keyboard";
    cfg.sample_rate = 96000.0;
    cfg.buffer_size = 128;
    cfg.input_channels = 1;
    cfg.output_channels = 2;
    cfg.show_settings_tab = false;

    REQUIRE(app.apply_config(cfg));
    REQUIRE_FALSE(app.is_running());
    REQUIRE(counted_null_processor_factory_calls == 0);
    REQUIRE(app.config().audio_device_id == "BuiltInOut");
    REQUIRE(app.config().midi_input_id == "Keyboard");
    REQUIRE(app.config().sample_rate == 96000.0);
    REQUIRE(app.config().buffer_size == 128);
    REQUIRE(app.config().input_channels == 1);
    REQUIRE(app.config().output_channels == 2);
    REQUIRE_FALSE(app.config().show_settings_tab);
}

TEST_CASE("StandaloneApp rejects headless editor runs without screenshot before startup",
          "[standalone]") {
    counted_null_processor_factory_calls = 0;
    StandaloneApp app(counted_null_processor_factory);

    StandaloneConfig cfg;
    cfg.headless = true;
    cfg.screenshot_path.clear();
    app.set_config(cfg);

    REQUIRE_FALSE(app.run_with_editor(false));
    REQUIRE_FALSE(app.is_running());
    REQUIRE(counted_null_processor_factory_calls == 0);
}

TEST_CASE("SettingsPanel applies audio and MIDI selections",
          "[standalone][settings][issue-493]") {
    StubAudioSystem audio;
    audio.devices = {
        {.id = "builtin-out",
         .name = "Built-in Output",
         .max_output_channels = 2,
         .sample_rates = {44100.0},
         .buffer_sizes = {64},
         .is_default_output = true},
        {.id = "usb-out",
         .name = "USB Output",
         .max_output_channels = 2,
         .sample_rates = {48000.0, 96000.0},
         .buffer_sizes = {128, 256}},
        {.id = "mic-in",
         .name = "USB Mic",
         .max_input_channels = 1,
         .sample_rates = {48000.0},
         .buffer_sizes = {128},
         .is_default_input = true},
    };

    StubMidiSystem midi;
    midi.inputs = {
        {.id = "keys", .name = "Keys", .is_input = true},
        {.id = "pads", .name = "Pads", .is_input = true},
    };

    SettingsPanel panel;
    panel.bind_systems(&audio, &midi);

    StandaloneConfig applied;
    int apply_calls = 0;
    panel.set_callbacks(SettingsPanelCallbacks{
        .on_config_apply = [&](const StandaloneConfig& cfg) {
            applied = cfg;
            ++apply_calls;
        },
    });

    auto& tabs = settings_tabs(panel);
    REQUIRE(tabs.tab_count() == 2);
    auto* audio_tab = tabs.child_at(0);
    auto* midi_tab = tabs.child_at(1);
    REQUIRE(audio_tab != nullptr);
    REQUIRE(midi_tab != nullptr);

    auto combos = descendants<ComboBox>(*audio_tab);
    REQUIRE(combos.size() >= 5);
    auto* output_combo = combos[0];
    auto* input_combo = combos[1];
    auto* sample_rate_combo = combos[2];
    auto* buffer_size_combo = combos[3];

    REQUIRE(output_combo->items().size() == 3);
    REQUIRE(input_combo->items().size() == 2);

    output_combo->set_selected(2);
    REQUIRE(applied.audio_device_id == "usb-out");
    REQUIRE(applied.sample_rate == 48000.0);
    REQUIRE(applied.buffer_size == 256);

    sample_rate_combo->set_selected(1);
    buffer_size_combo->set_selected(1);
    input_combo->set_selected(1);

    REQUIRE(applied.audio_device_id == "usb-out");
    REQUIRE(applied.input_channels == 1);
    REQUIRE(applied.sample_rate == 96000.0);
    REQUIRE(applied.buffer_size == 256);

    auto lists = descendants<ListBox>(*midi_tab);
    REQUIRE(lists.size() == 1);
    REQUIRE(lists[0]->items().size() == 2);
    lists[0]->set_selected(1);

    REQUIRE(applied.midi_input_id == "pads");
    REQUIRE(apply_calls >= 4);
}

TEST_CASE("SettingsPanel set_current_config prefers explicit output over default",
          "[standalone][settings]") {
    StubAudioSystem audio;
    audio.devices = {
        {.id = "builtin-out",
         .name = "Built-in Output",
         .max_output_channels = 2,
         .sample_rates = {44100.0},
         .buffer_sizes = {64},
         .is_default_output = true},
        {.id = "usb-out",
         .name = "USB Output",
         .max_output_channels = 2,
         .sample_rates = {48000.0, 96000.0},
         .buffer_sizes = {128, 256}},
    };

    SettingsPanel panel;
    panel.bind_systems(&audio, nullptr);

    StandaloneConfig cfg;
    cfg.audio_device_id = "usb-out";
    cfg.sample_rate = 96000.0;
    cfg.buffer_size = 256;
    panel.set_current_config(cfg);

    auto& tabs = settings_tabs(panel);
    auto* audio_tab = tabs.child_at(0);
    REQUIRE(audio_tab != nullptr);

    auto combos = descendants<ComboBox>(*audio_tab);
    REQUIRE(combos.size() >= 4);
    REQUIRE(combos[0]->selected() == 2);
    REQUIRE(combos[2]->items().size() == 2);
    REQUIRE(combos[2]->selected() == 1);
    REQUIRE(combos[3]->items().size() == 2);
    REQUIRE(combos[3]->selected() == 1);
}

TEST_CASE("SettingsPanel preserves audio selections across apply rebind",
          "[standalone][settings][audio]") {
    StubAudioSystem audio;
    audio.devices = {
        {.id = "builtin-out",
         .name = "Built-in Output",
         .max_output_channels = 2,
         .sample_rates = {44100.0},
         .buffer_sizes = {64},
         .is_default_output = true},
        {.id = "usb-out",
         .name = "USB Output",
         .max_output_channels = 2,
         .sample_rates = {44100.0, 48000.0},
         .buffer_sizes = {64, 512}},
    };

    SettingsPanel panel;
    panel.bind_systems(&audio, nullptr);

    StandaloneConfig cfg;
    cfg.audio_device_id = "builtin-out";
    cfg.sample_rate = 44100.0;
    cfg.buffer_size = 64;
    panel.set_current_config(cfg);

    StandaloneConfig applied = cfg;
    int apply_calls = 0;
    panel.set_callbacks(SettingsPanelCallbacks{
        .on_config_apply = [&](const StandaloneConfig& next) {
            applied = next;
            ++apply_calls;
        },
    });

    auto& tabs = settings_tabs(panel);
    auto* audio_tab = tabs.child_at(0);
    REQUIRE(audio_tab != nullptr);

    auto combos = descendants<ComboBox>(*audio_tab);
    REQUIRE(combos.size() >= 4);
    auto* output_combo = combos[0];
    auto* sample_rate_combo = combos[2];
    auto* buffer_size_combo = combos[3];

    output_combo->set_selected(2);
    REQUIRE(applied.audio_device_id == "usb-out");
    REQUIRE(applied.sample_rate == 44100.0);
    REQUIRE(applied.buffer_size == 64);

    panel.bind_systems(&audio, nullptr);
    REQUIRE(output_combo->selected() == 2);
    REQUIRE(sample_rate_combo->items().size() == 2);
    REQUIRE(buffer_size_combo->items().size() == 2);

    sample_rate_combo->set_selected(1);
    buffer_size_combo->set_selected(1);
    REQUIRE(applied.sample_rate == 48000.0);
    REQUIRE(applied.buffer_size == 512);

    panel.bind_systems(&audio, nullptr);
    REQUIRE(output_combo->selected() == 2);
    REQUIRE(sample_rate_combo->selected() == 1);
    REQUIRE(buffer_size_combo->selected() == 1);
    REQUIRE(sample_rate_combo->selected_text() == "48000 Hz");

    sample_rate_combo->set_selected(0);
    REQUIRE(applied.sample_rate == 44100.0);
    REQUIRE(apply_calls >= 4);
}

TEST_CASE("SettingsPanel marks audio input unused for instrument-only configs",
          "[standalone][settings][audio]") {
    StubAudioSystem audio;
    audio.devices = {
        {.id = "builtin-out",
         .name = "Built-in Output",
         .max_output_channels = 2,
         .sample_rates = {44100.0, 48000.0},
         .buffer_sizes = {64},
         .is_default_output = true},
        {.id = "mic-in",
         .name = "USB Mic",
         .max_input_channels = 2,
         .sample_rates = {48000.0},
         .buffer_sizes = {64},
         .is_default_input = true},
    };

    SettingsPanel panel;
    panel.bind_systems(&audio, nullptr);

    StandaloneConfig cfg;
    cfg.audio_device_id = "builtin-out";
    cfg.sample_rate = 44100.0;
    cfg.input_channels = 0;
    cfg.supports_audio_input = false;
    panel.set_current_config(cfg);

    auto& tabs = settings_tabs(panel);
    auto* audio_tab = tabs.child_at(0);
    REQUIRE(audio_tab != nullptr);

    auto combos = descendants<ComboBox>(*audio_tab);
    REQUIRE(combos.size() >= 4);
    auto* input_combo = combos[1];
    auto* sample_rate_combo = combos[2];

    REQUIRE(input_combo->items().size() == 1);
    REQUIRE(input_combo->items()[0] == "(Not used by this instrument)");
    REQUIRE(input_combo->selected() == 0);

    StandaloneConfig applied;
    panel.set_callbacks(SettingsPanelCallbacks{
        .on_config_apply = [&](const StandaloneConfig& next) {
            applied = next;
        },
    });

    sample_rate_combo->set_selected(1);
    REQUIRE(applied.sample_rate == 48000.0);
    REQUIRE(applied.input_channels == 0);
    REQUIRE_FALSE(applied.supports_audio_input);
}

TEST_CASE("SettingsPanel constrains audio choices for fixed-rate instruments",
          "[standalone][settings][audio]") {
    StubAudioSystem audio;
    audio.devices = {
        {.id = "builtin-out",
         .name = "Built-in Output",
         .max_output_channels = 2,
         .sample_rates = {44100.0, 48000.0, 96000.0},
         .buffer_sizes = {64, 128, 256},
         .is_default_output = true},
        {.id = "usb-out",
         .name = "USB Output",
         .max_output_channels = 2,
         .sample_rates = {48000.0},
         .buffer_sizes = {64, 128}},
    };

    SettingsPanel panel;
    panel.bind_systems(&audio, nullptr);

    StandaloneConfig cfg;
    cfg.sample_rate = 44100.0;
    cfg.buffer_size = 256;
    cfg.allowed_sample_rates = {48000.0};
    cfg.allowed_buffer_sizes = {64};
    panel.set_current_config(cfg);

    StandaloneConfig applied;
    int apply_calls = 0;
    panel.set_callbacks(SettingsPanelCallbacks{
        .on_config_apply = [&](const StandaloneConfig& next) {
            applied = next;
            ++apply_calls;
        },
    });

    auto& tabs = settings_tabs(panel);
    auto* audio_tab = tabs.child_at(0);
    REQUIRE(audio_tab != nullptr);
    auto combos = descendants<ComboBox>(*audio_tab);
    REQUIRE(combos.size() >= 5);

    REQUIRE(combos[2]->items().size() == 1);
    REQUIRE(combos[2]->items()[0] == "48000 Hz");
    REQUIRE(combos[2]->selected() == 0);
    REQUIRE(combos[3]->items().size() == 1);
    REQUIRE(combos[3]->items()[0].find("64 samples") == 0);
    REQUIRE(combos[3]->selected() == 0);

    combos[0]->set_selected(2);
    REQUIRE(apply_calls == 1);
    REQUIRE(applied.audio_device_id == "usb-out");
    REQUIRE(applied.sample_rate == 48000.0);
    REQUIRE(applied.buffer_size == 64);
}

TEST_CASE("SettingsPanel updates input and output meters from audio bridges",
          "[standalone][settings][audio][meter]") {
    StubAudioSystem audio;
    audio.devices = {
        {.id = "builtin-out",
         .name = "Built-in Output",
         .max_output_channels = 2,
         .sample_rates = {48000.0},
         .buffer_sizes = {64},
         .is_default_output = true},
    };

    SettingsPanel panel;
    panel.bind_systems(&audio, nullptr);

    AudioBridge input_bridge;
    AudioBridge output_bridge;
    panel.set_input_meter_bridge(&input_bridge);
    panel.set_output_meter_bridge(&output_bridge);

    auto& tabs = settings_tabs(panel);
    auto* audio_tab = tabs.child_at(0);
    REQUIRE(audio_tab != nullptr);
    auto meters = descendants<MultiMeter>(*audio_tab);
    REQUIRE(meters.size() >= 2);
    REQUIRE(meters[0]->layout() == MultiMeter::Layout::horizontal);
    REQUIRE(meters[0]->display_style() == MultiMeter::DisplayStyle::segmented);
    REQUIRE(meters[1]->layout() == MultiMeter::Layout::horizontal);
    REQUIRE(meters[1]->display_style() == MultiMeter::DisplayStyle::segmented);

    MeterData input_data;
    input_data.num_channels = 1;
    input_data.peak[0] = 0.25f;
    input_data.rms[0] = 0.125f;
    input_bridge.push_meter(input_data);

    MeterData output_data;
    output_data.num_channels = 2;
    output_data.peak[0] = 0.5f;
    output_data.peak[1] = 0.75f;
    output_data.rms[0] = 0.25f;
    output_data.rms[1] = 0.5f;
    output_bridge.push_meter(output_data);

    panel.poll();

    REQUIRE(meters[0]->channel_count() == 1);
    REQUIRE(meters[0]->ballistics().channels[0].display_peak > 0.0f);
    REQUIRE(meters[1]->channel_count() == 2);
    REQUIRE(meters[1]->ballistics().channels[0].display_peak > 0.0f);
    REQUIRE(meters[1]->ballistics().channels[1].display_peak > 0.0f);
}

TEST_CASE("SettingsPanel refreshes hotplug lists and test tone callbacks",
          "[standalone][settings][issue-493]") {
    StubAudioSystem audio;
    audio.devices = {
        {.id = "builtin-out",
         .name = "Built-in Output",
         .max_output_channels = 2,
         .sample_rates = {44100.0},
         .buffer_sizes = {64},
         .is_default_output = true},
    };

    StubMidiSystem midi;
    midi.inputs = {{.id = "keys", .name = "Keys", .is_input = true}};

    SettingsPanel panel;
    panel.bind_systems(&audio, &midi);

    auto& tabs = settings_tabs(panel);
    auto* audio_tab = tabs.child_at(0);
    auto* midi_tab = tabs.child_at(1);
    REQUIRE(audio_tab != nullptr);
    REQUIRE(midi_tab != nullptr);

    REQUIRE(audio.enumerate_calls == 1);
    REQUIRE(midi.enumerate_calls == 1);

    audio.devices.push_back({
        .id = "usb-out",
        .name = "USB Output",
        .max_output_channels = 2,
        .sample_rates = {48000.0},
        .buffer_sizes = {128},
    });
    midi.inputs.push_back({.id = "pads", .name = "Pads", .is_input = true});

    audio.fire_device_change();
    midi.fire_port_change();
    panel.poll();

    auto combos = descendants<ComboBox>(*audio_tab);
    auto lists = descendants<ListBox>(*midi_tab);
    // Audio-tab combo order: [0] output, [1] input, [2] rate, [3] buffer,
    // [4] signal type (Sine/Noise), [5] tone frequency. The signal-type combo
    // sits between the buffer and frequency combos, so the frequency combo is
    // index 5.
    REQUIRE(combos.size() >= 6);
    REQUIRE(lists.size() == 1);
    REQUIRE(audio.enumerate_calls == 2);
    REQUIRE(midi.enumerate_calls == 2);
    REQUIRE(combos[0]->items().size() == 3);
    REQUIRE(lists[0]->items().size() == 2);

    TestSignalConfig last_signal;
    int signal_calls = 0;
    panel.set_callbacks(SettingsPanelCallbacks{
        .on_test_signal_changed = [&](const TestSignalConfig& cfg) {
            last_signal = cfg;
            ++signal_calls;
        },
    });

    auto toggles = descendants<Toggle>(*audio_tab);
    REQUIRE(toggles.size() == 1);
    toggles[0]->on_mouse_down({0, 0});

    REQUIRE(signal_calls == 1);
    REQUIRE(last_signal.type == TestSignalType::sine);
    REQUIRE(last_signal.sine_frequency_hz == 440.0f);
    REQUIRE(last_signal.sine_amplitude == 0.5f);

    combos[5]->set_selected(2);   // tone-frequency combo → 880 Hz
    REQUIRE(signal_calls == 2);
    REQUIRE(last_signal.sine_frequency_hz == 880.0f);

    toggles[0]->on_mouse_down({0, 0});
    REQUIRE(signal_calls == 3);
    REQUIRE(last_signal.type == TestSignalType::none);

    combos[5]->set_selected(0);   // tone-frequency combo → 220 Hz
    REQUIRE(signal_calls == 3);

    toggles[0]->on_mouse_down({0, 0});
    REQUIRE(signal_calls == 4);
    REQUIRE(last_signal.type == TestSignalType::sine);
    REQUIRE(last_signal.sine_frequency_hz == 220.0f);
    REQUIRE(last_signal.sine_amplitude == 0.5f);
}

TEST_CASE("SettingsPanel uses fallback rate and buffer choices without output devices",
          "[standalone][settings]") {
    StubAudioSystem audio;
    audio.devices = {
        {.id = "mic-only",
         .name = "Mic Only",
         .max_input_channels = 1,
         .sample_rates = {48000.0},
         .buffer_sizes = {128},
         .is_default_input = true},
    };

    SettingsPanel panel;
    panel.bind_systems(&audio, nullptr);

    StandaloneConfig applied;
    applied.audio_device_id = "stale";
    int apply_calls = 0;
    panel.set_callbacks(SettingsPanelCallbacks{
        .on_config_apply = [&](const StandaloneConfig& cfg) {
            applied = cfg;
            ++apply_calls;
        },
    });

    auto& tabs = settings_tabs(panel);
    auto* audio_tab = tabs.child_at(0);
    REQUIRE(audio_tab != nullptr);

    auto combos = descendants<ComboBox>(*audio_tab);
    REQUIRE(combos.size() >= 4);
    REQUIRE(combos[0]->items().size() == 1);  // "System Default (follow)" entry
    REQUIRE(combos[1]->items().size() == 2);
    REQUIRE(combos[2]->items().size() >= 6);
    REQUIRE(combos[3]->items().size() >= 7);
    REQUIRE(combos[2]->selected() == 1);
    REQUIRE(combos[3]->selected() == 2);

    combos[2]->set_selected(0);
    REQUIRE(apply_calls == 1);
    REQUIRE(applied.audio_device_id.empty());
    REQUIRE(applied.sample_rate == 44100.0);
    REQUIRE(applied.buffer_size == 256);

    combos[3]->set_selected(0);
    REQUIRE(apply_calls == 2);
    REQUIRE(applied.audio_device_id.empty());
    REQUIRE(applied.sample_rate == 44100.0);
    REQUIRE(applied.buffer_size == 64);
}

TEST_CASE("Standalone editor chrome keeps the editor root when settings are hidden",
          "[standalone][chrome]") {
    auto editor_root = std::make_unique<View>();
    auto* editor_ptr = editor_root.get();
    StandaloneConfig config;
    config.show_settings_tab = false;

    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root), config, nullptr, nullptr, nullptr, {});

    REQUIRE(&chrome.window_root() == editor_ptr);
    REQUIRE(chrome.settings_panel() == nullptr);
    REQUIRE(chrome.tab_panel() == nullptr);
    REQUIRE(chrome.extra_window_height() == 0.0f);
    REQUIRE(chrome.chrome_label() == std::string_view("editor-only"));
}

TEST_CASE("Standalone editor chrome wraps editor and settings in a tab panel",
          "[standalone][chrome]") {
    auto editor_root = std::make_unique<View>();
    StandaloneConfig config;
    config.show_settings_tab = true;

    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root), config, nullptr, nullptr, nullptr, {});

    REQUIRE(chrome.tab_panel() != nullptr);
    REQUIRE(&chrome.window_root() == chrome.tab_panel());
    REQUIRE(chrome.settings_panel() != nullptr);
    REQUIRE(chrome.tab_panel()->tab_count() == 2);
    REQUIRE(chrome.extra_window_height() == 0.0f);  // outer tab bar hidden — reserves no height
    REQUIRE(chrome.chrome_label() == std::string_view("tabs"));
}

TEST_CASE("Standalone editor chrome fills the editor tab to the design area "
          "(a fill-based editor must not collapse)",
          "[standalone][chrome][issue-45]") {
    // Regression for standalone editor squish: a fill-based editor (a bare View
    // has no intrinsic height and sets no flex on itself —
    // exactly like Bendr's design-viewport editor, which lays out from
    // local_bounds()) used to collapse to a thin strip inside the TabPanel.
    // TabPanel::add_tab applies no flex to tab content and FlexStyle defaults to
    // flex_grow=0 in a column, so the cross axis (width) stretched but the main
    // axis (height) collapsed to the zero intrinsic content height. The chrome
    // now sets flex_grow=1 on the editor root (mirroring SettingsPanel, which
    // does the same for itself), so the editor fills the letterboxed design area.
    auto editor_root = std::make_unique<View>();
    auto* editor_ptr = editor_root.get();
    StandaloneConfig config;
    config.show_settings_tab = true;

    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root), config, nullptr, nullptr, nullptr, {});

    // Mimic the host's design-size layout pass: the GPU window host lays the
    // window root out at the design-viewport size, then runs layout_children().
    View& root = chrome.window_root();
    root.set_bounds({0.0f, 0.0f, 760.0f, 560.0f});
    root.layout_children();

    // The visible editor tab must FILL the content area, not collapse to its
    // (zero) intrinsic height. Without the flex_grow fix this height is ~0.
    const auto bounds = editor_ptr->local_bounds();
    REQUIRE(bounds.width == Catch::Approx(760.0f).margin(1.0f));
    REQUIRE(bounds.height == Catch::Approx(560.0f).margin(1.0f));
}

TEST_CASE("Standalone settings chrome: an editor can detect and open the Settings tab",
          "[standalone][chrome][issue-45]") {
    // The editor-side mirror of the Settings panel's Done button: a plugin
    // editor nested in the standalone chrome can show its own gear and open the
    // Audio/MIDI Settings tab. In a DAW there is no chrome, so the helpers
    // report "unavailable" and opening is a safe no-op (the gear stays hidden).
    auto editor_root = std::make_unique<View>();
    auto* editor_ptr = editor_root.get();
    StandaloneConfig config;
    config.show_settings_tab = true;
    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root), config, nullptr, nullptr, nullptr, {});

    // Inside the chrome: settings are available and the editor tab is active.
    REQUIRE(pulp::format::standalone_settings_available(editor_ptr));
    REQUIRE(chrome.tab_panel()->active_tab() == 0);

    // Opening settings switches the chrome to the Settings tab (index 1).
    pulp::format::open_standalone_settings(editor_ptr);
    REQUIRE(chrome.tab_panel()->active_tab() == 1);

    // A loose view with no chrome ancestor (the plugin case): unavailable, and
    // open is a no-op that must not crash.
    auto loose = std::make_unique<View>();
    REQUIRE_FALSE(pulp::format::standalone_settings_available(loose.get()));
    pulp::format::open_standalone_settings(loose.get());
}

TEST_CASE("Standalone idle callback polls scripted UI before settings",
          "[standalone][chrome]") {
    StubWindowHost window;
    std::vector<std::string> calls;

    install_standalone_idle_callback(
        window,
        [&] { calls.push_back("scripted"); },
        [&] { calls.push_back("settings"); });

    REQUIRE(window.idle_callback_ != nullptr);
    window.idle_callback_();
    REQUIRE(calls == std::vector<std::string>{"scripted", "settings"});
}

TEST_CASE("Standalone idle callback still polls settings without scripted UI",
          "[standalone][chrome]") {
    StubWindowHost window;
    bool settings_polled = false;

    install_standalone_idle_callback(window, {}, [&] { settings_polled = true; });

    REQUIRE(window.idle_callback_ != nullptr);
    window.idle_callback_();
    REQUIRE(settings_polled);
}

TEST_CASE("Standalone idle callback can be composed by tool windows",
          "[standalone][chrome]") {
    std::vector<std::string> calls;

    auto prior = make_standalone_idle_callback(
        [&] { calls.push_back("scripted"); },
        [&] { calls.push_back("settings"); });
    auto composed = [prior, &calls] {
        prior();
        calls.push_back("audio-inspector");
    };

    composed();
    REQUIRE(calls == std::vector<std::string>{
        "scripted", "settings", "audio-inspector"});
}

TEST_CASE("Standalone repaint helper routes scripted UI repaint through the window",
          "[standalone][chrome]") {
    StubWindowHost window;
    StubScriptedUi scripted_ui;

    install_scripted_ui_repaint_callback(&scripted_ui, window);

    REQUIRE(scripted_ui.repaint_callback_ != nullptr);
    scripted_ui.repaint_callback_();
    REQUIRE(window.repaint_calls_ == 1);
}

TEST_CASE("Standalone pointer idle helper polls scripted UI and settings",
          "[standalone][chrome]") {
    StubWindowHost window;
    StubScriptedUi scripted_ui;
    StubSettingsPoller settings;

    install_standalone_idle_callback(window, &scripted_ui, &settings);

    REQUIRE(window.idle_callback_ != nullptr);
    window.idle_callback_();
    REQUIRE(scripted_ui.poll_calls_ == 1);
    REQUIRE(settings.poll_calls_ == 1);
}

TEST_CASE("Standalone editor content size subtracts chrome height",
          "[standalone][chrome]") {
    auto editor_root = std::make_unique<View>();
    auto tabs = make_standalone_editor_chrome(
        std::move(editor_root), StandaloneConfig{}, nullptr, nullptr, nullptr, {});

    // The outer [Editor][Settings] tab bar is hidden (card-stack chrome), so it reserves no
    // height — the editor fills the whole window and content height == window height.
    auto size = standalone_editor_content_size({640, 392}, tabs);
    REQUIRE(size.width == 640);
    REQUIRE(size.height == 392);

    auto editor_only = make_standalone_editor_chrome(
        std::make_unique<View>(),
        StandaloneConfig{.show_settings_tab = false},
        nullptr,
        nullptr,
        nullptr,
        {});
    size = standalone_editor_content_size({640, 360}, editor_only);
    REQUIRE(size.width == 640);
    REQUIRE(size.height == 360);
}

TEST_CASE("WindowHost default content-size and resize callback are no-ops",
          "[standalone][chrome]") {
    DefaultWindowHost window;

    const auto size = window.get_content_size();
    REQUIRE(size.width == 0);
    REQUIRE(size.height == 0);

    bool resize_fired = false;
    window.set_resize_callback([&](uint32_t, uint32_t) {
        resize_fired = true;
    });
    REQUIRE_FALSE(resize_fired);

    REQUIRE(window.capture_back_buffer_png() == std::vector<uint8_t>{0x89, 'P', 'N', 'G'});
}

TEST_CASE("Standalone host sync installs a resize callback and applies initial size",
          "[standalone][chrome]") {
    StubWindowHost window;
    window.content_size_ = {800, 432};
    auto chrome = make_standalone_editor_chrome(
        std::make_unique<View>(), StandaloneConfig{}, nullptr, nullptr, nullptr, {});

    std::vector<WindowHost::ContentSize> seen_sizes;
    sync_standalone_editor_host(
        window,
        chrome,
        [&](uint32_t width, uint32_t height) {
            seen_sizes.push_back({width, height});
        });

    REQUIRE(window.resize_callback_ != nullptr);
    REQUIRE(seen_sizes.size() == 1);
    REQUIRE(seen_sizes[0].width == 800);
    REQUIRE(seen_sizes[0].height == 432);  // hidden outer tab bar reserves no height

    window.resize_callback_(900, 532);
    REQUIRE(seen_sizes.size() == 2);
    REQUIRE(seen_sizes[1].width == 900);
    REQUIRE(seen_sizes[1].height == 532);
}

TEST_CASE("Standalone host sync skips zero-sized initial host content",
          "[standalone][chrome]") {
    StubWindowHost window;
    window.content_size_ = {0, 0};
    auto chrome = make_standalone_editor_chrome(
        std::make_unique<View>(), StandaloneConfig{}, nullptr, nullptr, nullptr, {});

    std::vector<WindowHost::ContentSize> seen_sizes;
    sync_standalone_editor_host(
        window,
        chrome,
        [&](uint32_t width, uint32_t height) {
            seen_sizes.push_back({width, height});
        });

    REQUIRE(window.resize_callback_ != nullptr);
    REQUIRE(seen_sizes.empty());

    window.resize_callback_(900, 532);
    REQUIRE(seen_sizes.size() == 1);
    REQUIRE(seen_sizes[0].width == 900);
    REQUIRE(seen_sizes[0].height == 532);  // no outer-chrome height subtracted
}

TEST_CASE("Standalone bridge attach forwards host sizing through bridge resize",
          "[standalone][chrome]") {
    StubWindowHost window;
    window.content_size_ = {840, 452};
    auto chrome = make_standalone_editor_chrome(
        std::make_unique<View>(), StandaloneConfig{}, nullptr, nullptr, nullptr, {});
    StubBridge bridge;

    attach_standalone_editor_bridge(window, chrome, bridge);

    REQUIRE(window.resize_callback_ != nullptr);
    REQUIRE(bridge.resize_calls_.size() == 1);
    REQUIRE(bridge.resize_calls_[0].width == 840);
    REQUIRE(bridge.resize_calls_[0].height == 452);  // no outer-chrome height subtracted

    window.resize_callback_(900, 500);
    REQUIRE(bridge.resize_calls_.size() == 2);
    REQUIRE(bridge.resize_calls_[1].width == 900);
    REQUIRE(bridge.resize_calls_[1].height == 500);
}

TEST_CASE("Standalone design viewport applies proportional window resize",
          "[standalone][chrome][resize][proportional]") {
    StubWindowHost window;
    auto chrome = make_standalone_editor_chrome(
        std::make_unique<View>(),
        StandaloneConfig{.show_settings_tab = false},
        nullptr,
        nullptr,
        nullptr,
        {});

    ViewSize hints = view_size_from_design(900, 520);
    configure_standalone_design_viewport(window, hints, chrome);

    REQUIRE(window.design_width_ == Catch::Approx(900.0f));
    REQUIRE(window.design_height_ == Catch::Approx(520.0f));
    REQUIRE(window.aspect_ratio_ == Catch::Approx(900.0f / 520.0f));
}

TEST_CASE("Standalone design viewport includes settings chrome height",
          "[standalone][chrome][resize][proportional]") {
    StubWindowHost window;
    auto chrome = make_standalone_editor_chrome(
        std::make_unique<View>(),
        StandaloneConfig{},
        nullptr,
        nullptr,
        nullptr,
        {});

    ViewSize hints = view_size_from_design(900, 520);
    configure_standalone_design_viewport(window, hints, chrome);

    REQUIRE(window.design_width_ == Catch::Approx(900.0f));
    REQUIRE(window.design_height_ == Catch::Approx(520.0f));  // no outer-chrome height
    REQUIRE(window.aspect_ratio_ == Catch::Approx(900.0f / 520.0f));
}

namespace {

class ResizeProcessor final : public Processor {
public:
    PluginDescriptor descriptor() const override { return {}; }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(
        pulp::audio::BufferView<float>&,
        const pulp::audio::BufferView<const float>&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const ProcessContext&) override {}
    ViewSize view_size() const override {
        return {900, 520, 320, 240, 1800, 1040, 900.0 / 520.0};
    }
};

}  // namespace

TEST_CASE("Standalone editor resize drives bridge and owned window",
          "[standalone][chrome][resize][editor-request]") {
    ResizeProcessor processor;
    pulp::state::StateStore store;
    ViewBridge bridge(processor, store);
    StubWindowHost window;
    int owner = 0;

    install_standalone_editor_resize_handler(
        processor, &owner, window, bridge);

    REQUIRE(processor.request_editor_resize(720, 480));
    REQUIRE(bridge.size_hints().preferred_width == 720);
    REQUIRE(bridge.size_hints().preferred_height == 480);
    REQUIRE(window.content_size_requests_.size() == 1);
    REQUIRE(window.content_size_requests_[0].width == 720);
    REQUIRE(window.content_size_requests_[0].height == 480);
    REQUIRE(window.design_width_ == Catch::Approx(720.0f));
    REQUIRE(window.design_height_ == Catch::Approx(480.0f));
    REQUIRE(window.aspect_ratio_ == Catch::Approx(1.5f));

    // Invalid requests fail before changing either the bridge or native host.
    REQUIRE_FALSE(processor.request_editor_resize(0, 480));
    REQUIRE(bridge.size_hints().preferred_width == 720);
    REQUIRE(window.content_size_requests_.size() == 1);

    processor.set_editor_resize_handler(&owner, nullptr);
    REQUIRE_FALSE(processor.request_editor_resize(800, 600));
}

TEST_CASE("Standalone tab resizing follows the bridge's current preferred size",
          "[standalone][chrome][resize][editor-request]") {
    ResizeProcessor processor;
    pulp::state::StateStore store;
    ViewBridge bridge(processor, store);
    StubWindowHost window;
    auto chrome = make_standalone_editor_chrome(
        std::make_unique<View>(), StandaloneConfig{}, nullptr, nullptr, nullptr, {});

    configure_standalone_tab_resizing(window, chrome, bridge);
    REQUIRE(chrome.tab_panel());

    REQUIRE(bridge.set_preferred_size(720, 480));
    const int editor = chrome.tab_panel()->find_tab("Editor");
    const int settings = chrome.tab_panel()->find_tab("Settings");
    REQUIRE(editor >= 0);
    REQUIRE(settings >= 0);

    chrome.tab_panel()->on_tab_change(editor);
    REQUIRE(window.content_size_requests_.back().width == 720);
    REQUIRE(window.content_size_requests_.back().height == 480);

    chrome.tab_panel()->on_tab_change(settings);
    REQUIRE(window.content_size_requests_.back().width == 720);
    REQUIRE(window.content_size_requests_.back().height ==
            static_cast<uint32_t>(SettingsPanel::preferred_height()));

    auto editor_only = make_standalone_editor_chrome(
        std::make_unique<View>(),
        StandaloneConfig{.show_settings_tab = false},
        nullptr, nullptr, nullptr, {});
    configure_standalone_tab_resizing(window, editor_only, bridge);
    REQUIRE_FALSE(editor_only.tab_panel());
}

TEST_CASE("Standalone log helper formats the chrome mode",
          "[standalone][chrome]") {
    auto editor_root = std::make_unique<View>();
    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root), StandaloneConfig{}, nullptr, nullptr, nullptr, {});

    log_standalone_window_open(640, 360, false, false, chrome);
    SUCCEED();
}

TEST_CASE("Standalone environment opts screenshot runs into hidden mode",
          "[standalone][chrome][issue-2515]") {
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv test_mode("PULP_TEST_MODE");
    ScopedEnv ci("CI");
    ScopedEnv screenshot("PULP_SCREENSHOT");
    ScopedEnv frames("PULP_FRAMES");
    headless.set("1");
    test_mode.unset();
    ci.unset();
    screenshot.set("/tmp/pulp-standalone-headless.png");
    frames.set("2");

    auto config = standalone_config_from_environment(StandaloneConfig{});

    REQUIRE(config.headless);
    REQUIRE(config.screenshot_path == "/tmp/pulp-standalone-headless.png");
    REQUIRE(config.screenshot_frame_delay == 2);
    REQUIRE_FALSE(standalone_headless_requires_screenshot(config));
}

TEST_CASE("Standalone environment preserves explicit config over env defaults",
          "[standalone][chrome][audio-inspector]") {
    ScopedEnv screenshot("PULP_SCREENSHOT");
    ScopedEnv frames("PULP_FRAMES");
    ScopedEnv probe_json("PULP_AUDIO_PROBE_JSON");
    screenshot.set("/tmp/env-shot.png");
    frames.set("7");
    probe_json.set("/tmp/env-probe.json");

    StandaloneConfig base;
    base.screenshot_path = "/tmp/config-shot.png";
    base.audio_probe_json_path = "/tmp/config-probe.json";
    base.screenshot_frame_delay = 11;

    auto config = standalone_config_from_environment(base);

    REQUIRE(config.screenshot_path == "/tmp/config-shot.png");
    REQUIRE(config.audio_probe_json_path == "/tmp/config-probe.json");
    REQUIRE(config.screenshot_frame_delay == 7);
    REQUIRE(config.headless);
    REQUIRE_FALSE(standalone_headless_requires_screenshot(config));
}

TEST_CASE("Standalone frame delay env accepts only positive plain integers",
          "[standalone][chrome][audio-inspector]") {
    int frames = 99;
    REQUIRE(parse_positive_frame_delay("1", frames));
    REQUIRE(frames == 1);
    REQUIRE(parse_positive_frame_delay("2147483647", frames));
    REQUIRE(frames == 2147483647);

    frames = 99;
    REQUIRE_FALSE(parse_positive_frame_delay("", frames));
    REQUIRE_FALSE(parse_positive_frame_delay("+1", frames));
    REQUIRE_FALSE(parse_positive_frame_delay("0", frames));
    REQUIRE_FALSE(parse_positive_frame_delay("-1", frames));
    REQUIRE_FALSE(parse_positive_frame_delay("12x", frames));
    REQUIRE_FALSE(parse_positive_frame_delay("12.5", frames));
    REQUIRE_FALSE(parse_positive_frame_delay("999999999999999999999", frames));
    REQUIRE(frames == 99);
}

TEST_CASE("Standalone delayed action fires exactly once after the configured delay",
          "[standalone][chrome][audio-inspector]") {
    DelayedAction action;
    action.delay = 2;
    int actions = 0;
    int closes = 0;
    action.action_fn = [&] { ++actions; };
    action.close_fn = [&] { ++closes; };

    action();
    REQUIRE(actions == 0);
    REQUIRE(closes == 0);

    action();
    REQUIRE(actions == 1);
    REQUIRE(closes == 1);

    action();
    REQUIRE(actions == 1);
    REQUIRE(closes == 1);
}

#if PULP_ENABLE_AUDIO_PROBES
TEST_CASE("Standalone audio probe JSON helpers write normalized snapshot files",
          "[standalone][chrome][audio-inspector]") {
    pulp::audio::AudioProbeSnapshot snap;
    snap.stage_id = pulp::audio::AudioProbeStage::kStandaloneOutputBoundary;
    snap.sample_rate = 44100.0;
    snap.block_size = 64;
    snap.channel_count = 1;
    snap.sequence_number = 3;
    snap.peak_max = 0.25f;
    snap.rms_max = 0.125f;
    snap.callbacks = 17;
    snap.clipped_blocks = 2;
    snap.nan_blocks = 1;

    auto stats = stats_for_probe_json_snapshot(snap);
    REQUIRE(stats.callbacks == 17);
    REQUIRE(stats.clipped_blocks == 2);
    REQUIRE(stats.nan_blocks == 1);

    const auto shot = (std::filesystem::temp_directory_path() /
                       "pulp-helper-main.png")
                          .string();
    REQUIRE(audio_inspector_screenshot_path(shot)
            == (std::filesystem::temp_directory_path() /
                "pulp-helper-main.audio-inspector.png")
                   .string());

    const auto out_path = std::filesystem::temp_directory_path() /
                          "pulp-audio-probe-helper.json";
    std::filesystem::remove(out_path);
    REQUIRE(write_audio_probe_json_snapshot(out_path.string(), snap));

    {
        std::ifstream in(out_path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto v = choc::json::parse(buffer.str());
        REQUIRE(v["stage"].getString() == "standalone_output_boundary");
        REQUIRE(v["callbacks"].get<std::int64_t>() == 17);
        REQUIRE(v["clipped_blocks"].get<std::int64_t>() == 2);
        REQUIRE(v["nan_blocks"].get<std::int64_t>() == 1);
    }
    std::filesystem::remove(out_path);

    REQUIRE_FALSE(write_audio_probe_json_snapshot("", snap));
    REQUIRE_FALSE(write_audio_probe_json_snapshot(
        std::filesystem::temp_directory_path().string(), snap));

    pulp::audio::AudioProbe probe;
    probe.prepare(1, 8, 44100.0, pulp::audio::AudioProbeStage::kMeterBridge);
    const auto wrapper_path = std::filesystem::temp_directory_path() /
                              "pulp-audio-probe-wrapper.json";
    std::filesystem::remove(wrapper_path);
    REQUIRE(write_audio_probe_json_file(wrapper_path.string(), probe));
    std::filesystem::remove(wrapper_path);
}

TEST_CASE("Standalone audio scope JSON helper writes v1 scope captures",
          "[standalone][chrome][audio-scope]") {
    pulp::audio::AudioProbe probe;
    pulp::audio::AudioProbe::CaptureConfig cap;
    cap.capture_frames = 16;
    probe.prepare(1, 8, 48000.0,
                  pulp::audio::AudioProbeStage::kStandaloneOutputBoundary, cap);

    std::vector<float> samples{
        -0.5f, -0.25f, 0.25f, 0.5f, -0.25f, 0.25f, 0.5f, -0.5f,
    };
    const float* ptrs[] = {samples.data()};
    pulp::audio::BufferView<const float> view(ptrs, 1, samples.size());
    probe.analyze_output(view);

    StandaloneConfig config;
    config.audio_scope_window_samples = 4;
    config.audio_scope_trigger = "rising-zero";
    config.audio_scope_channel = 0;

    const auto out_path = std::filesystem::temp_directory_path() /
                          "pulp-audio-scope-helper.json";
    std::filesystem::remove(out_path);
    REQUIRE(write_audio_scope_json_file(out_path.string(), probe, config));

    {
        std::ifstream in(out_path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto v = choc::json::parse(buffer.str());
        REQUIRE(v["schema"].getString() == "pulp.audio.scope.v1");
        REQUIRE(v["version"].get<std::int64_t>() == 1);
        REQUIRE(v["source"]["stage"].getString() == "standalone_output_boundary");
        REQUIRE(v["source"]["selected_channel"].get<std::int64_t>() == 0);
        REQUIRE(v["acquisition"]["trigger_mode"].getString() == "rising_zero");
        REQUIRE(v["acquisition"]["window_samples"].get<std::int64_t>() == 4);
        REQUIRE(v["measurements"]["peak_to_peak"].get<double>() > 0.0);
    }
    std::filesystem::remove(out_path);
}

TEST_CASE("Standalone PULP_AUDIO_PROBE_JSON env arms a headless probe dump",
          "[standalone][chrome][audio-inspector]") {
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv screenshot("PULP_SCREENSHOT");
    ScopedEnv probe_json("PULP_AUDIO_PROBE_JSON");
    headless.unset();
    screenshot.unset();           // probe-json alone, no screenshot requested
    probe_json.set("/tmp/pulp-standalone-probe.json");

    auto config = standalone_config_from_environment(StandaloneConfig{});

    // The dump is a headless one-shot like --screenshot, so it implies
    // headless — but with an EMPTY screenshot path (no PNG forced).
    REQUIRE(config.audio_probe_json_path == "/tmp/pulp-standalone-probe.json");
    REQUIRE(config.headless);
    REQUIRE(config.screenshot_path.empty());
    REQUIRE_FALSE(standalone_headless_requires_screenshot(config));
    REQUIRE_FALSE(standalone_probe_json_requested_but_disabled(config));
}

TEST_CASE("Standalone PULP_AUDIO_SCOPE_JSON env arms a headless scope dump",
          "[standalone][chrome][audio-scope]") {
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv screenshot("PULP_SCREENSHOT");
    ScopedEnv scope_json("PULP_AUDIO_SCOPE_JSON");
    ScopedEnv scope_window("PULP_AUDIO_SCOPE_WINDOW");
    ScopedEnv scope_trigger("PULP_AUDIO_SCOPE_TRIGGER");
    ScopedEnv scope_channel("PULP_AUDIO_SCOPE_CHANNEL");
    headless.unset();
    screenshot.unset();
    scope_json.set("/tmp/pulp-standalone-scope.json");
    scope_window.set("4096");
    scope_trigger.set("raw");
    scope_channel.set("0");

    auto config = standalone_config_from_environment(StandaloneConfig{});
    REQUIRE(config.audio_scope_json_path == "/tmp/pulp-standalone-scope.json");
    REQUIRE(config.audio_scope_window_samples == 4096);
    REQUIRE(config.audio_scope_trigger == "raw");
    REQUIRE(config.audio_scope_channel == 0);
    REQUIRE(config.headless);
    REQUIRE(config.screenshot_path.empty());
    REQUIRE_FALSE(standalone_headless_requires_screenshot(config));
    REQUIRE_FALSE(standalone_probe_json_requested_but_disabled(config));
}
#else
TEST_CASE("Standalone probe JSON requests are rejected when probes are disabled",
          "[standalone][chrome][audio-inspector]") {
    ScopedEnv probe_json("PULP_AUDIO_PROBE_JSON");
    probe_json.set("/tmp/pulp-standalone-probe.json");

    auto config = standalone_config_from_environment(StandaloneConfig{});
    REQUIRE(config.audio_probe_json_path == "/tmp/pulp-standalone-probe.json");
    REQUIRE(config.headless);
    REQUIRE(standalone_probe_json_requested_but_disabled(config));

    probe_json.unset();
    config.audio_probe_json_path = "/tmp/pulp-standalone-probe.json";
    REQUIRE(standalone_probe_json_requested_but_disabled(config));
}
#endif

TEST_CASE("Standalone headless CI without screenshot is rejected before launch",
          "[standalone][chrome][issue-2515]") {
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv test_mode("PULP_TEST_MODE");
    ScopedEnv ci("CI");
    ScopedEnv screenshot("PULP_SCREENSHOT");
    ScopedEnv frames("PULP_FRAMES");
    headless.set("0");
    test_mode.unset();
    ci.set("true");
    screenshot.unset();
    frames.set("not-an-int");

    StandaloneConfig base;
    base.screenshot_frame_delay = 9;
    auto config = standalone_config_from_environment(base);

    REQUIRE(config.headless);
    REQUIRE(config.screenshot_path.empty());
    REQUIRE(config.screenshot_frame_delay == 9);
    REQUIRE(standalone_headless_requires_screenshot(config));
}

TEST_CASE("Standalone empty headless env vars are ignored",
          "[standalone][chrome][issue-2515]") {
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv test_mode("PULP_TEST_MODE");
    ScopedEnv ci("CI");
    ScopedEnv screenshot("PULP_SCREENSHOT");
    headless.set("");
    test_mode.set("");
    ci.set("");
    screenshot.unset();

    auto config = standalone_config_from_environment(StandaloneConfig{});

    REQUIRE_FALSE(config.headless);
    REQUIRE(config.screenshot_path.empty());
    REQUIRE_FALSE(standalone_headless_requires_screenshot(config));
}

TEST_CASE("Plugin editor disable env blocks native editor launch",
          "[standalone][chrome][issue-2515]") {
    ScopedEnv disable_editor("PULP_DISABLE_PLUGIN_EDITOR");
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv test_mode("PULP_TEST_MODE");
    ScopedEnv ci("CI");
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    ScopedEnv display("DISPLAY");
    ScopedEnv wayland("WAYLAND_DISPLAY");
    display.set(":99");
    wayland.unset();
#endif
    headless.unset();
    test_mode.unset();
    ci.unset();
    disable_editor.unset();

    REQUIRE_FALSE(editor_launch_blocked_by_environment());

    disable_editor.set("1");
    REQUIRE(editor_launch_blocked_by_environment());

    disable_editor.unset();
    headless.set("1");
    REQUIRE(editor_launch_blocked_by_environment());

    headless.unset();
    test_mode.set("1");
    REQUIRE(editor_launch_blocked_by_environment());

    test_mode.unset();
    ci.set("1");
    REQUIRE(editor_launch_blocked_by_environment());
}

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
TEST_CASE("Plugin editor launch is blocked when no display is available",
          "[standalone][chrome][issue-2515]") {
    ScopedEnv disable_editor("PULP_DISABLE_PLUGIN_EDITOR");
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv test_mode("PULP_TEST_MODE");
    ScopedEnv ci("CI");
    ScopedEnv display("DISPLAY");
    ScopedEnv wayland("WAYLAND_DISPLAY");
    disable_editor.unset();
    headless.unset();
    test_mode.unset();
    ci.unset();
    display.unset();
    wayland.unset();

    REQUIRE(editor_launch_blocked_by_environment());
}
#endif

TEST_CASE("make_standalone_window_options propagates min_* from ViewSize",
          "[standalone][chrome][issue-1362]") {
    auto editor_root = std::make_unique<View>();
    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root),
        StandaloneConfig{.show_settings_tab = false},
        nullptr, nullptr, nullptr, {});

    pulp::format::ViewSize hints;
    hints.preferred_width = 1320;
    hints.preferred_height = 860;
    hints.min_width = 800;
    hints.min_height = 600;

    auto opts = make_standalone_window_options(hints, chrome, "Plug — Standalone", false);

    REQUIRE(opts.title == "Plug — Standalone");
    REQUIRE(opts.width == Catch::Approx(1320.0f));
    REQUIRE(opts.height == Catch::Approx(860.0f));
    REQUIRE(opts.min_width == Catch::Approx(800.0f));
    REQUIRE(opts.min_height == Catch::Approx(600.0f));
    REQUIRE(opts.resizable);
    REQUIRE_FALSE(opts.use_gpu);
}

TEST_CASE("make_standalone_window_options leaves min_* at zero when unset",
          "[standalone][chrome][issue-1362]") {
    auto editor_root = std::make_unique<View>();
    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root),
        StandaloneConfig{.show_settings_tab = false},
        nullptr, nullptr, nullptr, {});

    // Default ViewSize: zero min_* — the platform window host should
    // see "no minimum" rather than a stale clamp.
    pulp::format::ViewSize hints;
    hints.preferred_width = 600;
    hints.preferred_height = 400;

    auto opts = make_standalone_window_options(hints, chrome, "X", true);

    REQUIRE(opts.min_width == Catch::Approx(0.0f));
    REQUIRE(opts.min_height == Catch::Approx(0.0f));
    REQUIRE(opts.use_gpu);
}

TEST_CASE("make_standalone_window_options extends min_height when chrome adds rows",
          "[standalone][chrome][issue-1362]") {
    // The Settings tab is reached via the editor's own control (the outer tab bar is hidden),
    // so the chrome reserves no extra height and the window/min sizes are unchanged.
    auto editor_root = std::make_unique<View>();
    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root),
        StandaloneConfig{.show_settings_tab = true},
        nullptr, nullptr, nullptr, {});
    REQUIRE(chrome.extra_window_height() == Catch::Approx(0.0f));

    pulp::format::ViewSize hints;
    hints.preferred_width = 1320;
    hints.preferred_height = 860;
    hints.min_width = 800;
    hints.min_height = 600;

    auto opts = make_standalone_window_options(hints, chrome, "Plug", false);

    // Preferred height = preferred_height + chrome.extra_window_height()
    REQUIRE(opts.height == Catch::Approx(860.0f));  // preferred_height + 0 chrome
    REQUIRE(opts.min_width == Catch::Approx(800.0f));
    REQUIRE(opts.min_height == Catch::Approx(600.0f));  // min unchanged (no chrome height)
}
