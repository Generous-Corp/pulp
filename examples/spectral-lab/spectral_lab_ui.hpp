#pragma once

// Native GPU UI for the Spectral Lab example processor, built on Pulp's view
// stack.
//
// Three live panels stacked vertically: a hero "cloud" panel that draws the
// frozen spectral layers as stacked bars lit by the live Morph weighting (so you
// can see which captured moment is loud as you scrub Morph), a log-frequency
// spectrum of the wet output, and a control strip with vertical sliders for Mix
// / Layers / Morph / Smear / Jitter plus an Engine (CPU/GPU) toggle stacked over
// a Freeze trigger.
//
// Layout is fully proportional to the view bounds so it scales with the host
// window. Pointer input drives parameters through proper host gestures so edits
// stick and record in the DAW; the slider handle tracks the in-flight edit value.

#include "spectral_lab.hpp"
#include <pulp/state/parameter_edit.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/theme_presets.hpp>
#include <pulp/canvas/canvas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pulp::examples {

namespace cv = pulp::canvas;
namespace vw = pulp::view;

// The UI palette is resolved from Pulp's "Ink & Signal" design language (the
// flagship token preset), not hardcoded — so the editor tracks a reskin. Each
// lookup falls back to a brand-matched constant so it still renders if a token
// is absent.
struct SlPalette {
    cv::Color bg, surface, elevated, border, text, text_dim, accent, accent_warm;
    cv::Color cloud_cold, cloud_hot, cloud_axis, spec_line, spec_fill;
    cv::Color slider_track, slider_fill, toggle_on;
};

inline SlPalette make_ink_signal_palette() {
    vw::Theme th;
    if (const auto* preset = vw::find_preset("ink-signal"))
        th = vw::theme_from_preset(*preset, /*dark=*/true);
    auto C = [&](const char* name, cv::Color fb) { return th.color(name).value_or(fb); };
    SlPalette p;
    p.bg           = C("bg.primary",      cv::Color::rgba8(18, 19, 30));
    p.surface      = C("bg.surface",      cv::Color::rgba8(30, 32, 48));
    p.elevated     = C("bg.elevated",     cv::Color::rgba8(46, 49, 70));
    p.border       = C("control.border",  cv::Color::rgba8(70, 74, 100));
    p.text         = C("text.primary",    cv::Color::rgba8(214, 221, 245));
    p.text_dim     = C("text.secondary",  cv::Color::rgba8(150, 158, 188));
    p.accent       = C("accent.primary",  cv::Color::rgba8(22, 218, 194));   // signal teal
    p.accent_warm  = C("accent.secondary", cv::Color::rgba8(139, 108, 245)); // ink violet
    // Layer cloud — cold (ink violet) for quiet layers, warming to signal teal /
    // amber where the Morph weighting makes a layer loud.
    p.cloud_cold   = C("accent.secondary", cv::Color::rgba8(139, 108, 245));
    p.cloud_hot    = C("accent.warning",  cv::Color::rgba8(246, 184, 71));
    p.cloud_axis   = C("waveform.grid",   cv::Color::rgba8(58, 62, 86));
    p.spec_line    = C("waveform.line",   cv::Color::rgba8(22, 218, 194));
    p.spec_fill    = p.spec_line.with_alpha(0.16f);
    p.slider_track = C("slider.track",    cv::Color::rgba8(40, 43, 62));
    p.slider_fill  = C("slider.fill",     p.accent).with_alpha(0.45f);
    p.toggle_on    = C("accent.warning",  cv::Color::rgba8(246, 184, 71));
    return p;
}

class SpectralLabUi : public vw::View {
public:
    SpectralLabUi(pulp::state::StateStore& store,
                  pulp::examples::SpectrumBus& spectrum,
                  pulp::examples::SpectralLabProcessor& proc)
        : store_(store), spectrum_(spectrum), proc_(proc), edit_(store) {
        set_continuous_repaint(true);
        set_requires_gpu_host(true);
    }

    void on_resized() override { layout(); }

    void paint(cv::Canvas& canvas) override {
        if (layout_dirty_) layout();
        const float W = local_bounds().width, H = local_bounds().height;
        const float s = scale();

        canvas.set_fill_color(pal_.bg);
        canvas.fill_rect(0, 0, W, H);

        canvas.set_fill_color(pal_.text);
        canvas.set_font("Inter", 21.0f * s);
        canvas.fill_text("Spectral Lab", 20 * s, 32 * s);

        // Subtitle carries the live engine status so it's always clear whether
        // the AUDIO is on the GPU (the UI is GPU-rendered either way).
        std::string audio_status = "Audio: CPU";
        const auto g = proc_.gpu_status();
        if (g.active) {
            audio_status = g.backend.empty() ? "Audio: GPU"
                                             : ("Audio: GPU · " + g.backend);
            audio_status += " · " + std::to_string(g.layers) + " layers";
            audio_status += " · " + std::to_string(g.blocks) + " blocks";
            if (g.misses > 0)
                audio_status += ", " + std::to_string(g.misses) + " misses";
            if (g.blocks > 0 && g.avg_us > 0.0) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), " · %.0f µs/block (%.0f%% of real-time)",
                              g.avg_us, g.rt_percent);
                audio_status += buf;
            }
        }
        canvas.set_fill_color(pal_.text_dim);
        canvas.set_font("Inter", 12.0f * s);
        canvas.fill_text("Spectral freeze + morph cloud · GPU-rendered UI · " + audio_status,
                         20 * s, 50 * s);

        read_spectrum();
        paint_cloud(canvas);
        paint_spectrum(canvas);
        paint_controls(canvas);

        request_repaint();  // self-driven loop for the live panels
    }

    void on_mouse_event(const vw::MouseEvent& e) override {
        switch (e.phase) {
            case vw::MousePhase::press:   pointer_press(e.position); break;
            case vw::MousePhase::drag:    pointer_move(e.position); break;
            case vw::MousePhase::release: pointer_release(); break;
            case vw::MousePhase::hover:   break;
            case vw::MousePhase::automatic:
                if (e.is_down) {
                    if (pointer_down_) pointer_move(e.position);
                    else pointer_press(e.position);
                }
                break;
        }
    }
    void on_mouse_down(vw::Point p) override { pointer_press(p); }
    void on_mouse_drag(vw::Point p) override { pointer_move(p); }
    void on_mouse_up(vw::Point) override { pointer_release(); }

    // Test accessors (headless interaction verification).
    void layout_for_test() { layout(); }
    vw::Point slider_center_for_test(int i) {
        if (layout_dirty_) layout();
        const auto& t = sliders_[static_cast<size_t>(i)].track;
        return {t.x + t.width * 0.5f, t.y + t.height * 0.5f};
    }
    vw::Point freeze_center_for_test() {
        if (layout_dirty_) layout();
        return {freeze_.x + freeze_.width * 0.5f, freeze_.y + freeze_.height * 0.5f};
    }
    vw::Point engine_center_for_test() {
        if (layout_dirty_) layout();
        return {engine_.x + engine_.width * 0.5f, engine_.y + engine_.height * 0.5f};
    }

private:
    struct Slider {
        pulp::state::ParamID id;
        const char* label;
        float lo, hi;
        int decimals;
        const char* unit;
        bool snap_int = false;   // round to a whole step (Layers)
        vw::Rect cell{};
        vw::Rect track{};
    };

    static bool in_rect(vw::Point p, const vw::Rect& r) {
        return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
    }
    float scale() const { return std::max(0.5f, local_bounds().height / 560.0f); }

    void layout() {
        const float W = local_bounds().width, H = local_bounds().height;
        const float s = scale();
        const float m = 20 * s, top = 64 * s;
        const float avail = H - top - m;

        const float cloud_h = avail * 0.42f;
        const float spec_h  = avail * 0.26f;
        const float ctl_h   = avail - cloud_h - spec_h - 2 * (12 * s);

        cloud_         = {m, top, W - 2 * m, cloud_h};
        spectrum_rect_ = {m, cloud_.bottom() + 12 * s, W - 2 * m, spec_h};
        controls_      = {m, spectrum_rect_.bottom() + 12 * s, W - 2 * m, ctl_h};

        // Six equal columns: five sliders (Mix/Layers/Morph/Smear/Jitter) + a
        // toggle column holding the Engine (CPU/GPU) toggle over a Freeze trigger.
        const float cw = controls_.width / 6.0f;
        const float label_h = 20 * s, value_h = 22 * s, pad = 10 * s;
        for (int i = 0; i < kNumSliders; ++i) {
            Slider& sl = sliders_[static_cast<size_t>(i)];
            sl.cell = {controls_.x + i * cw, controls_.y, cw, controls_.height};
            const float tw = std::min(16 * s, cw * 0.22f);
            const float tx = sl.cell.x + (cw - tw) * 0.5f;
            const float ty = sl.cell.y + label_h + pad;
            const float th = sl.cell.height - label_h - value_h - 2 * pad;
            sl.track = {tx, ty, tw, std::max(20.0f, th)};
        }
        const float bw = std::min(cw - 20 * s, 120 * s);
        const float bh = std::min(controls_.height * 0.34f, 48 * s);
        const float bx = controls_.x + kNumSliders * cw + (cw - bw) * 0.5f;
        const float gap = 12 * s;
        const float stack_h = 2 * bh + gap;
        const float y0 = controls_.y + (controls_.height - stack_h) * 0.5f;
        engine_ = {bx, y0, bw, bh};
        freeze_ = {bx, y0 + bh + gap, bw, bh};
        layout_dirty_ = false;
    }

    void read_spectrum() {
        const SpectrumFrame& frame = spectrum_.read();
        for (int i = 0; i < kSpectrumBins; ++i) {
            const float db = frame[static_cast<size_t>(i)];
            const float target = std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f);
            float& v = spec_display_[static_cast<size_t>(i)];
            v += (target > v ? 0.5f : 0.18f) * (target - v);  // fast rise, slow fall
        }
    }

    // ── layer cloud (hero) ──
    // Each captured layer is a vertical bar; height + tint follow the live Morph
    // weighting, so the loud layer glows as you scrub Morph across the chord.
    void paint_cloud(cv::Canvas& canvas) {
        const float s = scale();
        const auto& r = cloud_;
        canvas.set_fill_color(pal_.surface);
        canvas.fill_rounded_rect(r.x, r.y, r.width, r.height, 10 * s);
        canvas.set_fill_color(pal_.text_dim);
        canvas.set_font("Inter", 11.0f * s);
        canvas.fill_text("frozen layers (morph weighting)", r.x + 10 * s, r.y + 16 * s);

        canvas.save();
        canvas.clip_rect(r.x, r.y, r.width, r.height);

        const int layers = std::max(1, static_cast<int>(std::lround(
            slider_value(kLayersIdx))));
        const int captured = std::clamp(proc_.captured_layers(), 0, layers);
        const float morph = slider_value(kMorphIdx);

        std::vector<float> w(static_cast<size_t>(layers), 0.0f);
        spectral_morph_weights(w.data(), layers,
                               static_cast<std::uint32_t>(captured), morph);
        float wmax = 1e-6f;
        for (float v : w) wmax = std::max(wmax, v);

        const float x0 = r.x + 10 * s, xspan = r.width - 20 * s;
        const float base = r.bottom() - 10 * s, topY = r.y + 26 * s;
        const float span = base - topY;
        const float bw = std::max(2.0f, xspan / static_cast<float>(layers) * 0.7f);
        for (int i = 0; i < layers; ++i) {
            const float frac = (layers > 1)
                ? static_cast<float>(i) / static_cast<float>(layers - 1)
                : 0.0f;
            const float x = x0 + frac * (xspan - bw);
            const bool active = i < captured;
            const float t = active ? std::clamp(w[static_cast<size_t>(i)] / wmax, 0.0f, 1.0f)
                                   : 0.0f;
            // Quiet floor so uncaptured / low-weight layers are still visible.
            const float h = (active ? (0.12f + 0.88f * t) : 0.06f) * span;
            canvas.set_fill_color(active ? lerp_color(pal_.cloud_cold, pal_.cloud_hot, t)
                                         : pal_.cloud_axis);
            canvas.fill_rounded_rect(x, base - h, bw, h, std::min(3.0f * s, bw * 0.5f));
        }
        canvas.restore();
    }

    // ── spectrum panel (log-frequency dB curve) ──
    void paint_spectrum(cv::Canvas& canvas) {
        const float s = scale();
        const auto& r = spectrum_rect_;
        canvas.set_fill_color(pal_.surface);
        canvas.fill_rounded_rect(r.x, r.y, r.width, r.height, 8 * s);
        canvas.set_fill_color(pal_.text_dim);
        canvas.set_font("Inter", 11.0f * s);
        canvas.fill_text("output spectrum (log f)", r.x + 8 * s, r.y + 16 * s);

        canvas.save();
        canvas.clip_rect(r.x, r.y, r.width, r.height);
        const int n = kSpectrumBins;
        const float base = r.bottom() - 6 * s, topY = r.y + 22 * s;
        const float x0 = r.x + 6 * s, xspan = r.width - 12 * s;
        const float inv_logn = 1.0f / std::log10(static_cast<float>(n));
        std::array<cv::Canvas::Point2D, kSpectrumBins + 2> poly{};
        int pc = 0;
        for (int i = 1; i < n; ++i) {
            const float lf = std::log10(1.0f + i) * inv_logn;
            const float x = x0 + lf * xspan;
            const float y = base - spec_display_[static_cast<size_t>(i)] * (base - topY);
            poly[static_cast<size_t>(pc++)] = {x, y};
        }
        const int curve_pts = pc;
        poly[static_cast<size_t>(pc++)] = {x0 + xspan, base};
        poly[static_cast<size_t>(pc++)] = {x0, base};
        canvas.set_fill_color(pal_.spec_fill);
        canvas.fill_path(poly.data(), static_cast<size_t>(pc));
        canvas.set_stroke_color(pal_.spec_line);
        canvas.set_line_width(1.6f);
        canvas.stroke_path(poly.data(), static_cast<size_t>(curve_pts));
        canvas.restore();
    }

    // ── controls (vertical sliders + toggles) ──
    void paint_controls(cv::Canvas& canvas) {
        const float s = scale();
        canvas.set_fill_color(pal_.surface);
        canvas.fill_rounded_rect(controls_.x, controls_.y, controls_.width,
                                 controls_.height, 8 * s);

        for (int i = 0; i < kNumSliders; ++i) {
            const Slider& sl = sliders_[static_cast<size_t>(i)];
            const auto& t = sl.track;
            canvas.set_fill_color(pal_.text_dim);
            canvas.set_font("Inter", 12.0f * s);
            canvas.fill_text(sl.label, sl.cell.x + (sl.cell.width - 36 * s) * 0.5f,
                             sl.cell.y + 16 * s);

            const float frac = value_frac(i);
            const float handle_y = t.bottom() - frac * t.height;
            canvas.set_fill_color(pal_.slider_track);
            canvas.fill_rounded_rect(t.x, t.y, t.width, t.height, t.width * 0.5f);
            canvas.set_fill_color(pal_.slider_fill);
            canvas.fill_rounded_rect(t.x, handle_y, t.width, t.bottom() - handle_y,
                                     t.width * 0.5f);
            const bool active = (active_slider_ == i);
            canvas.set_fill_color(active ? pal_.accent_warm : pal_.accent);
            canvas.fill_circle(t.x + t.width * 0.5f, handle_y, t.width * 0.9f);

            char buf[40];
            std::snprintf(buf, sizeof buf, "%.*f%s%s", sl.decimals,
                          static_cast<double>(slider_value(i)),
                          sl.unit[0] ? " " : "", sl.unit);
            canvas.set_fill_color(pal_.text);
            canvas.set_font("Inter", 13.0f * s);
            canvas.fill_text(buf, sl.cell.x + (sl.cell.width - 44 * s) * 0.5f,
                             sl.cell.bottom() - 8 * s);
        }

        // Engine toggle (CPU / GPU). Lit when GPU is requested; the subtitle
        // reports whether the GPU is actually carrying the audio.
        const bool gpu_req = store_.get_value(kEngine) >= 0.5f;
        canvas.set_fill_color(gpu_req ? pal_.accent : pal_.elevated);
        canvas.fill_rounded_rect(engine_.x, engine_.y, engine_.width, engine_.height, 8 * s);
        canvas.set_fill_color(gpu_req ? pal_.bg : pal_.text);
        canvas.set_font("Inter", 14.0f * s);
        canvas.fill_text(gpu_req ? "● GPU" : "CPU",
                         engine_.x + 16 * s, engine_.y + engine_.height * 0.62f);

        // Freeze trigger. Lit while held (a capture latches on the rising edge).
        const bool frozen = store_.get_value(kFreeze) >= 0.5f;
        canvas.set_fill_color(frozen ? pal_.toggle_on : pal_.elevated);
        canvas.fill_rounded_rect(freeze_.x, freeze_.y, freeze_.width, freeze_.height, 8 * s);
        canvas.set_fill_color(frozen ? pal_.bg : pal_.text);
        canvas.set_font("Inter", 14.0f * s);
        canvas.fill_text(frozen ? "● FREEZE" : "FREEZE",
                         freeze_.x + 16 * s, freeze_.y + freeze_.height * 0.62f);
    }

    // ── interaction ──
    void pointer_press(vw::Point p) {
        if (pointer_down_) return;
        if (layout_dirty_) layout();
        pointer_down_ = true;

        if (in_rect(p, freeze_)) { toggle_param(kFreeze); return; }
        if (in_rect(p, engine_)) { toggle_param(kEngine); return; }
        for (int i = 0; i < kNumSliders; ++i) {
            if (in_rect(p, sliders_[static_cast<size_t>(i)].cell)) {
                active_slider_ = i;
                edit_.begin(sliders_[static_cast<size_t>(i)].id);
                apply_slider(p);
                return;
            }
        }
    }
    void pointer_move(vw::Point p) {
        if (active_slider_ >= 0) apply_slider(p);
    }
    void pointer_release() {
        if (!pointer_down_) return;
        pointer_down_ = false;
        if (active_slider_ >= 0) {
            edit_.finish();
            active_slider_ = -1;
        }
    }
    void apply_slider(vw::Point p) {
        const Slider& sl = sliders_[static_cast<size_t>(active_slider_)];
        const auto& t = sl.track;
        const float frac = std::clamp((t.bottom() - p.y) / t.height, 0.0f, 1.0f);
        float v = sl.lo + frac * (sl.hi - sl.lo);
        if (sl.snap_int) v = std::round(v);
        edit_.set(sl.id, v);
    }

    void toggle_param(pulp::state::ParamID id) {
        const float v = store_.get_value(id) >= 0.5f ? 0.0f : 1.0f;
        pulp::state::ParameterEdit toggle(store_);
        toggle.begin(id);
        toggle.set(id, v);
        toggle.finish();
    }

    float slider_value(int i) const {
        const Slider& sl = sliders_[static_cast<size_t>(i)];
        if (i == active_slider_) return edit_.display_value(sl.id, store_.get_value(sl.id));
        return store_.get_value(sl.id);
    }
    float value_frac(int i) const {
        const Slider& sl = sliders_[static_cast<size_t>(i)];
        return std::clamp((slider_value(i) - sl.lo) / (sl.hi - sl.lo), 0.0f, 1.0f);
    }

    static cv::Color lerp_color(cv::Color a, cv::Color b, float t) {
        return a.interpolate(b, std::clamp(t, 0.0f, 1.0f));
    }

    pulp::state::StateStore& store_;
    pulp::examples::SpectrumBus& spectrum_;
    pulp::examples::SpectralLabProcessor& proc_;
    pulp::state::ParameterEdit edit_;
    SlPalette pal_ = make_ink_signal_palette();

    static constexpr int kNumSliders = 5;
    static constexpr int kLayersIdx = 1;
    static constexpr int kMorphIdx = 2;

    vw::Rect cloud_{}, spectrum_rect_{}, controls_{}, freeze_{}, engine_{};
    std::array<Slider, kNumSliders> sliders_{{
        {kMix,    "Mix",    0.0f, 100.0f, 0, "%"},
        {kLayers, "Layers", 1.0f, 128.0f, 0, "", true},
        {kMorph,  "Morph",  0.0f, 1.0f,   2, ""},
        {kSmear,  "Smear",  0.0f, 1.0f,   2, ""},
        {kJitter, "Jitter", 0.0f, 1.0f,   2, ""},
    }};
    std::array<float, kSpectrumBins> spec_display_{};
    int active_slider_ = -1;
    bool pointer_down_ = false;
    bool layout_dirty_ = true;
};

} // namespace pulp::examples
