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
            canvas, 0, 0, W, H, field_time_, field_flow, field_density, overall_energy());
        canvas.set_blend_mode(cv::Canvas::BlendMode::normal);  // ensure chrome is not additive

        canvas.set_fill_color(pal_.text);
        canvas.set_font("Inter", 21.0f * s);
        canvas.fill_text("SuperConvolver", 20 * s, 32 * s);
        // Subtitle carries the live engine status so it's always clear whether
        // the AUDIO is on the GPU (the UI is GPU-rendered either way). When the
        // GPU engine is active it names the backend; otherwise it says CPU.
        std::string audio_status = "Audio: CPU";
        // One coherent snapshot per repaint (single lock) so blocks, misses and
        // cost can't disagree across the line.
        const auto g = proc_.gpu_status();
        if (g.active) {
            audio_status = g.backend.empty() ? "Audio: GPU"
                                             : ("Audio: GPU · " + g.backend);
            // Live proof the GPU is carrying the audio: room count + blocks the
            // GPU worker produced vs blocks it missed (CPU/silence-filled).
            if (g.multi)
                audio_status += " · " + std::to_string(g.rooms) + " rooms";
            audio_status += " · " + std::to_string(g.blocks) + " blocks";
            if (g.misses > 0)
                audio_status += ", " + std::to_string(g.misses) + " misses";
            // Live GPU cost + headroom: the measured average wall-clock per block
            // (round-trip included), and what fraction of this device's real-time
            // budget that uses — so the lower the %, the more rooms the GPU could
            // still take. Shown once the worker has produced its first block.
            if (g.blocks > 0 && g.avg_us > 0.0) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), " · %.0f µs/block (%.0f%% of real-time)",
                              g.avg_us, g.rt_percent);
                audio_status += buf;
            }
        }
        canvas.set_fill_color(pal_.text_dim);
        canvas.set_font("Inter", 12.0f * s);
        canvas.fill_text(audio_status, 20 * s, 50 * s);

        // Source chip — the loaded impulse response, first-class: the one Source
        // the field of rooms blooms from. Name via clean_source_name (pure, no
        // per-frame file I/O); "Synthetic room" for the built-in fallback.
        {
            const std::string p = proc_.ir_path();
            const std::string name = p.empty()
                ? std::string("Synthetic room")
                : pulp::superconvolver::clean_source_name(p);
            const float y = 74 * s;
            canvas.set_fill_color(pal_.accent);
            canvas.fill_circle(25 * s, y - 4 * s, 4 * s);
            canvas.set_fill_color(pal_.text_dim);
            canvas.set_font("Inter", 9.5f * s);
            canvas.fill_text("SOURCE", 38 * s, y - 9 * s);
            canvas.set_fill_color(pal_.text);
            canvas.set_font("Inter", 14.0f * s);
            canvas.fill_text(name, 38 * s, y + 4 * s);
        }

        paint_load_ir(canvas);
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

        // Load-IR button, top-right of the header.
        const float btn_w = 104 * s, btn_h = 26 * s;
        load_ir_btn_ = {W - m - btn_w, 14 * s, btn_w, btn_h};

        // Hero IR waveform (largest), then spectrum, then the control strip.
        const float ir_h   = avail * 0.42f;
        const float spec_h = avail * 0.26f;
        const float ctl_h  = avail - ir_h - spec_h - 2 * (12 * s);

        ir_       = {m, top, W - 2 * m, ir_h};
        spectrum_rect_ = {m, ir_.bottom() + 12 * s, W - 2 * m, spec_h};
        controls_ = {m, spectrum_rect_.bottom() + 12 * s, W - 2 * m, ctl_h};

        // Five equal columns: four sliders (Mix/Size/Gain/Rooms) + a toggle
        // column holding the Engine (CPU/GPU) toggle stacked over Bypass.
        const float cw = controls_.width / 6.0f;
        const float label_h = 20 * s, value_h = 22 * s, pad = 10 * s;
        for (int i = 0; i < 5; ++i) {
            Slider& sl = sliders_[static_cast<size_t>(i)];
            sl.cell = {controls_.x + i * cw, controls_.y, cw, controls_.height};
            const float tw = std::min(16 * s, cw * 0.22f);
            const float tx = sl.cell.x + (cw - tw) * 0.5f;
            const float ty = sl.cell.y + label_h + pad;
            const float th = sl.cell.height - label_h - value_h - 2 * pad;
            sl.track = {tx, ty, tw, std::max(20.0f, th)};
        }
        // Toggle column (the fifth): Engine on top, Bypass below.
        const float bw = std::min(cw - 20 * s, 120 * s);
        const float bh = std::min(controls_.height * 0.34f, 48 * s);
        const float bx = controls_.x + 5 * cw + (cw - bw) * 0.5f;
        const float gap = 12 * s;
        const float stack_h = 2 * bh + gap;
        const float y0 = controls_.y + (controls_.height - stack_h) * 0.5f;
        engine_ = {bx, y0, bw, bh};
        bypass_ = {bx, y0 + bh + gap, bw, bh};
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
    void paint_controls(cv::Canvas& canvas) {
        const float s = scale();
        canvas.set_fill_color(pal_.surface);
        canvas.fill_rounded_rect(controls_.x, controls_.y, controls_.width,
                                 controls_.height, 8 * s);

        for (int i = 0; i < 5; ++i) {
            const Slider& sl = sliders_[static_cast<size_t>(i)];
            const auto& t = sl.track;
            // Label (top of cell), centered.
            canvas.set_fill_color(pal_.text_dim);
            canvas.set_font("Inter", 12.0f * s);
            canvas.fill_text(sl.label, sl.cell.x + (sl.cell.width - 36 * s) * 0.5f,
                             sl.cell.y + 16 * s);

            const float frac = value_frac(i);
            const float handle_y = t.bottom() - frac * t.height;
            // Track + fill below the handle.
            canvas.set_fill_color(pal_.slider_track);
            canvas.fill_rounded_rect(t.x, t.y, t.width, t.height, t.width * 0.5f);
            canvas.set_fill_color(pal_.slider_fill);
            canvas.fill_rounded_rect(t.x, handle_y, t.width, t.bottom() - handle_y,
                                     t.width * 0.5f);
            // Handle.
            const bool active = (active_slider_ == i);
            canvas.set_fill_color(active ? pal_.accent_warm : pal_.accent);
            canvas.fill_circle(t.x + t.width * 0.5f, handle_y, t.width * 0.9f);

            // Value readout (bottom of cell), centered.
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.*f%s%s", sl.decimals,
                          static_cast<double>(slider_value(i)),
                          sl.unit[0] ? " " : "", sl.unit);
            canvas.set_fill_color(pal_.text);
            canvas.set_font("Inter", 13.0f * s);
            canvas.fill_text(buf, sl.cell.x + (sl.cell.width - 44 * s) * 0.5f,
                             sl.cell.bottom() - 8 * s);
        }

        // Engine toggle (CPU / GPU). Lit (accent) when GPU is requested; the
        // subtitle reports whether the GPU is actually carrying the audio.
        const bool gpu_req = store_.get_value(kEngine) >= 0.5f;
        canvas.set_fill_color(gpu_req ? pal_.accent : pal_.elevated);
        canvas.fill_rounded_rect(engine_.x, engine_.y, engine_.width, engine_.height, 8 * s);
        canvas.set_fill_color(gpu_req ? pal_.bg : pal_.text);
        canvas.set_font("Inter", 14.0f * s);
        canvas.fill_text(gpu_req ? "● GPU" : "CPU",
                         engine_.x + 16 * s, engine_.y + engine_.height * 0.62f);

        // Bypass toggle.
        const bool bypassed = store_.get_value(kBypass) >= 0.5f;
        canvas.set_fill_color(bypassed ? pal_.bypass_on : pal_.elevated);
        canvas.fill_rounded_rect(bypass_.x, bypass_.y, bypass_.width, bypass_.height, 8 * s);
        canvas.set_fill_color(bypassed ? pal_.bg : pal_.text);
        canvas.set_font("Inter", 14.0f * s);
        canvas.fill_text(bypassed ? "● BYPASSED" : "BYPASS",
                         bypass_.x + 16 * s, bypass_.y + bypass_.height * 0.62f);
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
        if (in_rect(p, bypass_)) {
            toggle_param(kBypass);
            return;
        }
        if (in_rect(p, engine_)) {
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
        const float frac = std::clamp((t.bottom() - p.y) / t.height, 0.0f, 1.0f);
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
        {kRooms, "Rooms",  1.0f, 256.0f, 0, "", true},
        {kFlow,  "Flow",   0.0f, 100.0f, 0, "%"},
    }};
    std::array<float, kSpectrumBins> spec_display_{};
    int active_slider_ = -1;
    bool pointer_down_ = false;
    bool layout_dirty_ = true;
    double field_time_ = 0.0;   // advances per repaint → the field's animation clock
};

} // namespace pulp::examples
