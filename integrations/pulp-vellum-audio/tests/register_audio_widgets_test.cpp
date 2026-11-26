// The three data-display ids a script reaches through createMeter,
// createWaveform and createSpectrum, run through a real Vellum scripted
// session so the provider lookup is the one a design tool or editor performs.
#include <pulp/vellum_audio/audio_widget_registration.hpp>
#include <pulp/vellum_audio/audio_widgets.hpp>
#include <pulp/vellum_audio/waveform_view.hpp>

#include <vellum/state/store.hpp>
#include <vellum/view/data_display.hpp>
#include <vellum/view/screenshot.hpp>
#include <vellum/view/scripted_ui.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pulp::vellum_audio;

namespace {

constexpr const char* kScript = R"JS(
createMeter('m');
createWaveform('w');
createSpectrum('s');
)JS";

constexpr const char* kDataScript = R"JS(
createMeter('m');
createWaveform('w');
createSpectrum('s');
setMeterLevel('m', 0.7, 0.9);
var wave = [];
for (var i = 0; i < 256; ++i) wave.push(Math.sin(i * 0.2));
setWaveformData('w', wave);
var bins = [];
for (var i = 0; i < 64; ++i) bins.push(-60 + i);
setSpectrumData('s', bins);
)JS";

struct Mounted {
    ::vellum::view::View root;
    ::vellum::state::StateStore store;
    ::vellum::view::ScriptedUiSession session;
    std::string error;
    bool loaded = false;

    explicit Mounted(const char* source)
        : session(root, store, {.script_source = source, .script_name = "audio-displays.js"}) {
        root.set_bounds({0, 0, 240, 120});
        loaded = session.load(&error);
    }
    template <typename T> T* widget(const char* id) {
        return session.bridge() ? dynamic_cast<T*>(session.bridge()->widget(id)) : nullptr;
    }
};

struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

Frame render(::vellum::view::View& view) {
    Frame f;
    f.pixels = ::vellum::view::render_to_rgba(view, 240, 120, 1.0f, &f.width, &f.height);
    return f;
}

// Pixels in [x, x+w) x [0, 120) that differ from the empty render.
std::size_t changed_in_column(const Frame& before, const Frame& after, std::uint32_t x,
                              std::uint32_t w) {
    std::size_t changed = 0;
    for (std::uint32_t row = 0; row < after.height; ++row)
        for (std::uint32_t col = x; col < x + w; ++col) {
            const auto o = (static_cast<std::size_t>(row) * after.width + col) * 4;
            for (int c = 0; c < 3; ++c)
                if (before.pixels[o + c] != after.pixels[o + c]) {
                    ++changed;
                    break;
                }
        }
    return changed;
}

} // namespace

TEST_CASE("A script's createMeter, createWaveform and createSpectrum make the real widgets",
          "[pulp-vellum-audio][register][scripted-editor]") {
    ::vellum::view::clear_data_displays();
    register_audio_widgets();
    REQUIRE(::vellum::view::has_data_display("pulp.level"));
    REQUIRE(::vellum::view::has_data_display("pulp.series"));
    REQUIRE(::vellum::view::has_data_display("pulp.bins"));

    Mounted ui(kScript);
    INFO(ui.error);
    REQUIRE(ui.loaded);
    CHECK(ui.widget<LevelMeter>("m") != nullptr);
    CHECK(ui.widget<WaveformView>("w") != nullptr);
    CHECK(ui.widget<AnalyzerScope>("s") != nullptr);
}

TEST_CASE("Script data reaches the registered widgets and paints",
          "[pulp-vellum-audio][register][render]") {
    ::vellum::view::clear_data_displays();
    register_audio_widgets();

    Mounted ui(kDataScript);
    INFO(ui.error);
    REQUIRE(ui.loaded);
    auto* meter = ui.widget<LevelMeter>("m");
    auto* wave = ui.widget<WaveformView>("w");
    auto* scope = ui.widget<AnalyzerScope>("s");
    REQUIRE(meter);
    REQUIRE(wave);
    REQUIRE(scope);

    CHECK(meter->rms() == Catch::Approx(0.7f));
    CHECK(meter->peak() == Catch::Approx(0.9f));
    CHECK(wave->sample_count() == 256);
    CHECK(scope->column_count() > 0);

    // Three side-by-side 80 x 120 regions; each widget paints its own.
    Mounted empty(kScript);
    REQUIRE(empty.loaded);
    for (auto* m : {&ui, &empty}) {
        m->root.flex().direction = ::vellum::view::FlexDirection::row;
        for (auto* v : {static_cast<::vellum::view::View*>(m->widget<LevelMeter>("m")),
                        static_cast<::vellum::view::View*>(m->widget<WaveformView>("w")),
                        static_cast<::vellum::view::View*>(m->widget<AnalyzerScope>("s"))}) {
            v->flex().preferred_width = 80;
            v->flex().preferred_height = 120;
        }
    }
    const auto before = render(empty.root);
    const auto after = render(ui.root);
    CHECK(changed_in_column(before, after, 0, 80) > 200);
    CHECK(changed_in_column(before, after, 80, 80) > 200);
    // The spectrum is a thin rising line, so it changes fewer pixels.
    CHECK(changed_in_column(before, after, 160, 80) > 100);

    if (const char* dir = std::getenv("PULP_VELLUM_AUDIO_PNG_DIR"))
        CHECK(::vellum::view::render_to_file(
            ui.root, 240, 120, std::string(dir) + "/scripted-audio-displays.png", 4.0f));
}

TEST_CASE("Without a registered provider the script cannot create audio displays",
          "[pulp-vellum-audio][register][control]") {
    ::vellum::view::clear_data_displays();
    REQUIRE_FALSE(::vellum::view::has_data_display("pulp.level"));

    Mounted ui(kScript);
    CHECK_FALSE(ui.loaded);
    CHECK(ui.widget<LevelMeter>("m") == nullptr);

    // Registering again restores the displays after a clear.
    register_audio_widgets();
    CHECK(::vellum::view::has_data_display("pulp.series"));
    Mounted again(kScript);
    CHECK(again.loaded);
}
