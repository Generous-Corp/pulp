// Component shots — renders each core Ink & Signal primitive in isolation, in
// light and dark, to PNG cells, for a Figma-vs-Pulp comparison contact sheet.
// One-off verification tool (not a shipped feature surface).
//
//   pulp-component-shots --out /tmp/shots/native
//
// Writes <out>/<name>-light.png and <out>/<name>-dark.png per component.

#include <pulp/design/design_system.hpp>
#include <pulp/view/screenshot.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::design;
using namespace pulp::view;
using pulp::canvas::Color;
namespace canvas = pulp::canvas;

namespace {

// A fixed-size cell that paints the theme app background; children carry
// explicit bounds (suppress the flex pass).
class Cell : public View {
public:
    void paint(canvas::Canvas& c) override {
        c.set_fill_color(resolve_color("bg.primary", Color::rgba8(22, 26, 33)));
        c.fill_rect(0, 0, bounds().width, bounds().height);
    }
    void layout_children() override {}
};

std::vector<float> demo_wave(size_t n = 1024) {
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        s[i] = std::sin(t * 48.0f) * std::exp(-3.0f * t);
    }
    return s;
}

std::vector<float> demo_spectrum(size_t n = 48) {
    std::vector<float> m(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        m[i] = -6.0f - 36.0f * t + 8.0f * std::sin(t * 18.0f);
    }
    return m;
}

struct Entry {
    std::string name;
    float w, h;
    std::function<void(Cell&)> build;   // add widget(s) with explicit bounds
};

std::vector<Entry> entries() {
    std::vector<Entry> e;
    auto add = [&](std::string n, float w, float h, std::function<void(Cell&)> b) {
        e.push_back({std::move(n), w, h, std::move(b)});
    };

    add("Knob", 140, 140, [](Cell& c) {
        auto k = std::make_unique<Knob>(); k->set_value(0.62f); k->set_label("Cutoff");
        k->set_bounds({22, 16, 96, 96}); c.add_child(std::move(k));
    });
    add("Fader", 90, 160, [](Cell& c) {
        auto f = std::make_unique<Fader>(); f->set_value(0.62f);
        f->set_bounds({32, 20, 26, 120}); c.add_child(std::move(f));
    });
    add("Slider", 260, 70, [](Cell& c) {
        auto s = std::make_unique<RangeSlider>(); s->set_min(0); s->set_max(1); s->set_value(0.4f);
        s->set_bounds({20, 26, 220, 18}); c.add_child(std::move(s));
    });
    add("Toggle", 110, 70, [](Cell& c) {
        auto t = std::make_unique<Toggle>(); t->set_on(true);
        t->advance_animations(1.0f);   // settle thumb to target for static render
        t->set_bounds({29, 20, 52, 30}); c.add_child(std::move(t));
    });
    add("Checkbox", 90, 70, [](Cell& c) {
        auto cb = std::make_unique<Checkbox>(); cb->set_checked(true);
        cb->set_bounds({34, 24, 22, 22}); c.add_child(std::move(cb));
    });
    add("Button", 170, 70, [](Cell& c) {
        auto b = std::make_unique<TextButton>("Render");
        b->set_style(TextButton::Style::primary);
        b->set_bounds({30, 17, 110, 36}); c.add_child(std::move(b));
    });
    add("Badge", 150, 210, [](Cell& c) {
        const char* labels[] = {"Neutral", "Info", "Active", "48 kHz", "Peak"};
        const Tone tones[] = {Tone::neutral, Tone::info, Tone::success,
                              Tone::warning, Tone::danger};
        float y = 16;
        for (int i = 0; i < 5; ++i) {
            auto bd = std::make_unique<Badge>(labels[i], tones[i]);
            bd->set_bounds({34, y, 82, 24}); c.add_child(std::move(bd));
            y += 36;
        }
    });
    add("Meter", 300, 130, [](Cell& c) {
        // Three horizontal meters at Low/Mid/Peak levels (green/yellow/red
        // zones), matching the Figma Meter board.
        const float lvls[] = {0.45f, 0.8f, 0.97f};
        float y = 24;
        for (int i = 0; i < 3; ++i) {
            auto m = std::make_unique<Meter>();
            m->set_orientation(Meter::Orientation::horizontal);
            m->set_level(lvls[i], lvls[i]);
            m->set_bounds({24, y, 250, 14}); c.add_child(std::move(m));
            y += 34;
        }
    });
    add("ProgressBar", 260, 60, [](Cell& c) {
        auto p = std::make_unique<ProgressBar>(); p->set_progress(0.6f);
        p->set_bounds({20, 25, 220, 10}); c.add_child(std::move(p));
    });
    add("ComboBox", 220, 70, [](Cell& c) {
        auto cb = std::make_unique<ComboBox>();
        cb->set_items({"Sine", "Saw", "Square"}); cb->set_selected(1);
        cb->set_bounds({20, 19, 180, 32}); c.add_child(std::move(cb));
    });
    add("Stepper", 200, 70, [](Cell& c) {
        auto s = std::make_unique<Stepper>(); s->set_range(-24, 24); s->set_value(2); s->set_suffix("st");
        s->set_bounds({30, 17, 140, 36}); c.add_child(std::move(s));
    });
    add("Pan", 260, 60, [](Cell& c) {
        auto p = std::make_unique<PanControl>(); p->set_value(-0.4f);
        p->set_bounds({30, 21, 200, 18}); c.add_child(std::move(p));
    });
    add("Tab", 300, 90, [](Cell& c) {
        auto t = std::make_unique<TabPanel>();
        t->add_tab("Amp", std::make_unique<View>());
        t->add_tab("Filter", std::make_unique<View>());
        t->add_tab("FX", std::make_unique<View>());
        t->set_active_tab(0);
        t->set_bounds({20, 16, 260, 58}); c.add_child(std::move(t));
    });
    add("Spectrum", 300, 140, [](Cell& c) {
        auto s = std::make_unique<SpectrumView>(); s->set_spectrum(demo_spectrum());
        s->set_bounds({20, 18, 260, 104}); c.add_child(std::move(s));
    });
    add("Waveform", 300, 140, [](Cell& c) {
        auto w = std::make_unique<WaveformView>(); w->set_data(demo_wave());
        w->set_bounds({20, 18, 260, 104}); c.add_child(std::move(w));
    });
    add("XYPad", 180, 150, [](Cell& c) {
        auto p = std::make_unique<XYPad>();
        p->set_bounds({30, 18, 120, 114}); c.add_child(std::move(p));
    });
    add("InlineBanner", 420, 80, [](Cell& c) {
        auto b = std::make_unique<InlineBanner>(); b->set_tone(Tone::success);
        b->set_label("Build succeeded."); b->set_message("VST3 · AU · CLAP signed in 4.8s.");
        b->set_bounds({20, 17, 380, 46}); c.add_child(std::move(b));
    });
    add("Toast", 420, 100, [](Cell& c) {
        auto t = std::make_unique<Toast>(); t->set_title("Preset saved");
        t->set_subtitle("Velvet Plate → User library"); t->set_action("Undo");
        t->set_bounds({20, 18, 380, 64}); c.add_child(std::move(t));
    });
    add("EmptyState", 420, 100, [](Cell& c) {
        auto es = std::make_unique<EmptyState>(); es->set_message("No presets yet —"); es->set_action("create one");
        es->set_bounds({20, 18, 380, 64}); c.add_child(std::move(es));
    });
    add("Input", 240, 70, [](Cell& c) {
        auto ed = std::make_unique<TextEditor>(); ed->set_text("Cutoff");
        ed->set_bounds({20, 19, 200, 32}); c.add_child(std::move(ed));
    });
    return e;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out = "shots";
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];

    bool ok = true;
    for (const auto& entry : entries()) {
        for (bool dark : {false, true}) {
            auto cell = std::make_unique<Cell>();
            cell->set_theme(ink_signal_theme(dark));
            cell->set_bounds({0, 0, entry.w, entry.h});
            entry.build(*cell);
            const auto W = static_cast<uint32_t>(entry.w);
            const auto H = static_cast<uint32_t>(entry.h);
            const std::string path = out + "/" + entry.name + (dark ? "-dark.png" : "-light.png");
            const bool wrote = render_to_file(*cell, W, H, path, 2.0f,
                                              ScreenshotBackend::default_backend);
            std::printf("%s %s\n", wrote ? "wrote" : "FAILED", path.c_str());
            ok &= wrote;
        }
    }
    return ok ? 0 : 1;
}
