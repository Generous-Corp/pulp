#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/format/detail/standalone_editor_chrome.hpp>

#include <cstdlib>
#include <memory>
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
    REQUIRE(panel.child_count() == 1);
    auto* tabs = dynamic_cast<TabPanel*>(panel.child_at(0));
    REQUIRE(tabs != nullptr);
    return *tabs;
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

    REQUIRE(output_combo->items().size() == 2);
    REQUIRE(input_combo->items().size() == 2);

    output_combo->set_selected(1);
    REQUIRE(applied.audio_device_id == "usb-out");
    REQUIRE(applied.sample_rate == 48000.0);
    REQUIRE(applied.buffer_size == 128);

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
    REQUIRE(apply_calls >= 5);
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
    REQUIRE(combos.size() >= 5);
    REQUIRE(lists.size() == 1);
    REQUIRE(audio.enumerate_calls == 2);
    REQUIRE(midi.enumerate_calls == 2);
    REQUIRE(combos[0]->items().size() == 2);
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

    combos[4]->set_selected(2);
    REQUIRE(signal_calls == 2);
    REQUIRE(last_signal.sine_frequency_hz == 880.0f);
}

TEST_CASE("SettingsPanel uses fallback rate and buffer choices without output devices",
          "[standalone][settings][coverage][phase3]") {
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
    REQUIRE(combos[0]->items().empty());
    REQUIRE(combos[1]->items().size() == 2);
    REQUIRE(combos[2]->items().size() >= 6);
    REQUIRE(combos[3]->items().size() >= 7);

    combos[2]->set_selected(1);
    REQUIRE(apply_calls == 1);
    REQUIRE(applied.audio_device_id.empty());
    REQUIRE(applied.sample_rate == 48000.0);
    REQUIRE(applied.buffer_size == 64);

    combos[3]->set_selected(2);
    REQUIRE(apply_calls == 2);
    REQUIRE(applied.audio_device_id.empty());
    REQUIRE(applied.sample_rate == 48000.0);
    REQUIRE(applied.buffer_size == 256);
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
    REQUIRE(chrome.extra_window_height() == 32.0f);
    REQUIRE(chrome.chrome_label() == std::string_view("tabs"));
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

    auto size = standalone_editor_content_size({640, 392}, tabs);
    REQUIRE(size.width == 640);
    REQUIRE(size.height == 360);

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
    REQUIRE(seen_sizes[0].height == 400);

    window.resize_callback_(900, 532);
    REQUIRE(seen_sizes.size() == 2);
    REQUIRE(seen_sizes[1].width == 900);
    REQUIRE(seen_sizes[1].height == 500);
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
    REQUIRE(seen_sizes[0].height == 500);
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
    REQUIRE(bridge.resize_calls_[0].height == 420);

    window.resize_callback_(900, 500);
    REQUIRE(bridge.resize_calls_.size() == 2);
    REQUIRE(bridge.resize_calls_[1].width == 900);
    REQUIRE(bridge.resize_calls_[1].height == 468);
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
    // Settings tab adds 32px of chrome — the height min must rise in
    // lockstep with the preferred-height extension so the editor area
    // can never be squeezed below its declared min.
    auto editor_root = std::make_unique<View>();
    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root),
        StandaloneConfig{.show_settings_tab = true},
        nullptr, nullptr, nullptr, {});
    REQUIRE(chrome.extra_window_height() == Catch::Approx(32.0f));

    pulp::format::ViewSize hints;
    hints.preferred_width = 1320;
    hints.preferred_height = 860;
    hints.min_width = 800;
    hints.min_height = 600;

    auto opts = make_standalone_window_options(hints, chrome, "Plug", false);

    // Preferred height = preferred_height + chrome.extra_window_height()
    REQUIRE(opts.height == Catch::Approx(892.0f));
    // Min height is shifted by the same chrome amount so the editor
    // area's own minimum is never violated.
    REQUIRE(opts.min_width == Catch::Approx(800.0f));
    REQUIRE(opts.min_height == Catch::Approx(632.0f));
}
