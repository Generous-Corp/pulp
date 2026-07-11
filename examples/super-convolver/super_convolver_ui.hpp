#pragma once

// Native GPU UI for the SuperConvolver example processor, built on Pulp's
// view stack.
//
// Three live panels stacked vertically: a hero impulse-response waveform that
// redraws from the processor's current IR snapshot (so it visibly morphs as the
// Size knob rebuilds the reverb tail or a loaded file replaces it), a
// log-frequency spectrum of the wet output, and a control strip with vertical
// sliders for Mix / Size / Gain plus a Bypass toggle. A "Load IR" button in the
// header opens the native file picker to import a WAV/AIFF/FLAC impulse response
// (the loaded base IR; Rooms then decorrelates it) and shows the current source
// name. The IR panel is tinted column-by-column by the live spectrum — a cheap
// per-frame cross-feed that leans on the GPU host's smooth repaint.
//
// Layout is fully proportional to the view bounds so it scales with the host
// window and never clips. Pointer input drives parameters through proper host
// gestures (begin/set/finish) so edits stick and record in the DAW; the slider
// handle tracks the in-flight edit value so it follows the cursor regardless of
// the audio thread's per-block parameter echo.

#include "super_convolver.hpp"
#include "field_renderer.hpp"
#include <pulp/state/parameter_edit.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/file_chooser.hpp>
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
// flagship token preset), not hardcoded — so the editor is wired to the same
// semantic tokens (bg/text/accent/waveform/slider/meter…) the rest of the
// system uses and would track a reskin or appearance change. Each lookup falls
// back to a brand-matched constant so it still renders if a token is absent.
struct ScPalette {
    cv::Color bg, surface, elevated, border, text, text_dim, accent, accent_warm;
    cv::Color ir_cold, ir_hot, ir_axis, spec_line, spec_fill;
    cv::Color slider_track, slider_fill, bypass_on;
};

inline ScPalette make_ink_signal_palette() {
    vw::Theme th;
    if (const auto* preset = vw::find_preset("ink-signal"))
        th = vw::theme_from_preset(*preset, /*dark=*/true);
    auto C = [&](const char* name, cv::Color fb) { return th.color(name).value_or(fb); };
    ScPalette p;
    p.bg           = C("bg.primary",      cv::Color::rgba8(18, 19, 30));
    p.surface      = C("bg.surface",      cv::Color::rgba8(30, 32, 48));
    p.elevated     = C("bg.elevated",     cv::Color::rgba8(46, 49, 70));
    p.border       = C("control.border",  cv::Color::rgba8(70, 74, 100));
    p.text         = C("text.primary",    cv::Color::rgba8(214, 221, 245));
    p.text_dim     = C("text.secondary",  cv::Color::rgba8(150, 158, 188));
    p.accent       = C("accent.primary",  cv::Color::rgba8(22, 218, 194));   // signal teal
    p.accent_warm  = C("accent.secondary", cv::Color::rgba8(139, 108, 245)); // ink violet
    // IR waveform — signal-teal base that warms toward amber where the spectrum
    // is hot (the live cross-feed).
    p.ir_cold      = C("waveform.line",   cv::Color::rgba8(22, 218, 194));
    p.ir_hot       = C("accent.warning",  cv::Color::rgba8(246, 184, 71));
    p.ir_axis      = C("waveform.grid",   cv::Color::rgba8(58, 62, 86));
    p.spec_line    = C("waveform.line",   cv::Color::rgba8(22, 218, 194));
    p.spec_fill    = p.spec_line.with_alpha(0.16f);
    p.slider_track = C("slider.track",    cv::Color::rgba8(40, 43, 62));
    p.slider_fill  = C("slider.fill",     p.accent).with_alpha(0.45f);
    p.bypass_on    = C("accent.warning",  cv::Color::rgba8(246, 184, 71));
    return p;
}

class SuperConvolverUi : public vw::View {
public:
    SuperConvolverUi(pulp::state::StateStore& store,
                     pulp::examples::SpectrumBus& spectrum,
                     pulp::examples::SuperConvolverProcessor& proc)
        : store_(store), spectrum_(spectrum), proc_(proc), edit_(store) {
        // Live content (IR snapshot, spectrum curve, automation echo) → repaint
        // every frame. Prefer the GPU editor host for smooth rendering; the SDK
        // falls back to the CPU host if GPU init isn't available in the DAW.
        set_continuous_repaint(true);
        set_requires_gpu_host(true);
    }

    void on_resized() override { layout(); }

    void paint(cv::Canvas& canvas) override {
        if (layout_dirty_) layout();
        const float W = local_bounds().width, H = local_bounds().height;
        const float s = scale();

        // Deep ground for the field to glow against.
        canvas.set_fill_color(cv::Color::rgba8(7, 9, 14));
        canvas.fill_rect(0, 0, W, H);

        // The living acoustic field — the plugin's hero, drawn as the backdrop.
        // Header, status, and controls float on top. Emitter count = Rooms
        // (capped for a smooth 60fps), motion = Flow, brightness = live energy.
        read_spectrum();
        field_time_ += 1.0 / 60.0;
        const float field_flow = std::clamp(
            static_cast<float>(store_.get_value(kFlow)) * 0.01f, 0.0f, 1.0f);
        const int field_density = std::min(96,
            std::max(1, static_cast<int>(std::lround(store_.get_value(kRooms)))));
        pulp::superconvolver::draw_acoustic_field(
            canvas, 0, 0, W, H, field_time_, field_flow, field_density, overall_energy(),
            viz_mode_);
        canvas.set_blend_mode(cv::Canvas::BlendMode::normal);  // ensure chrome is not additive

        // Wordmark — tracked caps in bone #BCC2CC; info glyph after it.
        canvas.set_fill_color(cv::Color::rgba8(188, 194, 204));   // --bone
        canvas.set_font("Inter", 11.0f * s);
        const float mk_end = tracked_text(canvas, "SuperConvolver", 24 * s, 30 * s, 11.0f * s, 0.24f);
        // Info glyph — a 19px circle with a dim border and a centered "i", set
        // off from the wordmark by a clear gap (concept .info token).
        const float ix = mk_end + 18 * s, iy = 25.5f * s, ir = 9.5f * s;
        canvas.set_stroke_color(cv::Color::rgba8(220, 228, 238, 56));
        canvas.set_line_width(1.0f);
        canvas.stroke_circle(ix, iy, ir);
        canvas.set_fill_color(tk_label());
        canvas.set_font("Inter", 11.0f * s);
        centered_text(canvas, "i", ix, iy + 4.0f * s, 11.0f * s);

        const auto g = proc_.gpu_status();

        // Engine chip — top-right, tap to toggle CPU/GPU (concept tokens). Dot is
        // cyan for GPU / amber for CPU; the emitter cap is 128 (GPU) / 20 (CPU).
        {
            (void)g;
            const float rx = local_bounds().width - 24 * s;
            const bool gpu = store_.get_value(kEngine) >= 0.5f;
            const char* eng = gpu ? "GPU" : "CPU";
            const int cap = gpu ? 128 : 20;
            const int now = static_cast<int>(std::lround(store_.get_value(kRooms)));
            canvas.set_fill_color(tk_text());
            canvas.set_font("Inter", 12.0f * s);
            right_text(canvas, eng, rx, 29 * s, 12.0f * s);
            // Status dot with a soft glow.
            const float dotx = rx - static_cast<float>(std::string(eng).size()) * 12.0f * s * 0.52f - 10 * s;
            const cv::Color dot = gpu ? cv::Color::rgba8(175, 206, 220)   // --cyan
                                      : cv::Color::rgba8(231, 199, 154);  // --amber
            canvas.set_blend_mode(cv::Canvas::BlendMode::lighter);
            canvas.set_fill_color(dot.with_alpha(0.5f));
            canvas.fill_circle(dotx, 25 * s, 6.0f * s);
            canvas.set_blend_mode(cv::Canvas::BlendMode::normal);
            canvas.set_fill_color(dot);
            canvas.fill_circle(dotx, 25 * s, 3.0f * s);
            // "N / CAP emitters": numbers bone, "emitters" dim.
            canvas.set_fill_color(tk_label());
            canvas.set_font("Inter", 10.0f * s);
            right_text(canvas, "emitters", rx, 45 * s, 10.0f * s);
            char nb[24]; std::snprintf(nb, sizeof nb, "%d / %d", now, cap);
            canvas.set_fill_color(cv::Color::rgba8(188, 194, 204));  // --bone
            right_text(canvas, nb, rx - 8.0f * 10.0f * s * 0.52f - 4 * s, 45 * s, 10.0f * s);
        }

        // Mode tabs (Tracers / Currents / Field) — segmented pill, concept tokens.
        {
            static const char* kTabs[3] = {"Tracers", "Currents", "Field"};
            const float pad = 3 * s;
            const float cx0 = tabs_[0].x - pad, cy0 = tabs_[0].y - pad;
            const float cw = (tabs_[2].x + tabs_[2].width) - tabs_[0].x + 2 * pad;
            const float chh = tabs_[0].height + 2 * pad;
            canvas.set_fill_color(cv::Color::rgba8(220, 228, 238, 8));
            canvas.fill_rounded_rect(cx0, cy0, cw, chh, 9 * s);
            canvas.set_stroke_color(cv::Color::rgba8(220, 228, 238, 26));
            canvas.set_line_width(1.0f);
            canvas.stroke_rounded_rect(cx0, cy0, cw, chh, 9 * s);
            for (int i = 0; i < 3; ++i) {
                const auto& r = tabs_[static_cast<size_t>(i)];
                const bool on = (viz_mode_ == i);
                if (on) {
                    canvas.set_fill_color(cv::Color::rgba8(220, 228, 238, 26));
                    canvas.fill_rounded_rect(r.x, r.y, r.width, r.height, 7 * s);
                }
                canvas.set_fill_color(on ? tk_text() : tk_label());
                canvas.set_font("Inter", 10.5f * s);
                centered_text(canvas, kTabs[i], r.x + r.width * 0.5f, r.y + r.height * 0.64f, 10.5f * s);
            }
        }

        // Source chip (bordered) — the loaded IR, first-class and clickable to
        // load. Rect matches load_ir_btn_ (set in layout) for hit-testing.
        {
            const std::string p = proc_.ir_path();
            const std::string name = p.empty() ? std::string("Synthetic room")
                : pulp::superconvolver::clean_source_name(p);
            const auto& r = load_ir_btn_;
            canvas.set_fill_color(cv::Color::rgba8(220, 228, 238, 8));
            canvas.fill_rounded_rect(r.x, r.y, r.width, r.height, 10 * s);
            canvas.set_stroke_color(cv::Color::rgba8(220, 228, 238, 26));
            canvas.set_line_width(1.0f);
            canvas.stroke_rounded_rect(r.x, r.y, r.width, r.height, 10 * s);
            canvas.set_fill_color(cv::Color::rgba8(232, 240, 248));  // white orb
            canvas.fill_circle(r.x + 18 * s, r.y + r.height * 0.5f, 7 * s);
            canvas.set_fill_color(tk_text());
            canvas.set_font("Inter", 12.0f * s);
            canvas.fill_text(name, r.x + 34 * s, r.y + r.height * 0.46f);
            canvas.set_fill_color(cv::Color::rgba8(71, 76, 85));  // faint
            canvas.set_font("Inter", 8.5f * s);
            tracked_text(canvas, "Source", r.x + 34 * s, r.y + r.height * 0.82f, 8.5f * s, 0.2f);
        }

        paint_controls(canvas);

        request_repaint();  // self-driven loop for the live field
    }

    void on_mouse_event(const vw::MouseEvent& e) override {
        switch (e.phase) {
            case vw::MousePhase::press:   pointer_press(e.position); break;
            case vw::MousePhase::drag:    pointer_move(e.position); break;
            case vw::MousePhase::release: pointer_release(); break;
            case vw::MousePhase::hover:   break;
            case vw::MousePhase::automatic:
                // Ambiguous legacy convention: is_down=true opens (or, if
                // already open, ticks) the gesture. is_down=false is ignored —
                // the reliable release arrives via the explicit release phase
                // or on_mouse_up, so a spurious mid-drag up never ends a drag.
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
    vw::Rect ir_rect_for_test() { if (layout_dirty_) layout(); return ir_; }
    vw::Point slider_center_for_test(int i) {
        if (layout_dirty_) layout();
        const auto& t = sliders_[static_cast<size_t>(i)].track;
        return {t.x + t.width * 0.5f, t.y + t.height * 0.5f};
    }
    vw::Point bypass_center_for_test() {
        if (layout_dirty_) layout();
        return {bypass_.x + bypass_.width * 0.5f, bypass_.y + bypass_.height * 0.5f};
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
        bool snap_int = false;   // round to a whole step (Rooms)
        vw::Rect cell{};   // full clickable column
        vw::Rect track{};  // draggable vertical track
    };

    static bool in_rect(vw::Point p, const vw::Rect& r) {
        return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
    }
    float scale() const { return std::max(0.5f, local_bounds().height / 560.0f); }

    std::array<Slider, 5>& sliders() { return sliders_; }

    void layout() {
        const float W = local_bounds().width, H = local_bounds().height;
        const float s = scale();
        const float m = 20 * s, top = 64 * s;
        const float avail = H - top - m;

        // The Source chip (top-left, below the wordmark) IS the loader — click it
        // to open the picker. Rect shared by paint (draw) and pointer (hit-test).
        load_ir_btn_ = {24 * s, 44 * s, 196 * s, 38 * s};

        // Hero IR waveform (largest), then spectrum, then the control strip.
        const float ir_h   = avail * 0.42f;
        const float spec_h = avail * 0.26f;
        const float ctl_h  = avail - ir_h - spec_h - 2 * (12 * s);

        ir_       = {m, top, W - 2 * m, ir_h};
        spectrum_rect_ = {m, ir_.bottom() + 12 * s, W - 2 * m, spec_h};
        controls_ = {m, spectrum_rect_.bottom() + 12 * s, W - 2 * m, ctl_h};

        // Five equal columns: four sliders (Mix/Size/Gain/Rooms) + a toggle
        // column holding the Engine (CPU/GPU) toggle stacked over Bypass.
        const float cw = controls_.width / 5.0f;  // five sliders fill the row
        const float label_h = 20 * s, value_h = 22 * s, pad = 10 * s;
        for (int i = 0; i < 5; ++i) {
            Slider& sl = sliders_[static_cast<size_t>(i)];
            sl.cell = {controls_.x + i * cw, controls_.y, cw, controls_.height};
            (void)label_h; (void)value_h; (void)pad;
            // Horizontal track: full cell width (minus padding), thin, vertically
            // centered — the whole row of controls sits on one horizon.
            const float hpad = 14 * s, th = 4 * s;
            sl.track = {sl.cell.x + hpad, sl.cell.y + sl.cell.height * 0.54f,
                        cw - 2 * hpad, th};
        }
        // Engine is the top-right chip (tap to toggle CPU/GPU); Bypass lives in
        // the host chrome. Mode tabs are three equal cells centered at the top.
        engine_ = {W - 200 * s, 12 * s, 176 * s, 44 * s};
        bypass_ = {};
        const float tab_w = 92 * s, tab_h = 26 * s, tab_y = 16 * s;
        for (int i = 0; i < 3; ++i)
            tabs_[static_cast<size_t>(i)] =
                {W * 0.5f - tab_w * 1.5f + i * tab_w, tab_y, tab_w, tab_h};
        layout_dirty_ = false;
    }

    // ── spectrum read + smoothing (shared by the spectrum panel and the IR
    //    tint) ──
    void read_spectrum() {
        const SpectrumFrame& frame = spectrum_.read();
        for (int i = 0; i < kSpectrumBins; ++i) {
            const float db = frame[static_cast<size_t>(i)];
            const float target = std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f);
            float& v = spec_display_[static_cast<size_t>(i)];
            v += (target > v ? 0.5f : 0.18f) * (target - v);  // fast rise, slow fall
        }
    }
    float spectrum_energy_at(float frac) const {
        const int idx = std::clamp(static_cast<int>(frac * (kSpectrumBins - 1)),
                                   0, kSpectrumBins - 1);
        return spec_display_[static_cast<size_t>(idx)];
    }
    // Overall live level (a few bands averaged) → the field's brightness/swell.
    float overall_energy() const {
        float e = 0.0f; int n = 0;
        for (float f = 0.08f; f < 0.95f; f += 0.13f) { e += spectrum_energy_at(f); ++n; }
        return n ? std::clamp(e / static_cast<float>(n), 0.0f, 1.0f) : 0.0f;
    }

    // ── Load-IR button + current source name (header) ──
    static std::string basename_of(const std::string& path) {
        const auto slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    void paint_load_ir(cv::Canvas& canvas) {
        const float s = scale();
        const auto& b = load_ir_btn_;
        canvas.set_fill_color(pal_.elevated);
        canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 6 * s);
        canvas.set_stroke_color(pal_.border);
        canvas.set_line_width(1.0f);
        canvas.stroke_rounded_rect(b.x, b.y, b.width, b.height, 6 * s);
        canvas.set_fill_color(pal_.accent);
        canvas.set_font("Inter", 12.0f * s);
        canvas.fill_text("⤓ Load IR", b.x + 14 * s, b.y + b.height * 0.66f);

    }

    // The current IR source name (drawn in the IR panel header, where there's
    // dedicated space — keeps it clear of the long engine-status subtitle).
    std::string ir_source_name() const {
        const std::string path = proc_.ir_path();
        return path.empty() ? "built-in synthetic" : basename_of(path);
    }

    // Open the native file picker and set the chosen file as the IR source. The
    // dialog is synchronous on macOS (modal on the UI thread), so capturing the
    // processor by reference in the callback is safe.
    void open_ir_chooser() {
        vw::FileChooser chooser;
        chooser.set_title("Load impulse response")
               .add_extension_filter("Impulse response (WAV/AIFF/FLAC)",
                                      "wav;aiff;aif;flac");
        auto& proc = proc_;
        chooser.open([&proc](std::vector<std::string> paths) {
            // load_ir_path (not set_ir_path) forces a reload even when the user
            // re-picks the same file — e.g. it changed on disk, or a previously
            // missing path has since appeared.
            if (!paths.empty()) proc.load_ir_path(paths.front());
        });
    }

    // ── IR waveform (hero) ──
    void paint_ir(cv::Canvas& canvas) {
        const float s = scale();
        const auto& r = ir_;
        canvas.set_fill_color(pal_.surface);
        canvas.fill_rounded_rect(r.x, r.y, r.width, r.height, 10 * s);
        canvas.set_fill_color(pal_.text_dim);
        canvas.set_font("Inter", 11.0f * s);
        canvas.fill_text("impulse response", r.x + 10 * s, r.y + 16 * s);

        // Current IR source, right side of the panel header.
        {
            const float lx = r.x + r.width * 0.45f;
            canvas.save();
            canvas.clip_rect(lx, r.y, r.width * 0.55f - 10 * s, 22 * s);
            canvas.set_fill_color(pal_.text_dim);
            canvas.set_font("Inter", 11.0f * s);
            canvas.fill_text("source: " + ir_source_name(), lx, r.y + 16 * s);
            canvas.restore();
        }

        canvas.save();
        canvas.clip_rect(r.x, r.y, r.width, r.height);

        const float x0 = r.x + 8 * s, xspan = r.width - 16 * s;
        const float top = r.y + 24 * s, bot = r.bottom() - 8 * s;
        const float midY = (top + bot) * 0.5f, halfH = (bot - top) * 0.5f;

        // Center axis.
        canvas.set_stroke_color(pal_.ir_axis);
        canvas.set_line_width(1.0f);
        canvas.stroke_line(x0, midY, x0 + xspan, midY);

        const std::vector<float> ir = proc_.impulse_response_snapshot();
        const int cols = std::max(1, static_cast<int>(xspan));
        const std::size_t n = ir.size();
        if (n == 0) { canvas.restore(); return; }

        // Display peak-normalization: scale the drawn waveform to the panel by its
        // OWN max-abs sample so it always fills the panel regardless of the IR's
        // absolute level. This is purely visual — independent of the audio-path
        // unit-energy normalization (peak ~0.12, which would draw tiny) and of an
        // arbitrary loaded file's unpredictable level. Audio is unaffected.
        float ir_peak = 0.0f;
        for (float v : ir) ir_peak = std::max(ir_peak, std::abs(v));
        const float disp_gain = ir_peak > 1e-6f ? 1.0f / ir_peak : 1.0f;

        // Peak-pick the (possibly thousands-long) IR into one column per pixel,
        // mirrored about the center axis, tinted by the live spectrum energy at
        // the matching position.
        canvas.set_line_width(std::max(1.0f, xspan / static_cast<float>(cols)));
        for (int c = 0; c < cols; ++c) {
            const std::size_t start = static_cast<std::size_t>(
                static_cast<double>(c) * n / cols);
            std::size_t end = static_cast<std::size_t>(
                static_cast<double>(c + 1) * n / cols);
            if (end <= start) end = start + 1;
            if (end > n) end = n;
            float peak = 0.0f;
            for (std::size_t i = start; i < end; ++i)
                peak = std::max(peak, std::abs(ir[i]));
            const float frac = static_cast<float>(c) / static_cast<float>(cols);
            const float amp = std::clamp(peak * disp_gain, 0.0f, 1.0f) * halfH * 0.94f;
            const float x = x0 + frac * xspan;
            const float t = spectrum_energy_at(frac);  // 0..1 cold→hot tint
            canvas.set_stroke_color(lerp_color(pal_.ir_cold, pal_.ir_hot, t));
            canvas.stroke_line(x, midY - amp, x, midY + amp);
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

    // ── controls (vertical sliders + bypass toggle) ──
    // Center text within a cell width by estimating glyph advance (~0.52em for
    // Inter) — the canvas has no measure API here, so this keeps labels visually
    // centered without one.
    void centered_text(cv::Canvas& canvas, const std::string& txt, float cx,
                       float baseline, float px) {
        const float w = static_cast<float>(txt.size()) * px * 0.52f;
        canvas.fill_text(txt, cx - w * 0.5f, baseline);
    }

    // Design tokens from the concept (near-monochrome, Leica scientific).
    static cv::Color tk_text()  { return cv::Color::rgba8(237, 239, 243); }  // values
    static cv::Color tk_label() { return cv::Color::rgba8(120, 126, 136); }  // DENSITY/FLOW
    static std::string upper(const std::string& in) {
        std::string o = in;
        for (char& ch : o) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        return o;
    }
    // Right-align text ending at `right` (glyph-advance estimate, ~0.52em).
    void right_text(cv::Canvas& canvas, const std::string& txt, float right,
                    float baseline, float px) {
        canvas.fill_text(txt, right - static_cast<float>(txt.size()) * px * 0.52f, baseline);
    }
    // Draw uppercase text with letter-spacing (the concept's tracked caps), char
    // by char since the canvas has no tracking API. `em` = spacing in ems.
    float tracked_text(cv::Canvas& canvas, const std::string& in, float x,
                       float baseline, float px, float em) {
        const std::string t = upper(in);
        const float gap = px * em;
        for (char ch : t) {
            const char one[2] = {ch, 0};
            canvas.fill_text(one, x, baseline);
            // Per-glyph advance (uppercase Inter, em) so tracking reads even.
            float w;
            switch (ch) {
                case 'I': case 'J': w = 0.30f; break;
                case 'L': w = 0.52f; break;
                case 'S': case 'E': case 'F': case 'P': case 'T': case 'Z': w = 0.60f; break;
                case 'R': case 'B': case 'K': case 'V': case 'A': case 'X': case 'Y': w = 0.66f; break;
                case 'C': case 'D': case 'H': case 'N': case 'U': case 'O':
                case 'Q': case 'G': w = 0.73f; break;
                case 'M': case 'W': w = 0.88f; break;
                case ' ': w = 0.40f; break;
                default:  w = 0.64f; break;
            }
            x += px * w + gap;
        }
        return x;  // end x
    }

    void paint_controls(cv::Canvas& canvas) {
        const float s = scale();
        const auto& c = controls_;
        // No cell — the sliders sit directly on the field (concept style). A soft
        // linear fade at the bottom keeps labels legible over bright modes without
        // the banding a stepped fill would show (esp. in Field mode).
        {
            const float top = c.y - 26 * s, bot = c.bottom();
            const cv::Color gcols[2] = {cv::Color::rgba8(5, 6, 9, 0),
                                        cv::Color::rgba8(4, 5, 8, 224)};
            const float gpos[2] = {0.0f, 1.0f};
            canvas.set_fill_gradient_linear(c.x, top, c.x, bot, gcols, gpos, 2);
            canvas.fill_rect(c.x, top, c.width, bot - top);
            canvas.set_fill_color(cv::Color::rgba8(0, 0, 0, 0));  // clears the gradient
        }

        for (int i = 0; i < 5; ++i) {
            const Slider& sl = sliders_[static_cast<size_t>(i)];
            const auto& t = sl.track;
            const bool active = (active_slider_ == i);

            // Label at the track's left — uppercase, tracked, dim #787E88; value
            // right-aligned at the track's right — bold #EDEFF3. Concept tokens.
            canvas.set_fill_color(tk_label());
            canvas.set_font("Inter", 9.5f * s);
            tracked_text(canvas, sl.label, t.x, t.y - 12 * s, 9.5f * s, 0.26f);
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.*f%s%s", sl.decimals,
                          static_cast<double>(slider_value(i)),
                          sl.unit[0] ? " " : "", sl.unit);
            canvas.set_fill_color(tk_text());
            canvas.set_font("Inter", 14.0f * s);
            right_text(canvas, buf, t.x + t.width, t.y - 11 * s, 14.0f * s);

            const float frac = value_frac(i);
            const float hx = t.x + frac * t.width;
            const float hy = t.y + t.height * 0.5f;
            // Track rgba(220,228,238,.12); fill rgba(236,239,243,.62).
            canvas.set_fill_color(cv::Color::rgba8(220, 228, 238, 31));
            canvas.fill_rounded_rect(t.x, t.y, t.width, t.height, t.height * 0.5f);
            canvas.set_fill_color(cv::Color::rgba8(236, 239, 243, 158));
            canvas.fill_rounded_rect(t.x, t.y, std::max(0.0f, hx - t.x), t.height, t.height * 0.5f);
            // Thumb: 14px #EDEFF3 dot with a 2px dark border in #050607.
            const float hr = (active ? 8.0f : 7.0f) * s;
            canvas.set_fill_color(cv::Color::rgba8(5, 6, 7));
            canvas.fill_circle(hx, hy, hr);
            canvas.set_fill_color(cv::Color::rgba8(237, 239, 243));
            canvas.fill_circle(hx, hy, hr - 2.0f * s);
        }

        // No bottom Engine/Bypass chips: Engine toggles via the top-right chip,
        // and Bypass is provided by the host plugin chrome.
    }

    void paint_chip(cv::Canvas& canvas, const vw::Rect& r, bool on,
                    const std::string& label) {
        const float s = scale();
        canvas.set_fill_color(on ? cv::Color::rgba8(237, 239, 243, 30)
                                 : cv::Color::rgba8(237, 239, 243, 12));
        canvas.fill_rounded_rect(r.x, r.y, r.width, r.height, r.height * 0.5f);
        canvas.set_stroke_color(cv::Color::rgba8(237, 239, 243, on ? 90 : 36));
        canvas.set_line_width(1.0f);
        canvas.stroke_rounded_rect(r.x, r.y, r.width, r.height, r.height * 0.5f);
        canvas.set_fill_color(on ? tk_text() : tk_label());
        canvas.set_font("Inter", 12.0f * s);
        centered_text(canvas, label, r.x + r.width * 0.5f, r.y + r.height * 0.62f, 12.0f * s);
    }

    // ── interaction ──
    void pointer_press(vw::Point p) {
        if (pointer_down_) return;
        if (layout_dirty_) layout();
        pointer_down_ = true;

        if (in_rect(p, load_ir_btn_)) {
            open_ir_chooser();
            return;
        }
        for (int i = 0; i < 3; ++i) {
            if (in_rect(p, tabs_[static_cast<size_t>(i)])) { viz_mode_ = i; return; }
        }
        if (in_rect(p, engine_)) {   // top-right chip toggles CPU/GPU
            toggle_param(kEngine);
            return;
        }
        for (int i = 0; i < 5; ++i) {
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
        const float frac = std::clamp((p.x - t.x) / t.width, 0.0f, 1.0f);
        float v = sl.lo + frac * (sl.hi - sl.lo);
        if (sl.snap_int) v = std::round(v);   // Rooms is a whole-step control
        edit_.set(sl.id, v);
    }

    // Begin/set/finish a single-shot 0<->1 toggle as a proper host gesture so the
    // edit sticks and records in the DAW.
    void toggle_param(pulp::state::ParamID id) {
        const float v = store_.get_value(id) >= 0.5f ? 0.0f : 1.0f;
        pulp::state::ParameterEdit toggle(store_);
        toggle.begin(id);
        toggle.set(id, v);
        toggle.finish();
    }

    // Current display value (in-flight edit while dragging, else the store —
    // which tracks host automation).
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
    pulp::examples::SuperConvolverProcessor& proc_;
    pulp::state::ParameterEdit edit_;
    ScPalette pal_ = make_ink_signal_palette();   // resolved from the Ink & Signal preset

    vw::Rect ir_{}, spectrum_rect_{}, controls_{}, bypass_{}, engine_{}, load_ir_btn_{};
    std::array<Slider, 5> sliders_{{
        {kMix,   "Mix",    0.0f, 100.0f, 0, "%"},
        {kSize,  "Size",   0.05f, 4.0f,  2, "s"},
        {kGain,  "Gain",  -24.0f, 24.0f, 1, "dB"},
        {kRooms, "Rooms",  1.0f, 128.0f, 0, "", true},
        {kFlow,  "Flow",   0.0f, 100.0f, 0, "%"},
    }};
    std::array<float, kSpectrumBins> spec_display_{};
    int active_slider_ = -1;
    bool pointer_down_ = false;
    bool layout_dirty_ = true;
    double field_time_ = 0.0;   // advances per repaint → the field's animation clock
    int viz_mode_ = 0;          // 0 Tracers · 1 Currents · 2 Field
    vw::Rect tabs_[3]{};        // mode-tab hit rects
};

} // namespace pulp::examples
