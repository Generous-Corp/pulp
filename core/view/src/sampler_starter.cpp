#include <pulp/design/sampler_starter.hpp>

#include <pulp/design/design_system.hpp>  // apply_ink_signal + the umbrella widgets
#include <pulp/canvas/canvas.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace pulp::design {

using namespace pulp::view;

namespace {
using canvas::Color;

// Root that paints the app background; children carry explicit bounds so the
// flex/Yoga pass is suppressed (same approach as the widget gallery's root).
class SamplerRoot : public View {
public:
    void paint(canvas::Canvas& canvas) override {
        canvas.set_fill_color(resolve_color("bg.primary", Color::rgba8(22, 26, 33)));
        canvas.fill_rect(0, 0, bounds().width, bounds().height);
    }
    void layout_children() override {}
};

// A small synthetic decaying sine so the WaveformView shows a real shape in the
// starter / headless render without needing an audio file on disk.
std::vector<float> demo_waveform(size_t n = 1024) {
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        s[i] = std::sin(t * 48.0f) * std::exp(-3.0f * t);
    }
    return s;
}

}  // namespace

std::unique_ptr<View> build_sampler_starter(const Theme& theme) {
    auto root = std::make_unique<SamplerRoot>();
    root->set_theme(theme);

    const float W = kSamplerWidth, M = 24.0f;

    auto add = [&](std::unique_ptr<View> v, float x, float y, float w, float h) -> View* {
        v->set_bounds({x, y, w, h});
        View* p = v.get();
        root->add_child(std::move(v));
        return p;
    };

    // ── Header: title + format badge ────────────────────────────────────
    {
        auto title = std::make_unique<Label>("Pulp Sampler");
        title->set_font_size(20.0f);
        add(std::move(title), M, 18.0f, 280.0f, 26.0f);

        auto badge = std::make_unique<Badge>("VST3 · 48 kHz", Tone::info);
        add(std::move(badge), W - M - 130.0f, 20.0f, 130.0f, 22.0f);
    }

    // ── Sample waveform ─────────────────────────────────────────────────
    {
        auto wave = std::make_unique<WaveformView>();
        wave->set_data(demo_waveform());
        add(std::move(wave), M, 56.0f, W - 2 * M, 120.0f);
    }

    // ── ADSR + Gain knob row ────────────────────────────────────────────
    {
        const char* names[] = {"Attack", "Decay", "Sustain", "Release", "Gain"};
        const float values[] = {0.2f, 0.4f, 0.7f, 0.3f, 0.8f};
        const float kw = 96.0f;
        for (int i = 0; i < 5; ++i) {
            auto knob = std::make_unique<Knob>();
            knob->set_label(names[i]);
            knob->set_value(values[i]);
            add(std::move(knob), M + static_cast<float>(i) * (kw + 8.0f), 196.0f, kw, 88.0f);
        }
    }

    // ── Controls row: voices stepper, balance pan, output meter ──────────
    {
        auto voicesLabel = std::make_unique<Label>("Voices");
        voicesLabel->set_font_size(12.0f);
        add(std::move(voicesLabel), M, 300.0f, 80.0f, 16.0f);
        auto voices = std::make_unique<Stepper>();
        voices->set_range(1.0, 32.0);
        voices->set_step(1.0);
        voices->set_value(8.0);
        add(std::move(voices), M, 318.0f, 150.0f, 36.0f);

        auto panLabel = std::make_unique<Label>("Balance");
        panLabel->set_font_size(12.0f);
        add(std::move(panLabel), 200.0f, 300.0f, 80.0f, 16.0f);
        auto pan = std::make_unique<PanControl>();
        pan->set_value(0.0f);
        add(std::move(pan), 200.0f, 326.0f, 200.0f, 18.0f);

        auto meterLabel = std::make_unique<Label>("Output");
        meterLabel->set_font_size(12.0f);
        add(std::move(meterLabel), W - M - 120.0f, 300.0f, 120.0f, 16.0f);
        auto meter = std::make_unique<Meter>();
        meter->set_orientation(Meter::Orientation::horizontal);
        meter->set_level(0.6f, 0.85f);
        add(std::move(meter), W - M - 120.0f, 322.0f, 120.0f, 14.0f);
    }

    // ── Transport buttons ───────────────────────────────────────────────
    {
        auto load = std::make_unique<TextButton>("Load Sample");
        add(std::move(load), M, 384.0f, 130.0f, 34.0f);
        auto play = std::make_unique<TextButton>("Play");
        add(std::move(play), M + 138.0f, 384.0f, 90.0f, 34.0f);
    }

    root->set_bounds({0, 0, W, kSamplerHeight});
    return root;
}

}  // namespace pulp::design
