// Plugin-contributed settings sections (MM-PR5). Verifies the contract that lets a
// plugin surface its own Settings tabs (e.g. a model picker) which the host composes
// alongside its own host-owned Audio/MIDI tabs — keeping device selection a host concern
// while giving one unified Settings panel.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/settings_panel.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/test_signal.hpp>
#include <pulp/view/audio_bridge.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/visualizers.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/window_host.hpp>

#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using pulp::format::Processor;
using pulp::format::SettingsPanel;

namespace {

// Depth-first collect every descendant widget of type T (the panel keeps its
// combos/toggles private, so tests reach them through the public view tree).
template <typename T>
void collect_widgets(pulp::view::View* view, std::vector<T*>& out) {
    if (auto* typed = dynamic_cast<T*>(view)) out.push_back(typed);
    for (size_t i = 0; i < view->child_count(); ++i)
        collect_widgets<T>(view->child_at(i), out);
}


// Minimal concrete Processor whose contributed sections are configurable per test.
struct TestProcessor : Processor {
    std::vector<std::string> section_titles;

    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Test";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.settings";
        d.version = "1.0.0";
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    std::vector<SettingsSection> settings_sections() override {
        std::vector<SettingsSection> out;
        for (const auto& title : section_titles)
            out.push_back({title, std::make_unique<pulp::view::Label>(title)});
        return out;
    }
};

std::unique_ptr<pulp::view::View> label(const std::string& t) {
    return std::make_unique<pulp::view::Label>(t);
}

}  // namespace

TEST_CASE("Processor::settings_sections defaults to none", "[format][settings]") {
    struct Bare : TestProcessor {};
    Bare p;
    REQUIRE(p.settings_sections().empty());
}

TEST_CASE("A plugin contributes named settings sections with views", "[format][settings]") {
    TestProcessor p;
    p.section_titles = {"Models", "About"};
    auto secs = p.settings_sections();
    REQUIRE(secs.size() == 2);
    REQUIRE(secs[0].title == "Models");
    REQUIRE(secs[0].view != nullptr);
    REQUIRE(secs[1].title == "About");
    REQUIRE(secs[1].view != nullptr);
}

TEST_CASE("SettingsPanel signal-type combo maps Sine/Noise to the test-signal config",
          "[format][settings][signal_type]") {
    using pulp::format::SettingsPanelCallbacks;
    using pulp::format::TestSignalConfig;
    using pulp::format::TestSignalType;

    SettingsPanel panel;
    TestSignalConfig last{};
    int calls = 0;
    SettingsPanelCallbacks cb;
    cb.on_test_signal_changed = [&](const TestSignalConfig& cfg) { last = cfg; ++calls; };
    panel.set_callbacks(std::move(cb));

    // Locate the host-owned Sine/Noise combo in the built Audio tab.
    std::vector<pulp::view::ComboBox*> combos;
    collect_widgets(&panel, combos);
    pulp::view::ComboBox* type_combo = nullptr;
    for (auto* combo : combos)
        if (combo->items() == std::vector<std::string>{"Sine", "Noise"}) type_combo = combo;
    REQUIRE(type_combo != nullptr);
    REQUIRE(type_combo->selected() == 0);   // defaults to Sine

    // Find the test-tone toggle by effect: the toggle whose on_toggle drives the
    // test-signal callback. Enabling it with the type at Sine emits a sine config.
    std::vector<pulp::view::Toggle*> toggles;
    collect_widgets(&panel, toggles);
    pulp::view::Toggle* tone = nullptr;
    for (auto* toggle : toggles) {
        if (!toggle->on_toggle) continue;
        const int before = calls;
        toggle->on_toggle(true);
        if (calls > before) { tone = toggle; break; }
    }
    REQUIRE(tone != nullptr);
    REQUIRE(last.type == TestSignalType::sine);

    // Selecting Noise while the tone is on re-emits with the noise type; the
    // combo's on_change re-fires the toggle so the host picks up the new source.
    tone->set_on(true, false);        // is_on() → true, without notifying
    type_combo->set_selected(1);      // fires on_change → re-emit
    REQUIRE(last.type == TestSignalType::noise);

    // Back to Sine re-emits a sine config.
    type_combo->set_selected(0);
    REQUIRE(last.type == TestSignalType::sine);
}

TEST_CASE("SettingsPanel starts with host-owned Audio + MIDI tabs", "[format][settings]") {
    SettingsPanel panel;
    REQUIRE(panel.tab_count() == 2);  // Audio + MIDI
}

TEST_CASE("add_section appends plugin tabs after Audio/MIDI", "[format][settings]") {
    SettingsPanel panel;
    panel.add_section("Models", label("models"));
    REQUIRE(panel.tab_count() == 3);
    panel.add_section("License", label("license"));
    REQUIRE(panel.tab_count() == 4);
}

TEST_CASE("SettingsPanel can select a plugin tab by title", "[format][settings]") {
    SettingsPanel panel;
    panel.add_section("Models", label("models"));
    panel.add_section("License", label("license"));

    REQUIRE(panel.active_tab() == 0);
    REQUIRE(panel.set_active_tab("Models"));
    REQUIRE(panel.active_tab() == 2);
    REQUIRE_FALSE(panel.set_active_tab("Missing"));
    REQUIRE(panel.active_tab() == 2);
}

TEST_CASE("add_section ignores a null view", "[format][settings]") {
    SettingsPanel panel;
    const int before = panel.tab_count();
    panel.add_section("Nope", nullptr);
    REQUIRE(panel.tab_count() == before);
}

TEST_CASE("SettingsPanel Audio tab renders for visual inspection", "[format][settings][.demo]") {
    SettingsPanel panel;  // default Audio tab active; combos empty without bound systems
    auto png = pulp::view::render_to_png(panel, 700, 460, 2.0f,
                                         pulp::view::ScreenshotBackend::skia);
    REQUIRE_FALSE(png.empty());
    std::ofstream("/tmp/settings-audio.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

TEST_CASE("Standalone settings persist across launches (save → load round-trip)", "[format][settings]") {
    using pulp::format::StandaloneApp;
    using pulp::format::StandaloneConfig;
    const std::string app = "pulp-test-persist-roundtrip";

    // The user picks a device / rate / buffer; the standalone saves it.
    StandaloneConfig saved;
    saved.audio_device_id = "test-headphones";
    saved.midi_input_id = "test-midi";
    saved.sample_rate = 44100.0;
    saved.buffer_size = 256;
    REQUIRE(StandaloneApp::save_persisted_config(app, saved));

    // Next launch: load overlays the saved keys onto the app's configured defaults.
    StandaloneConfig defaults;
    defaults.sample_rate = 48000.0;
    defaults.buffer_size = 512;
    auto restored = StandaloneApp::load_persisted_config(app, defaults);
    REQUIRE(restored.audio_device_id == "test-headphones");
    REQUIRE(restored.midi_input_id == "test-midi");
    REQUIRE(restored.sample_rate == 44100.0);  // persisted value wins over the default
    REQUIRE(restored.buffer_size == 256);
}

TEST_CASE("load_persisted_config keeps the caller's defaults for an unknown app", "[format][settings]") {
    using pulp::format::StandaloneApp;
    using pulp::format::StandaloneConfig;
    // An app name with no saved file returns the base untouched (the first-launch case).
    StandaloneConfig defaults;
    defaults.audio_device_id = "configured-default";
    defaults.sample_rate = 96000.0;
    auto restored = StandaloneApp::load_persisted_config("pulp-test-no-such-app-9F3A2", defaults);
    REQUIRE(restored.audio_device_id == "configured-default");
    REQUIRE(restored.sample_rate == 96000.0);
}

TEST_CASE("Host composition: plugin sections compose onto the host panel", "[format][settings]") {
    // Mirrors exactly what the standalone chrome does with processor.settings_sections().
    TestProcessor p;
    p.section_titles = {"Models", "License"};

    SettingsPanel panel;
    for (auto& sec : p.settings_sections())
        if (sec.view) panel.add_section(std::move(sec.title), std::move(sec.view));

    REQUIRE(panel.tab_count() == 4);  // Audio + MIDI + Models + License
}

// ── Idle gate ────────────────────────────────────────────────────────────────
// SettingsPanel::poll() is installed as the standalone window's idle callback,
// and every macOS host fires that on EVERY display-link tick. `pop_latest_meter`
// reads a triple buffer and reports success for any block the audio thread has
// ever published, new or not, so the poll reaches the meters at the display's
// refresh rate for as long as an audio device is open — through digital silence,
// with the Settings tab shut. Repainting on each of those marks the whole window
// dirty, which arms the host's needs_repaint flag before every vsync: a window
// nobody is touching composites a full frame ~60-120 times a second forever.
//
// So the property is not "the meter updates". It is that an untouched window
// asks for NO frames: a still meter requests nothing, and a meter nobody can see
// requests nothing however loud the bus is.

namespace {

// Counts the repaints a view actually asks the host for. View::request_repaint()
// routes through WindowHost::mark_dirty() → repaint() when no RenderLoop is
// attached, so this is exactly the signal that arms a real host's dirty flag.
class RepaintCountingHost : public pulp::view::WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override { ++repaints; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
    int repaints = 0;
};

pulp::view::MeterData stereo_levels(float peak, float rms) {
    pulp::view::MeterData d;
    d.num_channels = 2;
    d.peak[0] = d.peak[1] = peak;
    d.rms[0] = d.rms[1] = rms;
    return d;
}

// Mount a panel under a host the way the standalone chrome does, so
// request_repaint() has somewhere to land.
struct MountedPanel {
    RepaintCountingHost host;
    pulp::view::View root;
    SettingsPanel* panel = nullptr;
    pulp::view::AudioBridge input;
    pulp::view::AudioBridge output;

    MountedPanel() {
        root.set_bounds({0, 0, 600, 500});
        auto owned = std::make_unique<SettingsPanel>();
        panel = owned.get();
        root.add_child(std::move(owned));
        root.set_window_host(&host);
        panel->set_input_meter_bridge(&input);
        panel->set_output_meter_bridge(&output);
    }

    void tick(int frames) {
        for (int i = 0; i < frames; ++i) panel->poll();
    }
};

}  // namespace

TEST_CASE("an open Settings panel over a silent bus settles to zero repaints",
          "[format][settings][idle-gate]") {
    MountedPanel m;
    m.panel->set_visible(true);

    // Control first: the instrument must be able to report motion, or "zero
    // repaints" below would prove nothing. A real level moves the needle.
    m.input.push_meter(stereo_levels(0.8f, 0.5f));
    m.output.push_meter(stereo_levels(0.8f, 0.5f));
    m.tick(1);
    REQUIRE(m.host.repaints > 0);

    // Silence. The ballistics decay, the peak hold expires, and every displayed
    // value reaches its resting state — after which no frame differs from the
    // last. 10 s at 30 Hz is far longer than the 1.5 s hold plus the 0.3 s
    // release, so anything still moving here moves forever.
    m.input.push_meter(stereo_levels(0.0f, 0.0f));
    m.output.push_meter(stereo_levels(0.0f, 0.0f));
    m.tick(300);

    const int settled = m.host.repaints;
    m.tick(300);
    CHECK(m.host.repaints == settled);  // 10 further seconds, not one frame asked for

    // And the gate never swallows real motion: the next real level repaints.
    m.input.push_meter(stereo_levels(0.6f, 0.4f));
    m.tick(1);
    CHECK(m.host.repaints > settled);
}

TEST_CASE("a channel-count change repaints even on a silent bus",
          "[format][settings][idle-gate]") {
    MountedPanel m;
    m.panel->set_visible(true);

    // Settle a silent stereo meter, so nothing at all is moving.
    m.input.push_meter(stereo_levels(0.0f, 0.0f));
    m.tick(300);
    const int settled = m.host.repaints;

    // A device swap changes how many bars are drawn without moving a level.
    // Gating purely on the levels would leave the old bar count on screen.
    pulp::view::MeterData mono;
    mono.num_channels = 1;
    m.input.push_meter(mono);
    m.tick(1);
    CHECK(m.host.repaints > settled);
}

TEST_CASE("a Settings panel that is not on screen asks for no frames at all",
          "[format][settings][idle-gate]") {
    MountedPanel m;
    // The standalone chrome parks the panel in a TabPanel, which hides every
    // tab but the active one — the state an untouched app sits in.
    m.panel->set_visible(false);

    // A bus that is anything but silent: a fresh, different level every frame,
    // so a poll that reached the meters could not possibly find them still.
    for (int i = 0; i < 300; ++i) {
        const float level = 0.2f + 0.6f * static_cast<float>(i % 7) / 7.0f;
        m.input.push_meter(stereo_levels(level, level * 0.5f));
        m.output.push_meter(stereo_levels(level, level * 0.5f));
        m.panel->poll();
    }
    CHECK(m.host.repaints == 0);

    // Opening Settings puts it back on screen, and the meters resume.
    m.panel->set_visible(true);
    m.input.push_meter(stereo_levels(0.9f, 0.6f));
    m.panel->poll();
    CHECK(m.host.repaints > 0);
}
